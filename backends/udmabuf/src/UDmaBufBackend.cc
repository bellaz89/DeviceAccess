// SPDX-FileCopyrightText: Deutsches Elektronen-Synchrotron DESY, MSK, ChimeraTK Project <chimeratk-support@desy.de>
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "UDmaBufBackend.h"

#include "Exception.h"

#include <filesystem>

#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace ChimeraTK {

  // BAR 0xff total size
  static constexpr size_t BAR1_SIZE = 0x38;

  // BAR 0xff register offsets
  static constexpr uint64_t REG_SYNC_MODE     = 0x00;
  static constexpr uint64_t REG_SYNC_DIR      = 0x04;
  static constexpr uint64_t REG_SYNC_OFF_LO   = 0x08;
  static constexpr uint64_t REG_SYNC_OFF_HI   = 0x0C;
  static constexpr uint64_t REG_SYNC_SIZE_LO  = 0x10;
  static constexpr uint64_t REG_SYNC_SIZE_HI  = 0x14;
  static constexpr uint64_t REG_SYNC_FOR_CPU  = 0x18;
  static constexpr uint64_t REG_SYNC_FOR_DEV  = 0x1C;
  static constexpr uint64_t REG_PHYS_ADDR_LO  = 0x20;
  static constexpr uint64_t REG_PHYS_ADDR_HI  = 0x24;
  static constexpr uint64_t REG_BUF_SIZE_LO   = 0x28;
  static constexpr uint64_t REG_BUF_SIZE_HI   = 0x2C;
  static constexpr uint64_t REG_SYNC_ON_READ  = 0x30;
  static constexpr uint64_t REG_SYNC_ON_WRITE = 0x34;

  /********************************************************************************************************************/

  UDmaBufBackend::UDmaBufBackend(std::string devName, std::string mapFileName)
  : DirectMappingBackend("/dev/" + devName, std::move(mapFileName), 0)
  , _devName(std::move(devName))
  , _sysfsBase("/sys/class/u-dma-buf/" + _devName + "/") {
    using Access = NumericAddressedRegisterInfo::Access;

    auto addReg = [this](const std::string& name, uint64_t address, uint32_t nElements, Access access) {
      _registerMap.addRegister(NumericAddressedRegisterInfo(
          RegisterPath("/u-dma-buf") / name, nElements, address, nElements * 4, 0xff, 32, 0, false, access));
    };

    addReg("sync_mode",       REG_SYNC_MODE,    1, Access::READ_WRITE);
    addReg("sync_direction",  REG_SYNC_DIR,     1, Access::READ_WRITE);
    addReg("sync_offset",     REG_SYNC_OFF_LO,  2, Access::READ_WRITE);
    addReg("sync_size",       REG_SYNC_SIZE_LO, 2, Access::READ_WRITE);
    addReg("sync_for_cpu",    REG_SYNC_FOR_CPU,  1, Access::WRITE_ONLY);
    addReg("sync_for_device", REG_SYNC_FOR_DEV,  1, Access::WRITE_ONLY);
    addReg("phys_addr",       REG_PHYS_ADDR_LO,  2, Access::READ_ONLY);
    addReg("size",            REG_BUF_SIZE_LO,   2, Access::READ_ONLY);
    addReg("sync_on_read",    REG_SYNC_ON_READ,  1, Access::READ_WRITE);
    addReg("sync_on_write",   REG_SYNC_ON_WRITE, 1, Access::READ_WRITE);
  }

  /********************************************************************************************************************/

  boost::shared_ptr<DeviceBackend> UDmaBufBackend::createInstance(
      std::string address, std::map<std::string, std::string> parameters) {
    if(address.empty()) {
      throw ChimeraTK::logic_error("udmabuf: Device name not specified.");
    }
    return boost::shared_ptr<DeviceBackend>(new UDmaBufBackend(address, parameters["map"]));
  }

  /********************************************************************************************************************/

  void UDmaBufBackend::open() {
    // Resolve udev symlink to real device name
    std::filesystem::path devPath(_devicePath);
    if(std::filesystem::is_symlink(devPath)) {
      devPath = std::filesystem::canonical(devPath);
      _devicePath = devPath.string();
      _sysfsBase = "/sys/class/u-dma-buf/" + devPath.filename().string() + "/";
    }
    // Inject size and base address from sysfs so DirectMappingBackend::open() picks them up
    _sizeParam = static_cast<size_t>(readSysfsUint64("size"));
    _baseAddrParam = readSysfsUint64("phys_addr");

    // Open persistent file descriptors for RW/WO sysfs attributes
    auto openFd = [&](const std::string& attr, int flags) {
      std::string path = _sysfsBase + attr;
      int fd = ::open(path.c_str(), flags);
      if(fd < 0) {
        throw ChimeraTK::runtime_error(
            "udmabuf: Cannot open sysfs attribute '" + path + "': " + std::strerror(errno));
      }
      return fd;
    };
    _fdSyncMode      = openFd("sync_mode",      O_RDWR);
    _fdSyncDir       = openFd("sync_direction", O_RDWR);
    _fdSyncOffset    = openFd("sync_offset",    O_RDWR);
    _fdSyncSize      = openFd("sync_size",      O_RDWR);
    _fdSyncForCpu    = openFd("sync_for_cpu",   O_WRONLY);
    _fdSyncForDevice = openFd("sync_for_device",O_WRONLY);

    try {
      DirectMappingBackend::open();
    }
    catch(...) {
      closeImpl();
      throw;
    }
  }

  /********************************************************************************************************************/

  void UDmaBufBackend::closeImpl() {
    auto closeFd = [](int& fd) {
      if(fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    };
    closeFd(_fdSyncMode);
    closeFd(_fdSyncDir);
    closeFd(_fdSyncOffset);
    closeFd(_fdSyncSize);
    closeFd(_fdSyncForCpu);
    closeFd(_fdSyncForDevice);
    DirectMappingBackend::closeImpl();
  }

  /********************************************************************************************************************/

  uint64_t UDmaBufBackend::readSysfsUint64(const std::string& attr) const {
    std::string path = _sysfsBase + attr;
    std::ifstream f(path);
    if(!f.is_open()) {
      throw ChimeraTK::runtime_error("udmabuf: Cannot open sysfs attribute '" + path + "'.");
    }
    uint64_t value = 0;
    f >> std::dec >> value;
    return value;
  }

  /********************************************************************************************************************/

  uint64_t UDmaBufBackend::readSysfsUint64(int fd) const {
    char buf[32];
    ssize_t n = ::pread(fd, buf, sizeof(buf) - 1, 0);
    if(n <= 0) {
      throw ChimeraTK::runtime_error(
          "udmabuf: pread from sysfs fd failed: " + std::string(std::strerror(errno)));
    }
    buf[n] = '\0';
    return std::strtoull(buf, nullptr, 10);
  }

  /********************************************************************************************************************/

  void UDmaBufBackend::writeSysfsUint64(int fd, uint64_t value) const {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%llu\n", static_cast<unsigned long long>(value));
    if(::pwrite(fd, buf, static_cast<size_t>(n), 0) != n) {
      throw ChimeraTK::runtime_error(
          "udmabuf: pwrite to sysfs fd failed: " + std::string(std::strerror(errno)));
    }
  }

  /********************************************************************************************************************/

  bool UDmaBufBackend::barIndexValid([[maybe_unused]] uint64_t bar) {
    return true;
  }

  /********************************************************************************************************************/

  void UDmaBufBackend::read(uint64_t bar, uint64_t address, int32_t* data, size_t sizeInBytes) {
    assert(_opened);
    checkActiveException();

    if(bar != 0xff) {
      if(_syncOnRead) {
        writeSysfsUint64(_fdSyncForCpu, 1);
      }
      DirectMappingBackend::read(bar, address, data, sizeInBytes);
      return;
    }

    // bar == 0xff: sysfs register bank
    if(address + sizeInBytes > BAR1_SIZE) {
      throw ChimeraTK::logic_error("udmabuf: BAR 0xff read address out of range.");
    }

    while(sizeInBytes > 0) {
      switch(address) {
        case REG_SYNC_MODE:
          *data = static_cast<int32_t>(readSysfsUint64(_fdSyncMode));
          break;
        case REG_SYNC_DIR:
          *data = static_cast<int32_t>(readSysfsUint64(_fdSyncDir));
          break;
        case REG_SYNC_OFF_LO: {
          auto syncOffset = readSysfsUint64(_fdSyncOffset);
          *data = static_cast<int32_t>(syncOffset & 0xFFFFFFFFu);
          break;
        }
        case REG_SYNC_OFF_HI: {
          auto syncOffset = readSysfsUint64(_fdSyncOffset);
          *data = static_cast<int32_t>((syncOffset >> 32) & 0xFFFFFFFFu);
          break;
        }
        case REG_SYNC_SIZE_LO: {
          auto syncSize = readSysfsUint64(_fdSyncSize);
          *data = static_cast<int32_t>(syncSize & 0xFFFFFFFFu);
          break;
        }
        case REG_SYNC_SIZE_HI: {
          auto syncSize = readSysfsUint64(_fdSyncSize);
          *data = static_cast<int32_t>((syncSize >> 32) & 0xFFFFFFFFu);
          break;
        }
        case REG_SYNC_FOR_CPU:
        case REG_SYNC_FOR_DEV:
          *data = 0; // write-only registers: return 0
          break;
        case REG_PHYS_ADDR_LO:
          *data = static_cast<int32_t>(_baseAddress & 0xFFFFFFFFu);
          break;
        case REG_PHYS_ADDR_HI:
          *data = static_cast<int32_t>(_baseAddress >> 32);
          break;
        case REG_BUF_SIZE_LO:
          *data = static_cast<int32_t>(static_cast<uint64_t>(_memSize) & 0xFFFFFFFFu);
          break;
        case REG_BUF_SIZE_HI:
          *data = static_cast<int32_t>(static_cast<uint64_t>(_memSize) >> 32);
          break;
        case REG_SYNC_ON_READ:
          *data = _syncOnRead ? 1 : 0;
          break;
        case REG_SYNC_ON_WRITE:
          *data = _syncOnWrite ? 1 : 0;
          break;
        default:
          throw ChimeraTK::logic_error(
              "udmabuf: BAR 0xff read from unknown register offset " + std::to_string(address));
      }
      address += 4;
      ++data;
      sizeInBytes -= 4;
    }
  }

  /********************************************************************************************************************/

  void UDmaBufBackend::write(uint64_t bar, uint64_t address, int32_t const* data, size_t sizeInBytes) {
    assert(_opened);
    checkActiveException();

    if(bar != 0xff) {
      DirectMappingBackend::write(bar, address, data, sizeInBytes);
      if(_syncOnWrite) {
        writeSysfsUint64(_fdSyncForDevice, 1);
      }
      return;
    }

    // bar == 0xff: sysfs register bank
    if(address + sizeInBytes > BAR1_SIZE) {
      throw ChimeraTK::logic_error("udmabuf: BAR 0xff write address out of range.");
    }

    while(sizeInBytes > 0) {
      auto u32 = static_cast<uint32_t>(*data);
      switch(address) {
        case REG_SYNC_MODE:
          writeSysfsUint64(_fdSyncMode, u32);
          break;
        case REG_SYNC_DIR:
          writeSysfsUint64(_fdSyncDir, u32);
          break;
        case REG_SYNC_OFF_LO:
          _syncOffsetLo = u32;
          break;
        case REG_SYNC_OFF_HI:
          writeSysfsUint64(_fdSyncOffset, (static_cast<uint64_t>(u32) << 32) | _syncOffsetLo);
          break;
        case REG_SYNC_SIZE_LO:
          _syncSizeLo = u32;
          break;
        case REG_SYNC_SIZE_HI:
          writeSysfsUint64(_fdSyncSize, (static_cast<uint64_t>(u32) << 32) | _syncSizeLo);
          break;
        case REG_SYNC_FOR_CPU:
          writeSysfsUint64(_fdSyncForCpu, u32);
          break;
        case REG_SYNC_FOR_DEV:
          writeSysfsUint64(_fdSyncForDevice, u32);
          break;
        case REG_SYNC_ON_READ:
          _syncOnRead = (u32 != 0);
          break;
        case REG_SYNC_ON_WRITE:
          _syncOnWrite = (u32 != 0);
          break;
        case REG_PHYS_ADDR_LO:
        case REG_PHYS_ADDR_HI:
        case REG_BUF_SIZE_LO:
        case REG_BUF_SIZE_HI:
          throw ChimeraTK::logic_error(
              "udmabuf: BAR 0xff register at offset " + std::to_string(address) + " is read-only.");
        default:
          throw ChimeraTK::logic_error(
              "udmabuf: BAR 0xff write to unknown register offset " + std::to_string(address));
      }
      address += 4;
      ++data;
      sizeInBytes -= 4;
    }
  }

  /********************************************************************************************************************/

  std::string UDmaBufBackend::readDeviceInfo() {
    std::string result = "UDmaBuf backend: Device = " + _devicePath + ", sysfs = " + _sysfsBase;
    if(_opened) {
      result += ", size = " + std::to_string(_memSize) + " bytes";
    }
    else {
      result += " (device closed)";
    }
    return result;
  }

} // namespace ChimeraTK
