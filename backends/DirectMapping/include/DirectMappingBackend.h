// SPDX-FileCopyrightText: Deutsches Elektronen-Synchrotron DESY, MSK, ChimeraTK Project <chimeratk-support@desy.de>
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "NumericAddressedBackend.h"

#include <cstddef>
#include <map>
#include <string>

namespace ChimeraTK {

  /**
   * Backend for memory-mapped device files (Linux only).
   *
   * Opens any device file, mmaps it into the process address space, and
   * performs register read/write via memcpy. Map file addresses are treated
   * as absolute system addresses; the base address is subtracted before
   * each access.
   *
   * CDD format: (DirectMapping:/dev/mydev?map=mymap.map&size=0x100000&base=0xA0000000)
   *   - address : device file path (e.g. /dev/mydev)
   *   - map     : register map file (optional)
   *   - size    : buffer size in bytes, hex or decimal (optional; falls back to fstat)
   *   - base    : base physical address to subtract from map file addresses (optional, default 0)
   */
  class DirectMappingBackend : public NumericAddressedBackend {
   protected:
    std::string _devicePath;     ///< Path to the device file
    size_t _sizeParam;           ///< Buffer size from CDD 'size' parameter (0 = not given)
    uint64_t _baseAddrParam;     ///< Base address from CDD 'base' parameter (0 = not given)
    int _fd = -1;                ///< File descriptor for the opened device file
    void* _mem = nullptr;        ///< Pointer to the mmap'd region
    size_t _memSize = 0;         ///< Actual mapped size in bytes
    uint64_t _baseAddress = 0;   ///< Base physical address, determined at open time

    /**
     * Determine the buffer size to mmap. Called from open().
     *
     * Default implementation: tries fstat first, then falls back to _sizeParam.
     * Throws ChimeraTK::logic_error if size cannot be determined.
     * Subclasses may override to use a different discovery mechanism.
     */
    virtual size_t discoverSize();

    /**
     * Determine the base physical address. Called from open().
     *
     * Default implementation returns _baseAddrParam.
     * Subclasses may override to auto-discover the address (e.g. from sysfs).
     */
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
