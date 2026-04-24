// SPDX-FileCopyrightText: Deutsches Elektronen-Synchrotron DESY, MSK, ChimeraTK Project <chimeratk-support@desy.de>
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "NumericAddressedBackend.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace ChimeraTK {

  /**
   * Backend for OpenAMP rpmsg-based register access.
   *
   * The backend uses a mandatory data device and an optional interrupt device.
   * The address part of the URI selects the data device. The optional `irq`
   * parameter selects the interrupt device.
   *
   * CDD format: (rpmsg:rpmsg_data0?map=my.map&irq=rpmsg_irq0)
   *   - address : rpmsg data device path or device name without /dev/ prefix
   *   - map     : register map file
   *   - irq     : optional rpmsg interrupt device path or device name without /dev/ prefix
   */
  class RpmsgBackend : public NumericAddressedBackend {
   public:
    RpmsgBackend(const std::string& dataDeviceName, const std::string& irqDeviceName, const std::string& mapFileName,
        const std::string& dataConsistencyKeyDescriptor);
    ~RpmsgBackend() override;

    static boost::shared_ptr<DeviceBackend> createInstance(
        std::string address, std::map<std::string, std::string> parameters);

    void open() override;
    void closeImpl() override;

    [[nodiscard]] size_t minimumTransferAlignment([[maybe_unused]] uint64_t bar) const override { return 4; }
    [[nodiscard]] bool barIndexValid(uint64_t bar) override;

    void read(uint64_t bar, uint64_t address, int32_t* data, size_t sizeInBytes) override;
    void write(uint64_t bar, uint64_t address, int32_t const* data, size_t sizeInBytes) override;
    std::future<void> activateSubscription(
        uint32_t interruptNumber, boost::shared_ptr<async::DomainImpl<std::nullptr_t>> asyncDomain) override;

    std::string readDeviceInfo() override;

   private:
    std::string _dataDevicePath;
    std::string _irqDevicePath;

    int _dataFd{-1};
    int _irqFd{-1};

    std::mutex _dataMutex;

    std::thread _interruptWaitingThread;
    std::atomic<bool> _stopInterruptLoop{false};
    /// Set by waitForInterruptLoop() just before it returns, so activateSubscription()
    /// can distinguish a still-running thread from one that exited early (e.g. on
    /// runtime_error) and needs to be re-spawned.
    std::atomic<bool> _interruptThreadFinished{false};
    boost::shared_ptr<async::DomainImpl<std::nullptr_t>> _asyncDomain;

    void waitForInterruptLoop(std::promise<void> subscriptionDonePromise);
  };

} // namespace ChimeraTK
