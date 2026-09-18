// SPDX-License-Identifier: Apache-2.0
#include "PerfService.h"
#include "PerfProfiles.h"
#include <aidl/vendor/marble/power/BnPerfSession.h>
#include <android-base/properties.h>
#include <android/binder_ibinder.h>
#include <android/binder_ibinder_platform.h>
#include <cerrno>
#include <mutex>
#include <utility>

namespace marble::power {
namespace {
// Opt-in until ABI/SELinux/behavior have passed real tests. Do not read client-
// supplied properties or accept unknown resources as an ordinary interaction.
bool enabled() {
    return ::android::base::GetBoolProperty("vendor.powerhal.perf.enabled", false);
}
class PerfSession final : public aidl::vendor::marble::power::BnPerfSession {
  public:
    PerfSession(std::shared_ptr<HintController> controller, uint64_t owner, uid_t uid, pid_t pid)
        : mController(std::move(controller)), mOwner(owner), mUid(uid), mPid(pid) {}
    ~PerfSession() override { mController->closePerfOwner(mOwner); }
    ndk::ScopedAStatus acquire(int32_t handle, int32_t duration,
                              const std::vector<int32_t>& resources, int32_t* result) override {
        *result = -EACCES;
        if (!ownsCall()) return ndk::ScopedAStatus::ok();
        std::lock_guard lock(mMutex);
        if ((*result = sessionErrorLocked()) != 0) return ndk::ScopedAStatus::ok();
        const auto profile = matchPerfProfile(resources);
        if (profile == kGoodixProfile) {
            // Only exact Goodix payloads reach this path. SID and gates are
            // server observations, not new client-controlled AIDL arguments.
            *result = mController->acquireGoodix(mOwner, handle, duration, GoodixAccess{
                    .fingerprintCaller = isGoodixFingerprintSid(AIBinder_getCallingSid()),
                    .enabled = ::android::base::GetBoolProperty(kGoodixEnableProperty, false),
                    .screenOffEnabled = ::android::base::GetBoolProperty(kGoodixScreenOffProperty, false),
            });
        } else if (profile.empty()) {
            *result = -ENOTSUP;
        } else {
            *result = mController->acquirePerf(mOwner, handle, duration, profile);
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus acquireHint(int32_t handle, int32_t duration, int32_t hint,
                                  int32_t type, int32_t* result) override {
        *result = -EACCES;
        if (!ownsCall() || !isChiCameraSid(AIBinder_getCallingSid())) {
            return ndk::ScopedAStatus::ok();
        }
        std::lock_guard lock(mMutex);
        if ((*result = sessionErrorLocked()) != 0) return ndk::ScopedAStatus::ok();
        if (!::android::base::GetBoolProperty(kChiEnableProperty, false)) {
            *result = -ENOTSUP;
        } else if (const auto* profile = matchChiProfile(hint, type, duration)) {
            *result = mController->acquirePerf(mOwner, handle, duration, profile);
        } else {
            *result = -ENOTSUP;
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus release(int32_t handle, int32_t* result) override {
        *result = -EACCES;
        if (!ownsCall()) return ndk::ScopedAStatus::ok();
        std::lock_guard lock(mMutex);
        if ((*result = sessionErrorLocked()) == 0) {
            *result = mController->releasePerf(mOwner, handle);
        }
        return ndk::ScopedAStatus::ok();
    }
  protected:
    ndk::SpAIBinder createBinder() override {
        auto binder = BnPerfSession::createBinder();
        AIBinder_setRequestingSid(binder.get(), true);
        return binder;
    }
  private:
    // Main-gate revocation is terminal, unlike an unsupported profile or a
    // disabled sub-feature. Keep reporting it even after the gate is re-enabled.
    int sessionErrorLocked() {
        if (!mRevoked && !enabled()) {
            mController->closePerfOwner(mOwner);
            mRevoked = true;
        }
        return mRevoked ? -ESTALE : 0;
    }
    bool ownsCall() const {
        return AIBinder_getCallingUid() == mUid && AIBinder_getCallingPid() == mPid;
    }
    std::shared_ptr<HintController> mController;
    const uint64_t mOwner;
    std::mutex mMutex;
    bool mRevoked = false;
    const uid_t mUid;
    const pid_t mPid;
};
}
PerfService::PerfService(std::shared_ptr<HintController> controller)
    : mController(std::move(controller)) {}
ndk::ScopedAStatus PerfService::openSession(
        std::shared_ptr<aidl::vendor::marble::power::IPerfSession>* result) {
    *result = nullptr;
    if (!enabled()) return ndk::ScopedAStatus::fromServiceSpecificError(ENOTSUP);
    const auto owner = mController->openPerfOwner();
    if (owner == 0) return ndk::ScopedAStatus::fromServiceSpecificError(ENOSPC);
    // SELinux restricts discovery/calls to camera/fingerprint HALs; UID/PID bind the
    // returned session so forwarding its Binder cannot transfer lock ownership.
    *result = ndk::SharedRefBase::make<PerfSession>(
        mController, owner, AIBinder_getCallingUid(), AIBinder_getCallingPid());
    return ndk::ScopedAStatus::ok();
}
}
