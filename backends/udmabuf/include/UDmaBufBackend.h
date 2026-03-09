// SPDX-FileCopyrightText: Deutsches Elektronen-Synchrotron DESY, MSK, ChimeraTK Project <chimeratk-support@desy.de>
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "DirectMappingBackend.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace ChimeraTK {

  /**
   * Backend for Linux u-dma-buf devices.
   *
   * CDD format: (UDmaBuf:udmabuf0?map=mymap.map)
   *
   * BAR 0: mmap'd DMA buffer (read/write via volatile pointer).
   * BAR 0xff: virtual register bank that maps u-dma-buf sysfs control attributes.
   *
   * BAR 0xff register layout (4-byte registers):
   *   0x00  sync_mode        RW
   *   0x04  sync_direction   RW
   *   0x08  sync_offset lo32 RW
   *   0x0C  sync_offset hi32 RW
   *   0x10  sync_size lo32   RW
   *   0x14  sync_size hi32   RW
   *   0x18  sync_for_cpu     WO
   *   0x1C  sync_for_device  WO
   *   0x20  phys_addr lo32   RO
   *   0x24  phys_addr hi32   RO
   *   0x28  buffer size lo32 RO
   *   0x2C  buffer size hi32 RO
   */
  class UDmaBufBackend : public DirectMappingBackend {
    std::string _devName;   // e.g. "udmabuf0"
    std::string _sysfsBase; // "/sys/class/u-dma-buf/udmabuf0/"

    // Cached lo-word for 64-bit split writes (committed on hi-word write)
    uint32_t _syncOffsetLo{0};
    uint32_t _syncSizeLo{0};

    uint64_t readSysfsUint64(const std::string& attr) const;
    void writeSysfsUint64(const std::string& attr, uint64_t value) const;

   public:
    UDmaBufBackend(std::string devName, std::string mapFileName);
    ~UDmaBufBackend() override = default;

    static boost::shared_ptr<DeviceBackend> createInstance(
        std::string address, std::map<std::string, std::string> parameters);

    void open() override;

    bool barIndexValid(uint64_t bar) override;

    void read(uint64_t bar, uint64_t address, int32_t* data, size_t sizeInBytes) override;
    void write(uint64_t bar, uint64_t address, int32_t const* data, size_t sizeInBytes) override;

    std::string readDeviceInfo() override;
  };

} // namespace ChimeraTK
