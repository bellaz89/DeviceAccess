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
   * Derives from DirectMappingBackend. Buffer size is auto-discovered from the
   * u-dma-buf sysfs interface at open() time. udev-created symlinks in /dev/ are
   * resolved transparently. Map file register addresses are treated as byte offsets
   * relative to the start of the DMA buffer (i.e. relative to its physical base address).
   *
   * DMA buffer data is accessed via pread/pwrite (inherited from DirectMappingBackend),
   * which go through the kernel's VFS read/write path. The u-dma-buf driver handles
   * cache synchronisation internally in its file_operations.read/write, so no explicit
   * sync_for_cpu/sync_for_device calls are needed from userspace.
   *
   * A virtual register bank on BAR 0xff maps u-dma-buf sysfs control attributes as
   * ChimeraTK registers under the module path "udma":
   *
   * | Register              | Offset | Width | Access |
   * |-----------------------|--------|-------|--------|
   * | udma/sync_mode        | 0x00   | 32    | RW     |
   * | udma/sync_dir         | 0x04   | 32    | RW     |
   * | udma/sync_offset      | 0x08   | 64    | RW     |
   * | udma/sync_size        | 0x10   | 64    | RW     |
   * | udma/sync_for_cpu     | 0x18   | 32    | WO     |
   * | udma/sync_for_dev     | 0x1C   | 32    | WO     |
   * | udma/phys_addr        | 0x20   | 64    | RO     |
   * | udma/size             | 0x28   | 64    | RO     |
   *
   * 64-bit attributes (sync_offset, sync_size, phys_addr, size) are native
   * int64_t scalars. The 32-bit buffer passed to read()/write() holds the
   * lo word at data[0] and the hi word at data[1] (little-endian).
   *
   * CDD format: (udma:udmabuf0?map=mymap.map)
   *   - address : u-dma-buf device name without /dev/ prefix (e.g. udmabuf0),
   *               or a udev symlink name
   *   - map     : register map file (optional)
   */
  class UDmaBufBackend : public DirectMappingBackend {
    std::string _sysfsBase; ///< Base sysfs path, resolved from device number at open() time
    uint64_t _physAddr{0};  ///< Physical base address of the DMA buffer, read from sysfs at open() time

    /// Persistent file descriptors for sysfs RW/WO attributes, open for the lifetime of the device connection
    int _fdSyncMode{-1};
    int _fdSyncDir{-1};
    int _fdSyncOffset{-1};
    int _fdSyncSize{-1};
    int _fdSyncForCpu{-1};
    int _fdSyncForDevice{-1};

    /**
     * Read a decimal uint64 from the sysfs attribute file at _sysfsBase + attr.
     * Used only during initialisation. Throws ChimeraTK::runtime_error on failure.
     */
    uint64_t readSysfsUint64(const std::string& attr) const;

    /**
     * Read a decimal uint64 from an already-open sysfs file descriptor via pread.
     * Throws ChimeraTK::runtime_error on failure.
     */
    uint64_t readSysfsUint64(int fd) const;

    /**
     * Write a decimal uint64 to an already-open sysfs file descriptor via pwrite.
     * Throws ChimeraTK::runtime_error on failure.
     */
    void writeSysfsUint64(int fd, uint64_t value) const;

   public:
    UDmaBufBackend(std::string devName, std::string mapFileName);
    ~UDmaBufBackend() override = default;

    static boost::shared_ptr<DeviceBackend> createInstance(
        std::string address, std::map<std::string, std::string> parameters);

    /**
     * Resolves udev symlinks, injects buffer size and base physical address from
     * sysfs into DirectMappingBackend, opens persistent sysfs file descriptors for
     * RW/WO attributes, then delegates to DirectMappingBackend::open().
     */
    void open() override;

    /** Closes persistent sysfs file descriptors then delegates to DirectMappingBackend::closeImpl(). */
    void closeImpl() override;

    bool barIndexValid(uint64_t bar) override;

    void read(uint64_t bar, uint64_t address, int32_t* data, size_t sizeInBytes) override;
    void write(uint64_t bar, uint64_t address, int32_t const* data, size_t sizeInBytes) override;

    std::string readDeviceInfo() override;
  };

} // namespace ChimeraTK
