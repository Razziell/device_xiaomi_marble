/*
 * Copyright (C) 2020 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "Power.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <perfmgr/HintManager.h>
#include <utility>

namespace marble::power {
namespace {
ndk::ScopedAStatus unsupported() {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

// setMode/setBoost are oneway. Unsupported hints are ignored, but failures of
// supported requests (including requested-state persistence) must not disappear
// silently in a successful Binder reply.
void reportFailure(bool success, const std::string& name) {
    if (!success) LOG(WARNING) << "Could not apply/save power hint: " << name;
}
}  // namespace

Power::Power(std::shared_ptr<HintController> controller) : mController(std::move(controller)) {
    for (auto mode : ndk::enum_range<hal::Mode>()) {
        const auto bit = static_cast<int32_t>(mode);
        if (bit >= 0 && bit < 63 && mController->supportsMode(hal::toString(mode))) {
            mSupportInfo.modes |= int64_t{1} << bit;
        }
    }
    for (auto boost : ndk::enum_range<hal::Boost>()) {
        const auto bit = static_cast<int32_t>(boost);
        if (bit >= 0 && bit < 63 && mController->supportsBoost(hal::toString(boost))) {
            mSupportInfo.boosts |= int64_t{1} << bit;
        }
    }
    // The zero-initialized remaining fields advertise no sessions, headroom,
    // composition data, session hints, tags, or modes.
    mSupportInfo.compositionData.maxBatchSize = 1;
}

ndk::ScopedAStatus Power::setMode(hal::Mode type, bool enabled) {
    const auto name = hal::toString(type);
    if (mController->supportsMode(name)) reportFailure(mController->setMode(name, enabled), name);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::isModeSupported(hal::Mode type, bool* result) {
    *result = mController->supportsMode(hal::toString(type));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::setBoost(hal::Boost type, int32_t durationMs) {
    const auto name = hal::toString(type);
    if (mController->supportsBoost(name)) {
        reportFailure(mController->setBoost(name, durationMs), name);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::isBoostSupported(hal::Boost type, bool* result) {
    *result = mController->supportsBoost(hal::toString(type));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::getSupportInfo(hal::SupportInfo* result) {
    *result = mSupportInfo;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::getCpuHeadroom(const hal::CpuHeadroomParams&, hal::CpuHeadroomResult*) {
    return unsupported();
}
ndk::ScopedAStatus Power::getGpuHeadroom(const hal::GpuHeadroomParams&, hal::GpuHeadroomResult*) {
    return unsupported();
}
ndk::ScopedAStatus Power::createHintSession(int32_t, int32_t, const std::vector<int32_t>&,
        int64_t, std::shared_ptr<hal::IPowerHintSession>* result) {
    *result = nullptr;
    return unsupported();
}
ndk::ScopedAStatus Power::createHintSessionWithConfig(int32_t, int32_t,
        const std::vector<int32_t>&, int64_t, hal::SessionTag, hal::SessionConfig* config,
        std::shared_ptr<hal::IPowerHintSession>* result) {
    *config = {};
    *result = nullptr;
    return unsupported();
}
ndk::ScopedAStatus Power::getHintSessionPreferredRate(int64_t* result) {
    *result = 0;
    return unsupported();
}
ndk::ScopedAStatus Power::getSessionChannel(int32_t, int32_t, hal::ChannelConfig* result) {
    *result = {};
    return unsupported();
}
ndk::ScopedAStatus Power::closeSessionChannel(int32_t, int32_t) {
    return unsupported();
}
ndk::ScopedAStatus Power::sendCompositionData(const std::vector<hal::CompositionData>&) {
    return unsupported();
}
ndk::ScopedAStatus Power::sendCompositionUpdate(const hal::CompositionUpdate&) {
    return unsupported();
}

binder_status_t Power::dump(int fd, const char**, uint32_t) {
    const auto* hm = ::android::perfmgr::HintManager::GetInstance();
    const std::string text = mController->dump() +
            "HintManager Running: " + (hm->IsRunning() ? "true\n" : "false\n");
    if (!::android::base::WriteStringToFd(text, fd)) return STATUS_FAILED_TRANSACTION;
    ::android::perfmgr::HintManager::GetInstance()->DumpToFd(fd);
    return STATUS_OK;
}

PowerExt::PowerExt(std::shared_ptr<HintController> controller) : mController(std::move(controller)) {}
ndk::ScopedAStatus PowerExt::setMode(const std::string& mode, bool enabled) {
    if (mController->supportsMode(mode)) reportFailure(mController->setMode(mode, enabled), mode);
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus PowerExt::isModeSupported(const std::string& mode, bool* result) {
    *result = mController->supportsMode(mode);
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus PowerExt::setBoost(const std::string& boost, int32_t durationMs) {
    if (mController->supportsBoost(boost)) {
        reportFailure(mController->setBoost(boost, durationMs), boost);
    }
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus PowerExt::isBoostSupported(const std::string& boost, bool* result) {
    *result = mController->supportsBoost(boost);
    return ndk::ScopedAStatus::ok();
}
}  // namespace marble::power
