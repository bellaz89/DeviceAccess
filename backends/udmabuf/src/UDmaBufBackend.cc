// SPDX-FileCopyrightText: Deutsches Elektronen-Synchrotron DESY, MSK, ChimeraTK Project <chimeratk-support@desy.de>
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "UDmaBufBackend.h"

#include "Exception.h"

#include <filesystem>

#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

namespace ChimeraTK {

  // BAR 0xff total size
  static constexpr size_t BAR1_SIZE = 0x30;

  // BAR 0xff register offsets
  static constexpr uint64_t REG_SYNC_MODE = 0x00;
  static constexpr uint64_t REG_SYNC_DIR = 0x04;
  static constexpr uint64_t REG_SYNC_OFF_LO = 0x08;
  static constexpr uint64_t REG_SYNC_OFF_HI = 0x0C;
  static constexpr uint64_t REG_SYNC_SIZE_LO = 0x10;
  static constexpr uint64_t REG_SYNC_SIZE_HI = 0x14;
  static constexpr uint64_t REG_SYNC_FOR_CPU = 0x18;
  static constexpr uint64_t REG_SYNC_FOR_DEV = 0x1C;
  static constexpr uint64_t REG_PHYS_ADDR_LO = 0x20;
  static constexpr uint64_t REG_PHYS_ADDR_HI = 0x24;
  static constexpr uint64_t REG_BUF_SIZE_LO = 0x28;
  static constexpr uint64_t REG_BUF_SIZE_HI = 0x2C;

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
    addReg("sync_for_cpu",    REG_SYNC_FOR_CPU, 1, Access::WRITE_ONLY);
    addReg("sync_for_device", REG_SYNC_FOR_DEV, 1, Access::WRITE_ONLY);
    addReg("phys_addr",       REG_PHYS_ADDR_LO, 2, Access::READ_ONLY);
    addReg("size",            REG_BUF_SIZE_LO,  2, Access::READ_ONLY);
  }

  /********************************************************************************************************************/

  boost::shared_ptr<DeviceBackend> UDmaBufBackend::createInstance(
      std::string address, std::map<std::string, std::string> parameters) {
    if(address.empty()) {
      throw ChimeraTK::logic_error("UDmaBuf: Device name not specified.");
    }
    return boost::shared_ptr<DeviceBackend>(new UDmaBufBackend(address, parameters["map"]));
  }

  /********************************************************************************************************************/

  void UDmaBufBackend::open() {
    std::filesystem::path devPath(_devicePath);
    if(std::filesystem::is_symlink(devPath)) {
      devPath = std::filesystem::canonical(devPath);
      _devicePath = devPath.string();
      _sysfsBase = "/sys/class/u-dma-buf/" + devPath.filename().string() + "/";
    }
    DirectMappingBackend::open();
  }

  /********************************************************************************************************************/

  size_t UDmaBufBackend::discoverSize() {
    uint64_t sz = readSysfsUint64("size");
    if(sz == 0) {
      throw ChimeraTK::runtime_error(
          "UDmaBuf: Could not determine buffer size from sysfs for device '" + _devName + "'.");
    }
    return static_cast<size_t>(sz);
  }

  /********************************************************************************************************************/

  uint64_t UDmaBufBackend::readSysfsUint64(const std::string& attr) const {
    std::string path = _sysfsBase + attr;
    std::ifstream f(path);
    if(!f.is_open()) {
      throw ChimeraTK::runtime_error("UDmaBuf: Cannot open sysfs attribute '" + path + "'.");
    }
    uint64_t value = 0;
    f >> std::dec >> value;
    return value;
  }

  /********************************************************************************************************************/

  void UDmaBufBackend::writeSysfsUint64(const std::string& attr, uint64_t value) const {
    std::string path = _sysfsBase + attr;
    std::ofstream f(path);
    if(!f.is_open()) {
      throw ChimeraTK::runtime_error("UDmaBuf: Cannot open sysfs attribute '" + path + "' for writing.");
    }
    f << std::dec << value << "\n";
    if(!f) {
      throw ChimeraTK::runtime_error("UDmaBuf: Write failed for sysfs attribute '" + path + "'.");
    }
  }

  /********************************************************************************************************************/

  bool UDmaBufBackend::barIndexValid(uint64_t bar) {
    return (bar == 0 || bar == 0xff);
  }

  /********************************************************************************************************************/

  void UDmaBufBackend::read(uint64_t bar, uint64_t address, int32_t* data, size_t sizeInBytes) {
    assert(_opened);
    checkActiveException();

    if(bar != 0xff) {
      // Delegate to base class for mmap'd buffer
      DirectMappingBackend::read(bar, address, data, sizeInBytes);
      return;
    }

    // bar == 0xff: sysfs register bank
    if(address + sizeInBytes > BAR1_SIZE) {
      throw ChimeraTK::logic_error("UDmaBuf: BAR 0xff read address out of range.");
    }

    while(sizeInBytes > 0) {
      uint64_t value64 = 0;
      switch(address) {
        case REG_SYNC_MODE:
          *data = static_cast<int32_t>(readSysfsUint64("sync_mode"));
          break;
        case REG_SYNC_DIR:
          *data = static_cast<int32_t>(readSysfsUint64("sync_direction"));
          break;
        case REG_SYNC_OFF_LO:
          value64 = readSysfsUint64("sync_offset");
          *data = static_cast<int32_t>(value64 & 0xFFFFFFFFu);
          break;
        case REG_SYNC_OFF_HI:
          value64 = readSysfsUint64("sync_offset");
          *data = static_cast<int32_t>((value64 >> 32) & 0xFFFFFFFFu);
          break;
        case REG_SYNC_SIZE_LO:
          value64 = readSysfsUint64("sync_size");
          *data = static_cast<int32_t>(value64 & 0xFFFFFFFFu);
          break;
        case REG_SYNC_SIZE_HI:
          value64 = readSysfsUint64("sync_size");
          *data = static_cast<int32_t>((value64 >> 32) & 0xFFFFFFFFu);
          break;
        case REG_SYNC_FOR_CPU:
        case REG_SYNC_FOR_DEV:
          *data = 0; // write-only registers: return 0
          break;
        case REG_PHYS_ADDR_LO:
          value64 = readSysfsUint64("phys_addr");
          *data = static_cast<int32_t>(value64 & 0xFFFFFFFFu);
          break;
        case REG_PHYS_ADDR_HI:
          value64 = readSysfsUint64("phys_addr");
          *data = static_cast<int32_t>((value64 >> 32) & 0xFFFFFFFFu);
          break;
        case REG_BUF_SIZE_LO:
          *data = static_cast<int32_t>(static_cast<uint64_t>(_memSize) & 0xFFFFFFFFu);
          break;
        case REG_BUF_SIZE_HI:
          *data = static_cast<int32_t>(static_cast<uint64_t>(_memSize) >> 32);
          break;
        default:
          throw ChimeraTK::logic_error(
              "UDmaBuf: BAR 0xff read from unknown register offset " + std::to_string(address));
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
      // Delegate to base class for mmap'd buffer
      DirectMappingBackend::write(bar, address, data, sizeInBytes);
      return;
    }

    // bar == 0xff: sysfs register bank
    if(address + sizeInBytes > BAR1_SIZE) {
      throw ChimeraTK::logic_error("UDmaBuf: BAR 0xff write address out of range.");
    }

    while(sizeInBytes > 0) {
      auto u32 = static_cast<uint32_t>(*data);
      switch(address) {
        case REG_SYNC_MODE:
          writeSysfsUint64("sync_mode", u32);
          break;
        case REG_SYNC_DIR:
          writeSysfsUint64("sync_direction", u32);
          break;
        case REG_SYNC_OFF_LO:
          _syncOffsetLo = u32;
          break;
        case REG_SYNC_OFF_HI:
          writeSysfsUint64("sync_offset", (static_cast<uint64_t>(u32) << 32) | _syncOffsetLo);
          break;
        case REG_SYNC_SIZE_LO:
          _syncSizeLo = u32;
          break;
        case REG_SYNC_SIZE_HI:
          writeSysfsUint64("sync_size", (static_cast<uint64_t>(u32) << 32) | _syncSizeLo);
          break;
        case REG_SYNC_FOR_CPU:
          writeSysfsUint64("sync_for_cpu", u32);
          break;
        case REG_SYNC_FOR_DEV:
          writeSysfsUint64("sync_for_device", u32);
          break;
        case REG_PHYS_ADDR_LO:
        case REG_PHYS_ADDR_HI:
        case REG_BUF_SIZE_LO:
        case REG_BUF_SIZE_HI:
          throw ChimeraTK::logic_error(
              "UDmaBuf: BAR 0xff register at offset " + std::to_string(address) + " is read-only.");
        default:
          throw ChimeraTK::logic_error(
              "UDmaBuf: BAR 0xff write to unknown register offset " + std::to_string(address));
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
