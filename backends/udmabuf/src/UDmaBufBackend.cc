// SPDX-FileCopyrightText: Deutsches Elektronen-Synchrotron DESY, MSK, ChimeraTK Project <chimeratk-support@desy.de>
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "UDmaBufBackend.h"

#include "Exception.h"

#include <filesystem>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace ChimeraTK {

  // BAR index for the virtual sysfs control register bank
  static constexpr uint64_t BAR_SYSFS = 0xff;

  // BAR_SYSFS total size
  static constexpr size_t BAR_SYSFS_SIZE = 0x38;

  // BAR 0xff register offsets
  static constexpr uint64_t REG_SYNC_MODE     = 0x00; // 32-bit
  static constexpr uint64_t REG_SYNC_DIR      = 0x04; // 32-bit
  static constexpr uint64_t REG_SYNC_OFF      = 0x08; // 64-bit
  static constexpr uint64_t REG_SYNC_SIZE     = 0x10; // 64-bit
  static constexpr uint64_t REG_SYNC_FOR_CPU  = 0x18; // 32-bit
  static constexpr uint64_t REG_SYNC_FOR_DEV  = 0x1C; // 32-bit
  static constexpr uint64_t REG_PHYS_ADDR     = 0x20; // 64-bit
  static constexpr uint64_t REG_BUF_SIZE      = 0x28; // 64-bit
  static constexpr uint64_t REG_SYNC_ON_READ  = 0x30; // 32-bit
  static constexpr uint64_t REG_SYNC_ON_WRITE = 0x34; // 32-bit

  /********************************************************************************************************************/

  UDmaBufBackend::UDmaBufBackend(std::string devName, std::string mapFileName)
  : DirectMappingBackend("/dev/" + devName, std::move(mapFileName), 0) {
    using Access = NumericAddressedRegisterInfo::Access;

    auto addReg = [this](const std::string& name, uint64_t address, uint32_t width, Access access) {
      uint32_t nBytes = width / 8;
      _registerMap.addRegister(NumericAddressedRegisterInfo(
          RegisterPath("/u-dma-buf") / name, 1, address, nBytes, BAR_SYSFS, width, 0, false, access));
    };

    addReg("sync_mode",       REG_SYNC_MODE,     32, Access::READ_WRITE);
    addReg("sync_direction",  REG_SYNC_DIR,      32, Access::READ_WRITE);
    addReg("sync_offset",     REG_SYNC_OFF,      64, Access::READ_WRITE);
    addReg("sync_size",       REG_SYNC_SIZE,     64, Access::READ_WRITE);
    addReg("sync_for_cpu",    REG_SYNC_FOR_CPU,  32, Access::WRITE_ONLY);
    addReg("sync_for_device", REG_SYNC_FOR_DEV,  32, Access::WRITE_ONLY);
    addReg("phys_addr",       REG_PHYS_ADDR,     64, Access::READ_ONLY);
    addReg("size",            REG_BUF_SIZE,      64, Access::READ_ONLY);
    addReg("sync_on_read",    REG_SYNC_ON_READ,  32, Access::READ_WRITE);
    addReg("sync_on_write",   REG_SYNC_ON_WRITE, 32, Access::READ_WRITE);
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
    // Determine the sysfs path from the device number. stat() follows symlinks automatically,
    // so this works regardless of whether _devicePath is a real node or a udev symlink,
    // and regardless of what name the device was given in /sys/class/.
    struct stat st{};
    if(::stat(_devicePath.c_str(), &st) < 0) {
      throw ChimeraTK::runtime_error(
          "udmabuf: Cannot stat '" + _devicePath + "': " + std::strerror(errno));
    }
    auto charLink = "/sys/dev/char/" + std::to_string(major(st.st_rdev)) + ":" +
                    std::to_string(minor(st.st_rdev));
    _sysfsBase = std::filesystem::canonical(charLink).string() + "/";

    // Inject buffer size from sysfs so DirectMappingBackend::open() picks it up.
    // The physical base address is stored separately; map file offsets are relative to it,
    // so _baseAddrParam stays 0 and DirectMappingBackend performs no address subtraction.
    _sizeParam = static_cast<size_t>(readSysfsUint64("size"));
    _physAddr  = readSysfsUint64("phys_addr");

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

    try {
      _fdSyncMode      = openFd("sync_mode",      O_RDWR);
      _fdSyncDir       = openFd("sync_direction", O_RDWR);
      _fdSyncOffset    = openFd("sync_offset",    O_RDWR);
      _fdSyncSize      = openFd("sync_size",      O_RDWR);
      _fdSyncForCpu    = openFd("sync_for_cpu",   O_WRONLY);
      _fdSyncForDevice = openFd("sync_for_device",O_WRONLY);

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

  namespace {
    void requireSize(size_t actual, size_t expected, uint64_t address, const char* op) {
      if(actual != expected) {
        throw ChimeraTK::logic_error("udmabuf: BAR 0xff " + std::string(op) + " at offset " +
            std::to_string(address) + " requires " + std::to_string(expected) + " bytes, got " +
            std::to_string(actual) + ".");
      }
    }
  } // namespace

  void UDmaBufBackend::read(uint64_t bar, uint64_t address, int32_t* data, size_t sizeInBytes) {
    assert(_opened);
    checkActiveException();

    if(bar != BAR_SYSFS) {
      if(_syncOnRead) {
        writeSysfsUint64(_fdSyncForCpu, 1);
      }
      DirectMappingBackend::read(bar, address, data, sizeInBytes);
      return;
    }

    // bar == 0xff: sysfs register bank
    if(address + sizeInBytes > BAR_SYSFS_SIZE) {
      throw ChimeraTK::logic_error("udmabuf: BAR 0xff read address out of range.");
    }

    switch(address) {
      case REG_SYNC_MODE:
        requireSize(sizeInBytes, 4, address, "read");
        *data = static_cast<int32_t>(readSysfsUint64(_fdSyncMode));
        break;
      case REG_SYNC_DIR:
        requireSize(sizeInBytes, 4, address, "read");
        *data = static_cast<int32_t>(readSysfsUint64(_fdSyncDir));
        break;
      case REG_SYNC_OFF: {
        requireSize(sizeInBytes, 8, address, "read");
        auto syncOffset = readSysfsUint64(_fdSyncOffset);
        data[0] = static_cast<int32_t>(syncOffset & 0xFFFFFFFFu);
        data[1] = static_cast<int32_t>(syncOffset >> 32);
        break;
      }
      case REG_SYNC_SIZE: {
        requireSize(sizeInBytes, 8, address, "read");
        auto syncSize = readSysfsUint64(_fdSyncSize);
        data[0] = static_cast<int32_t>(syncSize & 0xFFFFFFFFu);
        data[1] = static_cast<int32_t>(syncSize >> 32);
        break;
      }
      case REG_SYNC_FOR_CPU:
      case REG_SYNC_FOR_DEV:
        requireSize(sizeInBytes, 4, address, "read");
        *data = 0; // write-only registers: return 0
        break;
      case REG_PHYS_ADDR:
        requireSize(sizeInBytes, 8, address, "read");
        data[0] = static_cast<int32_t>(_physAddr & 0xFFFFFFFFu);
        data[1] = static_cast<int32_t>(_physAddr >> 32);
        break;
      case REG_BUF_SIZE:
        requireSize(sizeInBytes, 8, address, "read");
        data[0] = static_cast<int32_t>(static_cast<uint64_t>(_memSize) & 0xFFFFFFFFu);
        data[1] = static_cast<int32_t>(static_cast<uint64_t>(_memSize) >> 32);
        break;
      case REG_SYNC_ON_READ:
        requireSize(sizeInBytes, 4, address, "read");
        *data = _syncOnRead ? 1 : 0;
        break;
      case REG_SYNC_ON_WRITE:
        requireSize(sizeInBytes, 4, address, "read");
        *data = _syncOnWrite ? 1 : 0;
        break;
      default:
        throw ChimeraTK::logic_error(
            "udmabuf: BAR 0xff read from unknown register offset " + std::to_string(address));
    }
  }

  /********************************************************************************************************************/

  void UDmaBufBackend::write(uint64_t bar, uint64_t address, int32_t const* data, size_t sizeInBytes) {
    assert(_opened);
    checkActiveException();

    if(bar != BAR_SYSFS) {
      DirectMappingBackend::write(bar, address, data, sizeInBytes);
      if(_syncOnWrite) {
        writeSysfsUint64(_fdSyncForDevice, 1);
      }
      return;
    }

    // bar == 0xff: sysfs register bank
    if(address + sizeInBytes > BAR_SYSFS_SIZE) {
      throw ChimeraTK::logic_error("udmabuf: BAR 0xff write address out of range.");
    }

    switch(address) {
      case REG_SYNC_MODE:
        requireSize(sizeInBytes, 4, address, "write");
        writeSysfsUint64(_fdSyncMode, static_cast<uint32_t>(*data));
        break;
      case REG_SYNC_DIR:
        requireSize(sizeInBytes, 4, address, "write");
        writeSysfsUint64(_fdSyncDir, static_cast<uint32_t>(*data));
        break;
      case REG_SYNC_OFF:
        requireSize(sizeInBytes, 8, address, "write");
        writeSysfsUint64(_fdSyncOffset, static_cast<uint64_t>(static_cast<uint32_t>(data[0])) |
                                            (static_cast<uint64_t>(static_cast<uint32_t>(data[1])) << 32));
        break;
      case REG_SYNC_SIZE:
        requireSize(sizeInBytes, 8, address, "write");
        writeSysfsUint64(_fdSyncSize, static_cast<uint64_t>(static_cast<uint32_t>(data[0])) |
                                          (static_cast<uint64_t>(static_cast<uint32_t>(data[1])) << 32));
        break;
      case REG_SYNC_FOR_CPU:
        requireSize(sizeInBytes, 4, address, "write");
        writeSysfsUint64(_fdSyncForCpu, static_cast<uint32_t>(*data));
        break;
      case REG_SYNC_FOR_DEV:
        requireSize(sizeInBytes, 4, address, "write");
        writeSysfsUint64(_fdSyncForDevice, static_cast<uint32_t>(*data));
        break;
      case REG_SYNC_ON_READ:
        requireSize(sizeInBytes, 4, address, "write");
        _syncOnRead = (*data != 0);
        break;
      case REG_SYNC_ON_WRITE:
        requireSize(sizeInBytes, 4, address, "write");
        _syncOnWrite = (*data != 0);
        break;
      case REG_PHYS_ADDR:
      case REG_BUF_SIZE:
        throw ChimeraTK::logic_error(
            "udmabuf: BAR 0xff register at offset " + std::to_string(address) + " is read-only.");
      default:
        throw ChimeraTK::logic_error(
            "udmabuf: BAR 0xff write to unknown register offset " + std::to_string(address));
    }
  }

  /********************************************************************************************************************/

  std::string UDmaBufBackend::readDeviceInfo() {
    if(_opened) {
      return "udmabuf backend: Device = " + _devicePath + ", sysfs = " + _sysfsBase +
             ", size = " + std::to_string(_memSize) + " bytes";
    }
    return "udmabuf backend: Device = " + _devicePath + " (closed)";
  }

} // namespace ChimeraTK
