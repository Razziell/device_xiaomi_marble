/*
 * Copyright (C) 2020 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/power/BnPower.h>
#include <aidl/google/hardware/power/extension/pixel/BnPowerExt.h>
#include <memory>

#include "HintController.h"

namespace marble::power {
namespace hal = ::aidl::android::hardware::power;

class Power final : public hal::BnPower {
  public:
    explicit Power(std::shared_ptr<HintController> controller);
    ndk::ScopedAStatus setMode(hal::Mode type, bool enabled) override;
    ndk::ScopedAStatus isModeSupported(hal::Mode type, bool* result) override;
    ndk::ScopedAStatus setBoost(hal::Boost type, int32_t durationMs) override;
    ndk::ScopedAStatus isBoostSupported(hal::Boost type, bool* result) override;
    ndk::ScopedAStatus getSupportInfo(hal::SupportInfo* result) override;
    ndk::ScopedAStatus getCpuHeadroom(const hal::CpuHeadroomParams& params,
                                     hal::CpuHeadroomResult* result) override;
    ndk::ScopedAStatus getGpuHeadroom(const hal::GpuHeadroomParams& params,
                                     hal::GpuHeadroomResult* result) override;
    ndk::ScopedAStatus createHintSession(int32_t tgid, int32_t uid,
            const std::vector<int32_t>& threadIds, int64_t durationNanos,
            std::shared_ptr<hal::IPowerHintSession>* result) override;
    ndk::ScopedAStatus createHintSessionWithConfig(int32_t tgid, int32_t uid,
            const std::vector<int32_t>& threadIds, int64_t durationNanos, hal::SessionTag tag,
            hal::SessionConfig* config, std::shared_ptr<hal::IPowerHintSession>* result) override;
    ndk::ScopedAStatus getHintSessionPreferredRate(int64_t* result) override;
    ndk::ScopedAStatus getSessionChannel(int32_t tgid, int32_t uid,
                                       hal::ChannelConfig* result) override;
    ndk::ScopedAStatus closeSessionChannel(int32_t tgid, int32_t uid) override;
    ndk::ScopedAStatus sendCompositionData(const std::vector<hal::CompositionData>& data) override;
    ndk::ScopedAStatus sendCompositionUpdate(const hal::CompositionUpdate& update) override;
    binder_status_t dump(int fd, const char** args, uint32_t numArgs) override;

  private:
    std::shared_ptr<HintController> mController;
    hal::SupportInfo mSupportInfo{};
};

// Keep the existing Pixel string extension for clients such as Flipendo, but
// route it through exactly the same policy as standard AIDL requests.
class PowerExt final : public ::aidl::google::hardware::power::extension::pixel::BnPowerExt {
  public:
    explicit PowerExt(std::shared_ptr<HintController> controller);
    ndk::ScopedAStatus setMode(const std::string& mode, bool enabled) override;
    ndk::ScopedAStatus isModeSupported(const std::string& mode, bool* result) override;
    ndk::ScopedAStatus setBoost(const std::string& boost, int32_t durationMs) override;
    ndk::ScopedAStatus isBoostSupported(const std::string& boost, bool* result) override;

  private:
    std::shared_ptr<HintController> mController;
};
}  // namespace marble::power
