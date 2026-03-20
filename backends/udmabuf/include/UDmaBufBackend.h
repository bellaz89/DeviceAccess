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
   * In addition to the mmap'd DMA buffer on BAR 0, a virtual register bank on
   * BAR 0xff maps u-dma-buf sysfs control attributes as ChimeraTK registers
   * under the module path "u-dma-buf":
   *
   * | Register               | Offset | Elements | Access |
   * |------------------------|--------|----------|--------|
   * | Register                | Offset | Width | Access |
   * |-------------------------|--------|-------|--------|
   * | u-dma-buf/sync_mode     | 0x00   | 32    | RW     |
   * | u-dma-buf/sync_dir      | 0x04   | 32    | RW     |
   * | u-dma-buf/sync_offset   | 0x08   | 64    | RW     |
   * | u-dma-buf/sync_size     | 0x10   | 64    | RW     |
   * | u-dma-buf/sync_for_cpu  | 0x18   | 32    | WO     |
   * | u-dma-buf/sync_for_dev  | 0x1C   | 32    | WO     |
   * | u-dma-buf/phys_addr     | 0x20   | 64    | RO     |
   * | u-dma-buf/size          | 0x28   | 64    | RO     |
   * | u-dma-buf/sync_on_read  | 0x30   | 32    | RW     |
   * | u-dma-buf/sync_on_write | 0x34   | 32    | RW     |
   *
   * 64-bit attributes (sync_offset, sync_size, phys_addr, size) are native
   * int64_t scalars. The 32-bit buffer passed to read()/write() holds the
   * lo word at data[0] and the hi word at data[1] (little-endian).
   *
   * When sync_on_read is non-zero, every BAR 0 read is preceded by a sync_for_cpu
   * trigger (cache invalidation) so the CPU sees data written by the device.
   * When sync_on_write is non-zero, every BAR 0 write is followed by a
   * sync_for_device trigger (cache flush) so the device sees data written by the CPU.
   *
   * CDD format: (u-dma-buf:udmabuf0?map=mymap.map)
   *   - address : u-dma-buf device name without /dev/ prefix (e.g. udmabuf0),
   *               or a udev symlink name
   *   - map     : register map file (optional)
   */
  class UDmaBufBackend : public DirectMappingBackend {
    std::string _sysfsBase; ///< Base sysfs path, resolved from device number at open() time
    uint64_t _physAddr{0};  ///< Physical base address of the DMA buffer, read from sysfs at open() time

    /// When true, every BAR 0 read is preceded by a sync_for_cpu trigger (cache invalidation)
    bool _syncOnRead{false};
    /// When true, every BAR 0 write is followed by a sync_for_device trigger (cache flush)
    bool _syncOnWrite{false};

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
