/*
 * Copyright (C) 2018, 2020 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device-owned replacement for the Lineage/Pixel Power/InteractionHandler policy.
 * ADPF is intentionally unsupported. Provenance is recorded in the external
 * EVOX_AI/marble-power-hal.md project guide.
 */
#pragma once
#include "GoodixPolicy.h"
#include "RequestedModes.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <thread>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

namespace marble::power {

// The adapter owns no HAL state; unit tests replace it without touching sysfs.
class HintBackend {
  public:
    virtual ~HintBackend() = default;
    virtual bool supports(const std::string& hint) const = 0;
    virtual bool start(const std::string& hint) = 0;
    virtual bool startFor(const std::string& hint, std::chrono::milliseconds duration) = 0;
    virtual bool end(const std::string& hint) = 0;
};

struct InteractionConfig {
    int64_t minMs = 300;
    int64_t maxMs = 1500;
    int64_t offsetMs = 0;
};

class HintController {
  public:
    explicit HintController(HintBackend& backend, InteractionConfig config = {},
                            RequestedModeStore* modeStore = nullptr);
    ~HintController();

    // Private vendor session API, serialized with standard modes and boosts.
    uint64_t openPerfOwner();
    void closePerfOwner(uint64_t owner);
    int acquirePerf(uint64_t owner, int handle, int durationMs, const std::string& profile);
    int acquireGoodix(uint64_t owner, int handle, int durationMs, GoodixAccess access);
    int releasePerf(uint64_t owner, int handle);
    bool supportsMode(const std::string& name) const;
    bool supportsBoost(const std::string& name) const;
    // Startup only, before exposing Binder: restore the complete requested set
    // before reconciliation, without transient hints or off-epoch admission.
    bool restoreModes();
    bool setMode(const std::string& name, bool enabled);
    bool setBoost(const std::string& name, int32_t durationMs);
    std::string dump() const;

    // Negative input is cancellation, never clamped into a positive boost.
    static int64_t interactionDurationMs(int32_t durationMs, InteractionConfig config);

  private:
    bool saveModesLocked();
    bool reconcileLocked();
    bool cancelTransientLocked();
    bool boostsBlockedLocked() const;
    bool wantedLocked(const std::string& name) const;
    bool reconcilePerfLocked();
    void expirePerfLocked();
    void runPerfTimer();
    bool clearPerfLocked();
    bool cancelGoodixScreenOffLocked();
    int acquirePerfLocked(uint64_t owner, int handle, int durationMs,
                          const std::string& profile, bool allowScreenOff = false);

    HintBackend& mBackend;
    const InteractionConfig mConfig;
    RequestedModeStore* const mModeStore;
    std::string mSavedModes;
    uint64_t mModeSaveFailures = 0;
    mutable std::mutex mMutex;
    std::set<std::string> mRequested;
    std::set<std::string> mApplied;
    bool mInteractiveKnown = false;
    std::chrono::steady_clock::time_point mInteractionDeadline{};

    struct PerfLock {
        uint64_t owner;
        std::string profile;
        std::chrono::steady_clock::time_point deadline;
    };
    struct PerfOwner {
        std::chrono::steady_clock::time_point window{};
        unsigned requests = 0;
    };
    std::map<uint64_t, PerfOwner> mPerfOwners;
    std::map<int, PerfLock> mPerfLocks;
    std::map<std::string, std::chrono::steady_clock::time_point> mPerfApplied;
    uint64_t mNextPerfOwner = 1;
    int64_t mNextPerfHandle = 1; // Never reuse handles within one service lifetime.
    std::condition_variable mPerfCv;
    std::thread mPerfThread;
    bool mPerfStop = false;
    uint64_t mPerfAccepted = 0;
    uint64_t mPerfRejected = 0;
    // Global across owners, reset only on a real transition into known screen-off.
    // Consumed before backend enqueue; failed/partial writes do not refund it.
    bool mGoodixScreenOffUsed = true;
    // Keys are fixed internal reason names, never caller-supplied strings.
    std::map<std::string, uint64_t> mGoodixStats;
};

}  // namespace marble::power
