/*
 * Copyright (C) 2020 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "Power.h"
#include "PerfService.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_ibinder_platform.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <perfmgr/HintManager.h>
#include <processgroup/processgroup.h>
#include <sched.h>

#include <thread>

namespace {
using ::android::perfmgr::HintManager;

class PropertyModeStore final : public marble::power::RequestedModeStore {
  public:
    std::string load() const override {
        return ::android::base::GetProperty(marble::power::kRequestedModesProperty, "");
    }
    bool save(const std::string& value) override {
        return ::android::base::SetProperty(marble::power::kRequestedModesProperty, value);
    }
};

class LibperfmgrBackend final : public marble::power::HintBackend {
  public:
    explicit LibperfmgrBackend(HintManager& hm) : mHm(hm) {}
    bool supports(const std::string& hint) const override { return mHm.IsHintSupported(hint); }
    bool start(const std::string& hint) override { return mHm.DoHint(hint); }
    bool startFor(const std::string& hint, std::chrono::milliseconds duration) override {
        return mHm.DoHint(hint, duration);
    }
    bool end(const std::string& hint) override { return mHm.EndHint(hint); }

  private:
    HintManager& mHm;
};
}  // namespace

int main() {
    ::android::base::SetDefaultTag("powerhal-marble");
    ::android::base::SetMinimumLogSeverity(::android::base::INFO);
    auto* hm = HintManager::GetInstance();  // Parse once; start after the init gate.
    CHECK(hm != nullptr);
    CHECK(!hm->IsAdpfSupported()) << "Marble HAL does not implement ADPF; reject incompatible JSON";

    if (!SetTaskProfiles(0, {"PreferIdleSet"})) {
        LOG(WARNING) << "PreferIdleSet task profile unavailable";
    }

    LibperfmgrBackend backend(*hm);
    marble::power::InteractionConfig config{
            .minMs = ::android::base::GetIntProperty<int64_t>("vendor.powerhal.interaction.min", 300),
            .maxMs = ::android::base::GetIntProperty<int64_t>("vendor.powerhal.interaction.max", 1500),
            .offsetMs = ::android::base::GetIntProperty<int64_t>("vendor.powerhal.interaction.offset", 0),
    };
    PropertyModeStore modeStore;
    auto controller = std::make_shared<marble::power::HintController>(backend, config, &modeStore);

    // Restore one complete requested-state snapshot, not applied JSON properties
    // which lose masked intent. Boot/zygote resets discard it; transient votes
    // are never restored. Missing/invalid snapshots do not fall back to old props.
    if (!controller->restoreModes()) {
        LOG(WARNING) << "Could not restore requested power modes";
    }

    // Register before waiting: clients and off-mode charging must not deadlock
    // on the post-fs-data gate. libperfmgr queues early requests until Start().
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto power = ndk::SharedRefBase::make<marble::power::Power>(controller);
    auto extension = ndk::SharedRefBase::make<marble::power::PowerExt>(controller);
    const auto binder = power->asBinder();
    const auto extensionBinder = extension->asBinder();
    AIBinder_setMinSchedulerPolicy(binder.get(), SCHED_NORMAL, -20);
    AIBinder_setMinSchedulerPolicy(extensionBinder.get(), SCHED_NORMAL, -20);
    CHECK(AIBinder_setExtension(binder.get(), extensionBinder.get()) == STATUS_OK);
    const std::string instance = std::string(marble::power::hal::IPower::descriptor) + "/default";
    CHECK(AServiceManager_addService(binder.get(), instance.c_str()) == STATUS_OK);

    // A separate vendor-private Binder interface, hosted in the same process.
    // No new init service, no replacement of IPowerExt, no VINTF HAL entry.
    auto perf = ndk::SharedRefBase::make<marble::power::PerfService>(controller);
    const auto perfBinder = perf->asBinder();
    const auto perfStatus = AServiceManager_addService(
            perfBinder.get(), "vendor.marble.power.IPerf/default");
    if (perfStatus != STATUS_OK) {
        // The optional perf bridge must not take down the framework Power HAL
        // on a service-context/SELinux/registration failure. Clients use a
        // non-waiting lookup and report failure if this service is unavailable.
        LOG(ERROR) << "Perf bridge registration failed: " << perfStatus
                   << "; continuing without the vendor perf bridge";
    }

    std::thread([hm] {
        ::android::base::WaitForProperty("vendor.powerhal.init", "1");
        CHECK(hm->Start()) << "Cannot start libperfmgr";
    }).detach();

    LOG(INFO) << "Marble Power HAL AIDL v6 started; timer-only interaction, no ADPF";
    ABinderProcess_joinThreadPool();
    return 1;
}
