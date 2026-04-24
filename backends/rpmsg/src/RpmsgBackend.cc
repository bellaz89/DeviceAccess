// SPDX-FileCopyrightText: Deutsches Elektronen-Synchrotron DESY, MSK, ChimeraTK Project <chimeratk-support@desy.de>
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "RpmsgBackend.h"

#include "Exception.h"

#include <cassert>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace {

  constexpr size_t kRpmsgInterruptPayloadSizeBytes = 4;
  constexpr uint32_t kWriteFlagMask = 0x80000000U;

  std::string normaliseDevicePath(const std::string& deviceName) {
    if(deviceName.empty()) {
      return {};
    }
    if(deviceName.front() == '/') {
      return deviceName;
    }
    return "/dev/" + deviceName;
  }

  [[noreturn]] void throwErrno(const std::string& message) {
    throw ChimeraTK::runtime_error(message + ": " + std::strerror(errno));
  }

  void writeAll(int fd, const void* buffer, size_t size) {
    auto* ptr = static_cast<const uint8_t*>(buffer);
    while(size > 0) {
      auto ret = ::write(fd, ptr, size);
      if(ret < 0) {
        if(errno == EINTR) {
          continue;
        }
        throwErrno("rpmsg write failed");
      }
      if(ret == 0) {
        throw ChimeraTK::runtime_error("rpmsg write failed: wrote zero bytes");
      }
      ptr += ret;
      size -= static_cast<size_t>(ret);
    }
  }

  void readAll(int fd, void* buffer, size_t size) {
    auto* ptr = static_cast<uint8_t*>(buffer);
    while(size > 0) {
      auto ret = ::read(fd, ptr, size);
      if(ret < 0) {
        if(errno == EINTR) {
          continue;
        }
        throwErrno("rpmsg read failed");
      }
      if(ret == 0) {
        throw ChimeraTK::runtime_error("rpmsg read failed: unexpected end of stream");
      }
      ptr += ret;
      size -= static_cast<size_t>(ret);
    }
  }

  void closeFd(int& fd) {
    if(fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  uint32_t toWordCount(size_t sizeInBytes) {
    if(sizeInBytes % sizeof(uint32_t) != 0) {
      throw ChimeraTK::logic_error(
          "rpmsg backend only supports transfers whose size is a multiple of 4 bytes.");
    }

    auto wordCount = sizeInBytes / sizeof(uint32_t);
    if(wordCount > static_cast<size_t>(std::numeric_limits<uint32_t>::max() & ~kWriteFlagMask)) {
      throw ChimeraTK::logic_error("rpmsg transfer too large.");
    }

    return static_cast<uint32_t>(wordCount);
  }

  uint32_t toDeviceAddress(uint64_t address) {
    if(address > std::numeric_limits<uint32_t>::max()) {
      std::ostringstream error;
      error << "rpmsg backend only supports 32-bit device addresses, got 0x" << std::hex << address << ".";
      throw ChimeraTK::logic_error(error.str());
    }
    return static_cast<uint32_t>(address);
  }

} // namespace

namespace ChimeraTK {

  RpmsgBackend::RpmsgBackend(const std::string& dataDeviceName, const std::string& irqDeviceName,
      const std::string& mapFileName, const std::string& dataConsistencyKeyDescriptor)
  : NumericAddressedBackend(
        mapFileName, std::make_unique<NumericAddressedRegisterCatalogue>(), dataConsistencyKeyDescriptor),
    _dataDevicePath(normaliseDevicePath(dataDeviceName)), _irqDevicePath(normaliseDevicePath(irqDeviceName)) {}

  RpmsgBackend::~RpmsgBackend() {
    RpmsgBackend::closeImpl();
  }

  boost::shared_ptr<DeviceBackend> RpmsgBackend::createInstance(
      std::string address, std::map<std::string, std::string> parameters) {
    if(address.empty()) {
      throw ChimeraTK::logic_error("rpmsg: Data device name not specified.");
    }
    if(parameters["map"].empty()) {
      throw ChimeraTK::logic_error("rpmsg: No map file name given.");
    }

    return boost::make_shared<RpmsgBackend>(address, parameters["irq"], parameters["map"], parameters["DataConsistencyKeys"]);
  }

  void RpmsgBackend::open() {
    if(_opened) {
      if(isFunctional()) {
        return;
      }
      close();
    }

    _dataFd = ::open(_dataDevicePath.c_str(), O_RDWR);
    if(_dataFd < 0) {
      throwErrno("rpmsg: Failed to open data device '" + _dataDevicePath + "'");
    }

    if(!_irqDevicePath.empty()) {
      _irqFd = ::open(_irqDevicePath.c_str(), O_RDONLY);
      if(_irqFd < 0) {
        auto message = "rpmsg: Failed to open interrupt device '" + _irqDevicePath + "'";
        closeFd(_dataFd);
        throwErrno(message);
      }
    }

    setOpenedAndClearException();
  }

  void RpmsgBackend::closeImpl() {
    if(_interruptWaitingThread.joinable()) {
      _stopInterruptLoop = true;
      _interruptWaitingThread.join();
    }
    _interruptThreadFinished = false;

    _asyncDomain.reset();
    closeFd(_irqFd);
    closeFd(_dataFd);
    _opened = false;
  }

  bool RpmsgBackend::barIndexValid(uint64_t bar) {
    return bar == 0;
  }

  void RpmsgBackend::read(uint64_t bar, uint64_t address, int32_t* data, size_t sizeInBytes) {
    assert(_opened);
    checkActiveException();

    if(!barIndexValid(bar)) {
      throw ChimeraTK::logic_error("rpmsg: Only BAR 0 is supported.");
    }

    uint32_t header[2] = {toDeviceAddress(address), toWordCount(sizeInBytes)};

    std::lock_guard<std::mutex> lock(_dataMutex);
    writeAll(_dataFd, header, sizeof(header));
    readAll(_dataFd, data, sizeInBytes);
  }

  void RpmsgBackend::write(uint64_t bar, uint64_t address, int32_t const* data, size_t sizeInBytes) {
    assert(_opened);
    checkActiveException();

    if(!barIndexValid(bar)) {
      throw ChimeraTK::logic_error("rpmsg: Only BAR 0 is supported.");
    }

    auto wordCount = toWordCount(sizeInBytes);
    uint32_t header[2] = {toDeviceAddress(address), wordCount | kWriteFlagMask};

    std::lock_guard<std::mutex> lock(_dataMutex);
    writeAll(_dataFd, header, sizeof(header));
    writeAll(_dataFd, data, sizeInBytes);
  }

  std::future<void> RpmsgBackend::activateSubscription(
      uint32_t interruptNumber, boost::shared_ptr<async::DomainImpl<std::nullptr_t>> asyncDomain) {
    std::promise<void> subscriptionDonePromise;
    auto subscriptionDoneFuture = subscriptionDonePromise.get_future();

    if(_irqDevicePath.empty()) {
      subscriptionDonePromise.set_value();
      return subscriptionDoneFuture;
    }

    if(interruptNumber != 0) {
      setException("rpmsg: Backend only uses interrupt number 0");
      subscriptionDonePromise.set_value();
      return subscriptionDoneFuture;
    }

    // If a previous interrupt thread exited early (e.g. on runtime_error) it stays
    // joinable() until explicitly joined. Harvest it here so the subscription can
    // be restarted instead of silently no-opping.
    if(_interruptWaitingThread.joinable() && _interruptThreadFinished.load()) {
      _interruptWaitingThread.join();
      _interruptThreadFinished = false;
    }

    if(_interruptWaitingThread.joinable()) {
      subscriptionDonePromise.set_value();
      return subscriptionDoneFuture;
    }

    _stopInterruptLoop = false;
    _asyncDomain = asyncDomain;
    _interruptWaitingThread = std::thread(&RpmsgBackend::waitForInterruptLoop, this, std::move(subscriptionDonePromise));
    return subscriptionDoneFuture;
  }

  std::string RpmsgBackend::readDeviceInfo() {
    std::string result = "rpmsg backend: data device = " + _dataDevicePath;
    if(!_irqDevicePath.empty()) {
      result += ", interrupt device = " + _irqDevicePath;
    }
    if(!isOpen()) {
      result += " (device closed)";
    }
    return result;
  }

  void RpmsgBackend::waitForInterruptLoop(std::promise<void> subscriptionDonePromise) {
    subscriptionDonePromise.set_value();

    std::array<uint8_t, kRpmsgInterruptPayloadSizeBytes> buffer{};
    while(!_stopInterruptLoop) {
      try {
        pollfd pfd{};
        pfd.fd = _irqFd;
        pfd.events = POLLIN;

        auto ret = ::poll(&pfd, 1, 100);
        if(ret < 0) {
          if(errno == EINTR) {
            continue;
          }
          throwErrno("rpmsg: Waiting for interrupt message failed");
        }
        if(ret == 0) {
          continue;
        }
        if((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          throw ChimeraTK::runtime_error("rpmsg: Interrupt device became unusable.");
        }

        readAll(_irqFd, buffer.data(), buffer.size());

        if(!isFunctional()) {
          break;
        }

        _asyncDomain->distribute(nullptr);
      }
      catch(ChimeraTK::runtime_error& ex) {
        setException(ex.what());
        break;
      }
    }

    _interruptThreadFinished = true;
  }

} // namespace ChimeraTK
