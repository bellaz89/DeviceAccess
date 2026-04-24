// SPDX-FileCopyrightText: Deutsches Elektronen-Synchrotron DESY, MSK, ChimeraTK Project <chimeratk-support@desy.de>
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "UioAccess.h"

#include "Exception.h"

#include <sys/mman.h>

#include <boost/filesystem/directory.hpp>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <format>
#include <fstream>
#include <utility>

namespace ChimeraTK {

  UioAccess::UioMap::UioMap() {}

  UioAccess::UioMap::UioMap(int deviceFileDescriptor, size_t uioMapIdx, const std::string& uioMapPath)
  : _deviceLowerBound(readUint64HexFromFile(std::format("{}/addr", uioMapPath))),
    _deviceHigherBound(_deviceLowerBound + readUint64HexFromFile(std::format("{}/size", uioMapPath))) {
    size_t mapSize = _deviceHigherBound - _deviceLowerBound;

    void* mapped =
        mmap(NULL, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, deviceFileDescriptor, uioMapIdx * getpagesize());

    if(mapped == MAP_FAILED) {
      _deviceLowerBound = 0;
      _deviceHigherBound = 0;
      throw ChimeraTK::runtime_error(std::format("UIO: Cannot allocate memory for UIO map '{}'", uioMapPath));
    }

    _deviceUserBase = mapped;
  }

  UioAccess::UioMap::~UioMap() {
    if(*this) {
      auto mapSize = _deviceHigherBound - _deviceLowerBound;
      munmap(_deviceUserBase, mapSize);
    }
  }

  UioAccess::UioMap::UioMap(UioMap&& other) noexcept
  : _deviceLowerBound(std::exchange(other._deviceLowerBound, 0)),
    _deviceHigherBound(std::exchange(other._deviceHigherBound, 0)),
    _deviceUserBase(std::exchange(other._deviceUserBase, nullptr)) {}

  UioAccess::UioMap& UioAccess::UioMap::operator=(UioMap&& other) noexcept {
    if(this != &other) {
      if(*this) {
        auto mapSize = _deviceHigherBound - _deviceLowerBound;
        munmap(_deviceUserBase, mapSize);
      }

      this->_deviceLowerBound = std::exchange(other._deviceLowerBound, 0);
      this->_deviceHigherBound = std::exchange(other._deviceHigherBound, 0);
      this->_deviceUserBase = std::exchange(other._deviceUserBase, nullptr);
    }
    return *this;
  }

  UioAccess::UioMap::operator bool() const noexcept {
    return _deviceUserBase != nullptr;
  }

  void UioAccess::UioMap::read(uint64_t address, int32_t* data, size_t sizeInBytes) {
    volatile int32_t* rptr = static_cast<volatile int32_t*>(_deviceUserBase) +
        checkMapAddress(address, sizeInBytes, false) / sizeof(int32_t);

    while(sizeInBytes >= sizeof(int32_t)) {
      *(data++) = *(rptr++);
      sizeInBytes -= sizeof(int32_t);
    }
  }

  void UioAccess::UioMap::write(uint64_t address, int32_t const* data, size_t sizeInBytes) {
    volatile int32_t* __restrict__ wptr =
        static_cast<volatile int32_t*>(_deviceUserBase) + checkMapAddress(address, sizeInBytes, true) / sizeof(int32_t);

    while(sizeInBytes >= sizeof(int32_t)) {
      *(wptr++) = *(data++);
      sizeInBytes -= sizeof(int32_t);
    }
  }

  size_t UioAccess::UioMap::checkMapAddress(uint64_t address, size_t sizeInBytes, bool isWrite) {
    if(!*this) [[unlikely]] {
      std::string requestType = isWrite ? "Write" : "Read";
      throw ChimeraTK::logic_error(std::format("UIO: {} request on unmapped memory region", requestType));
    }

    if(address < _deviceLowerBound || address + sizeInBytes > _deviceHigherBound) [[unlikely]] {
      std::string requestType = isWrite ? "Write" : "Read";
      throw ChimeraTK::logic_error(
          std::format("UIO: {} request (low = {}, high = {}) outside device memory region (low = {}, high = {})",
              requestType, address, address + sizeInBytes, _deviceLowerBound, _deviceHigherBound));
    }

    // This is a temporary work around, because register nodes of current map use absolute bus addresses.
    return address - _deviceLowerBound;
  }

  UioAccess::UioAccess(const std::string& deviceFilePath) : _deviceFilePath(deviceFilePath.c_str()) {
    // Pre-discover the number of UIO maps so that mapIndexValid() (called by barIndexValid() during
    // register accessor creation) works correctly before open() is called.
    // The ChimeraTK framework creates register accessors before opening devices - see e.g.
    // ExceptionHandlingDecorator.cc which explicitly notes the device is not yet open.
    discoverMaps();
  }

  void UioAccess::discoverMaps() {
    try {
      if(boost::filesystem::is_symlink(_deviceFilePath)) {
        _deviceFilePath = boost::filesystem::canonical(_deviceFilePath);
      }
    }
    catch(const boost::filesystem::filesystem_error&) {
      return; // Device path not accessible yet; _mapsNumber stays 0.
    }

    _filename = _deviceFilePath.filename().string();
    _mapsNumber = 0;
    while(true) {
      std::string uioMapPath = std::format("/sys/class/uio/{}/maps/map{}", _filename, _mapsNumber);
      if(!boost::filesystem::is_directory(uioMapPath)) {
        break;
      }
      if(_mapsNumber >= MAX_UIO_MAPS) {
        throw ChimeraTK::runtime_error(std::format(
            "UIO: Device '{}' has more than {} UIO maps, which exceeds the supported maximum.", _filename, MAX_UIO_MAPS));
      }
      _mapPaths[_mapsNumber] = std::move(uioMapPath);
      _mapsNumber++;
    }
  }

  UioAccess::~UioAccess() {
    close();
  }

  void UioAccess::open() {
    if(_mapsNumber == 0) {
      // The device was not yet accessible (or had no discoverable maps) at construction
      // time. Retry discovery now so devices that appeared after the backend was
      // instantiated can still be opened successfully.
      discoverMaps();
      if(_mapsNumber == 0) {
        throw ChimeraTK::runtime_error(
            std::format("UIO: No UIO maps found for device '{}'", _deviceFilePath.string()));
      }
    }

    _lastInterruptCount = readUint32FromFile(std::format("/sys/class/uio/{}/event", _filename));

    // Open UIO device file here, so that interrupt thread can run before calling open()
    _deviceFileDescriptor = ::open(_deviceFilePath.c_str(), O_RDWR);
    if(_deviceFileDescriptor < 0) {
      throw ChimeraTK::runtime_error(std::format("UIO: Failed to open device file '{}'", getDeviceFilePath()));
    }

    try {
      for(uint8_t i = 0; i < _mapsNumber; ++i) {
        _maps[i] = UioAccess::UioMap(_deviceFileDescriptor, i, _mapPaths[i]);
      }
    }
    catch(...) {
      for(auto& map : _maps) map = UioMap{};
      ::close(_deviceFileDescriptor);
      throw;
    }

    _opened = true;
  }

  void UioAccess::close() {
    if(_opened) {
      for(auto& map : _maps) map = UioMap{};
      ::close(_deviceFileDescriptor);
      _opened = false;
    }
  }

  bool UioAccess::mapIndexValid(uint64_t map) {
    return map < _mapsNumber;
  }

  void UioAccess::read(uint64_t map, uint64_t address, int32_t* __restrict__ data, size_t sizeInBytes) {
    if(!mapIndexValid(map)) [[unlikely]] {
      throw ChimeraTK::logic_error(
          std::format("UIO: Attempt to access map {} outside the range (registered maps = {})", map, _maps.size()));
    }

    getMap(map).read(address, data, sizeInBytes);
  }

  void UioAccess::write(uint64_t map, uint64_t address, int32_t const* data, size_t sizeInBytes) {
    if(!mapIndexValid(map)) [[unlikely]] {
      throw ChimeraTK::logic_error(
          std::format("UIO: Attempt to access map {} outside the range (registered maps = {})", map, _maps.size()));
    }

    getMap(map).write(address, data, sizeInBytes);
  }

  UioAccess::UioMap& UioAccess::getMap(size_t map) {
    return _maps[map];
  }

  uint32_t UioAccess::waitForInterrupt(int timeoutMs) {
    // Represents the total interrupt count since system uptime.
    uint32_t totalInterruptCount = 0;
    // Will hold the number of new interrupts
    uint32_t occurredInterruptCount = 0;

    struct pollfd pfd;
    pfd.fd = _deviceFileDescriptor;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeoutMs);

    if(ret >= 1) {
      // No timeout, start reading
      ret = ::read(_deviceFileDescriptor, &totalInterruptCount, sizeof(totalInterruptCount));

      if(ret != (ssize_t)sizeof(totalInterruptCount)) {
        throw ChimeraTK::runtime_error(std::format("UIO - Reading interrupt failed: {}", std::strerror(errno)));
      }

      // Prevent overflow of interrupt count value
      occurredInterruptCount = subtractUint32OverflowSafe(totalInterruptCount, _lastInterruptCount);
      _lastInterruptCount = totalInterruptCount;
    }
    else if(ret == 0) {
      // Timeout
      occurredInterruptCount = 0;
    }
    else {
      throw ChimeraTK::runtime_error(std::format("UIO - Waiting for interrupt failed: {}", std::strerror(errno)));
    }
    return occurredInterruptCount;
  }

  void UioAccess::clearInterrupts() {
    uint32_t unmask = 1;
    ssize_t ret = ::write(_deviceFileDescriptor, &unmask, sizeof(unmask));

    if(ret != (ssize_t)sizeof(unmask)) {
      throw ChimeraTK::runtime_error(std::format("UIO - Clearing interrupts failed: {}", std::strerror(errno)));
    }
  }

  std::string UioAccess::getDeviceFilePath() {
    return _deviceFilePath.string();
  }

  uint32_t UioAccess::subtractUint32OverflowSafe(uint32_t minuend, uint32_t subtrahend) {
    // uint32_t arithmetic is defined to wrap modulo 2^32, which gives the correct
    // number of interrupts even when the kernel counter has wrapped around.
    return minuend - subtrahend;
  }

  uint32_t UioAccess::readUint32FromFile(std::string fileName) {
    std::ifstream inputFile(fileName);
    if(!inputFile.is_open()) {
      throw ChimeraTK::runtime_error(std::format("UIO: Cannot open '{}' for reading", fileName));
    }

    uint64_t value = 0;
    if(!(inputFile >> value)) {
      throw ChimeraTK::runtime_error(std::format("UIO: Failed to parse integer from '{}'", fileName));
    }
    return (uint32_t)value;
  }

  uint64_t UioAccess::readUint64HexFromFile(std::string fileName) {
    std::ifstream inputFile(fileName);
    if(!inputFile.is_open()) {
      throw ChimeraTK::runtime_error(std::format("UIO: Cannot open '{}' for reading", fileName));
    }

    uint64_t value = 0;
    if(!(inputFile >> std::hex >> value)) {
      throw ChimeraTK::runtime_error(std::format("UIO: Failed to parse hex integer from '{}'", fileName));
    }
    return value;
  }

} // namespace ChimeraTK
