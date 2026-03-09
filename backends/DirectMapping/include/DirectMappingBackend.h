// SPDX-FileCopyrightText: Deutsches Elektronen-Synchrotron DESY, MSK, ChimeraTK Project <chimeratk-support@desy.de>
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "NumericAddressedBackend.h"

#include <cstddef>
#include <map>
#include <string>

namespace ChimeraTK {

  class DirectMappingBackend : public NumericAddressedBackend {
   protected:
    std::string _devicePath;
    size_t _sizeParam;      // from CDD parameter (0 = not given)
    uint64_t _baseAddrParam; // from CDD 'base' parameter (0 = not given)
    int _fd = -1;
    void* _mem = nullptr;
    size_t _memSize = 0;
    uint64_t _baseAddress = 0; // determined at open time

    // Hooks for subclasses to override size and base address discovery
    virtual size_t discoverSize();
    virtual uint64_t discoverBaseAddress();

   public:
    DirectMappingBackend(std::string devicePath, std::string mapFileName, size_t sizeParam, uint64_t baseAddrParam = 0);
    ~DirectMappingBackend() override;

    static boost::shared_ptr<DeviceBackend> createInstance(
        std::string address, std::map<std::string, std::string> parameters);

    void open() override;
    void closeImpl() override;

    bool barIndexValid(uint64_t bar) override;

    size_t minimumTransferAlignment([[maybe_unused]] uint64_t bar) const override { return 4; }

    void read(uint64_t bar, uint64_t address, int32_t* data, size_t sizeInBytes) override;
    void write(uint64_t bar, uint64_t address, int32_t const* data, size_t sizeInBytes) override;

    std::string readDeviceInfo() override;
  };

} // namespace ChimeraTK
