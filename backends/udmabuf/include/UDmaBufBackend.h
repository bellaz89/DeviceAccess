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
   * Backend for Linux u-dma-buf devices (Linux only).
   *
   * Derives from DirectMappingBackend. Buffer size and base physical address are
   * auto-discovered from the u-dma-buf sysfs interface at open() time. udev-created
   * symlinks in /dev/ are resolved transparently.
   *
   * In addition to the mmap'd DMA buffer on BAR 0, a virtual register bank on
   * BAR 0xff maps u-dma-buf sysfs control attributes as ChimeraTK registers
   * under the module path "u-dma-buf":
   *
   * | Register               | Offset | Elements | Access |
   * |------------------------|--------|----------|--------|
   * | u-dma-buf/sync_mode    | 0x00   | 1        | RW     |
   * | u-dma-buf/sync_dir     | 0x04   | 1        | RW     |
   * | u-dma-buf/sync_offset  | 0x08   | 2        | RW     |
   * | u-dma-buf/sync_size    | 0x10   | 2        | RW     |
   * | u-dma-buf/sync_for_cpu | 0x18   | 1        | WO     |
   * | u-dma-buf/sync_for_dev | 0x1C   | 1        | WO     |
   * | u-dma-buf/phys_addr    | 0x20   | 2        | RO     |
   * | u-dma-buf/size         | 0x28   | 2        | RO     |
   *
   * 64-bit attributes (sync_offset, sync_size, phys_addr, size) are exposed as
   * 2-element 32-bit arrays (lo word first). On write, the lo word is cached and
   * the sysfs attribute is written atomically when the hi word is received.
   *
   * CDD format: (u-dma-buf:udmabuf0?map=mymap.map)
   *   - address : u-dma-buf device name without /dev/ prefix (e.g. udmabuf0),
   *               or a udev symlink name
   *   - map     : register map file (optional)
   */
  class UDmaBufBackend : public DirectMappingBackend {
    std::string _devName;   ///< Device name as given in the CDD (e.g. "udmabuf0")
    std::string _sysfsBase; ///< Base sysfs path, e.g. "/sys/class/u-dma-buf/udmabuf0/"

    /// Cached lo-word of sync_offset; committed to sysfs when hi-word is written
    uint32_t _syncOffsetLo{0};
    /// Cached lo-word of sync_size; committed to sysfs when hi-word is written
    uint32_t _syncSizeLo{0};

    /**
     * Read a decimal uint64 from the sysfs attribute file at _sysfsBase + attr.
     * Throws ChimeraTK::runtime_error on failure.
     */
    uint64_t readSysfsUint64(const std::string& attr) const;

    /**
     * Write a decimal uint64 to the sysfs attribute file at _sysfsBase + attr.
     * Throws ChimeraTK::runtime_error on failure.
     */
    void writeSysfsUint64(const std::string& attr, uint64_t value) const;

   public:
    UDmaBufBackend(std::string devName, std::string mapFileName);
    ~UDmaBufBackend() override = default;

    static boost::shared_ptr<DeviceBackend> createInstance(
        std::string address, std::map<std::string, std::string> parameters);

    /**
     * Resolves udev symlinks, then injects buffer size and base physical address
     * from sysfs into DirectMappingBackend before delegating to its open().
     */
    void open() override;

    bool barIndexValid(uint64_t bar) override;

    void read(uint64_t bar, uint64_t address, int32_t* data, size_t sizeInBytes) override;
    void write(uint64_t bar, uint64_t address, int32_t const* data, size_t sizeInBytes) override;

    std::string readDeviceInfo() override;
  };

} // namespace ChimeraTK
