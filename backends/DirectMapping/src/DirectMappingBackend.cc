// SPDX-FileCopyrightText: Deutsches Elektronen-Synchrotron DESY, MSK, ChimeraTK Project <chimeratk-support@desy.de>
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "DirectMappingBackend.h"

#include "Exception.h"

#include <sys/mman.h>
#include <sys/stat.h>

#include <fcntl.h>

#include <cassert>
#include <cerrno>
#include <cstring>

namespace ChimeraTK {

  DirectMappingBackend::DirectMappingBackend(
      std::string devicePath, std::string mapFileName, size_t sizeParam, uint64_t baseAddrParam)
  : NumericAddressedBackend(std::move(mapFileName))
  , _devicePath(std::move(devicePath))
  , _sizeParam(sizeParam)
  , _baseAddrParam(baseAddrParam) {}

  DirectMappingBackend::~DirectMappingBackend() {
    DirectMappingBackend::closeImpl();
  }

  boost::shared_ptr<DeviceBackend> DirectMappingBackend::createInstance(
      std::string address, std::map<std::string, std::string> parameters) {
    if(address.empty()) {
      throw ChimeraTK::logic_error("DirectMapping: Device path not specified.");
    }

    size_t sizeParam = 0;
    if(!parameters["size"].empty()) {
      try {
        sizeParam = std::stoull(parameters["size"], nullptr, 0);
      }
      catch(std::exception&) {
        throw ChimeraTK::logic_error(
            "DirectMapping: Invalid size parameter '" + parameters["size"] + "'. Use decimal or 0x-prefixed hex.");
      }
    }

    uint64_t baseAddrParam = 0;
    if(!parameters["base"].empty()) {
      try {
        baseAddrParam = std::stoull(parameters["base"], nullptr, 0);
      }
      catch(std::exception&) {
        throw ChimeraTK::logic_error(
            "DirectMapping: Invalid base parameter '" + parameters["base"] + "'. Use decimal or 0x-prefixed hex.");
      }
    }

    return boost::shared_ptr<DeviceBackend>(
        new DirectMappingBackend(address, parameters["map"], sizeParam, baseAddrParam));
  }

  size_t DirectMappingBackend::discoverSize() {
    struct stat st{};
    if(fstat(_fd, &st) == 0 && st.st_size > 0) {
      return static_cast<size_t>(st.st_size);
    }
    if(_sizeParam > 0) {
      return _sizeParam;
    }
    throw ChimeraTK::logic_error("DirectMapping: size could not be determined for '" + _devicePath +
        "'. Provide a 'size' parameter in the CDD.");
  }

  uint64_t DirectMappingBackend::discoverBaseAddress() {
    return _baseAddrParam;
  }

  void DirectMappingBackend::open() {
    if(_opened) {
      if(isFunctional()) {
        return;
      }
      close();
    }

    _fd = ::open(_devicePath.c_str(), O_RDWR);
    if(_fd < 0) {
      throw ChimeraTK::runtime_error(
          "DirectMapping: Failed to open device file '" + _devicePath + "': " + std::strerror(errno));
    }

    try {
      _memSize = discoverSize();
      _baseAddress = discoverBaseAddress();

      _mem = mmap(nullptr, _memSize, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0);
      if(_mem == MAP_FAILED) {
        _mem = nullptr;
        throw ChimeraTK::runtime_error(
            "DirectMapping: mmap failed for '" + _devicePath + "': " + std::strerror(errno));
      }
    }
    catch(...) {
      closeImpl();
      throw;
    }

    setOpenedAndClearException();
  }

  void DirectMappingBackend::closeImpl() {
    // Not gated on _opened: open() may throw partway through after _fd is set but
    // before setOpenedAndClearException() has run, and the catch handler relies on
    // closeImpl() to release those partially-acquired resources.
    if(_mem != nullptr) {
      munmap(_mem, _memSize);
      _mem = nullptr;
    }
    if(_fd >= 0) {
      ::close(_fd);
      _fd = -1;
    }
    _opened = false;
  }

  bool DirectMappingBackend::barIndexValid(uint64_t bar) {
    (void)bar;
    return true;
  }

  void DirectMappingBackend::read(uint64_t bar, uint64_t address, int32_t* data, size_t sizeInBytes) {
    assert(_opened);
    checkActiveException();

    (void)bar;
    if(address < _baseAddress) {
      throw ChimeraTK::logic_error("DirectMapping: Read address is below the configured base address.");
    }
    address -= _baseAddress;
    if(address + sizeInBytes > _memSize) {
      throw ChimeraTK::logic_error("DirectMapping: Read request exceeds device memory region.");
    }

    ::memcpy(data, static_cast<char*>(_mem) + address, sizeInBytes);
  }

  void DirectMappingBackend::write(uint64_t bar, uint64_t address, int32_t const* data, size_t sizeInBytes) {
    assert(_opened);
    checkActiveException();

    (void)bar;
    if(address < _baseAddress) {
      throw ChimeraTK::logic_error("DirectMapping: Write address is below the configured base address.");
    }
    address -= _baseAddress;
    if(address + sizeInBytes > _memSize) {
      throw ChimeraTK::logic_error("DirectMapping: Write request exceeds device memory region.");
    }

    ::memcpy(static_cast<char*>(_mem) + address, data, sizeInBytes);
  }

  std::string DirectMappingBackend::readDeviceInfo() {
    std::string result = "DirectMapping backend: Device path = " + _devicePath;
    if(_opened) {
      result += ", size = " + std::to_string(_memSize) + " bytes";
    }
    else {
      result += " (device closed)";
    }
    return result;
  }

} // namespace ChimeraTK
