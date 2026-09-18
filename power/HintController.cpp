/*
 * Copyright (C) 2018, 2020 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "HintController.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <string_view>

namespace marble::power {
namespace {
constexpr std::array<std::string_view, 7> kModes = {
        "INTERACTIVE", "LOW_POWER", "SUSTAINED_PERFORMANCE", "FIXED_PERFORMANCE",
        "EXPENSIVE_RENDERING", "LAUNCH", "Flipendo"};

InteractionConfig normalize(InteractionConfig config) {
    // Bound properties too: no malformed override can create an indefinite boost.
    config.maxMs = std::clamp<int64_t>(config.maxMs, 1, 10000);
    config.minMs = std::clamp<int64_t>(config.minMs, 1, config.maxMs);
    config.offsetMs = std::clamp<int64_t>(config.offsetMs, 0, 10000);
    return config;
}
}  // namespace

HintController::HintController(HintBackend& backend, InteractionConfig config,
                               RequestedModeStore* modeStore)
    : mBackend(backend), mConfig(normalize(config)), mModeStore(modeStore) {}

bool HintController::restoreModes() {
    std::lock_guard lock(mMutex);
    if (!mModeStore) return true;
    // Do not use this startup operation to replace live state or refill budgets.
    if (mInteractiveKnown || !mRequested.empty() || !mApplied.empty() || !mPerfOwners.empty()) {
        return false;
    }
    const auto saved = mModeStore->load();
    const auto state = decodeRequestedModes(saved);
    if (!state) return false;
    for (const auto& name : state->modes) {
        if (supportsMode(name)) mRequested.insert(name);
    }
    mInteractiveKnown = state->interactiveKnown;
    mSavedModes = saved;
    // mGoodixScreenOffUsed stays true: a HAL restart is not a new off epoch.
    // The complete set is installed before any enqueue, so masked floors are
    // not briefly applied while restoring saver/noninteractive state.
    return reconcileLocked();
}

bool HintController::saveModesLocked() {
    if (!mModeStore) return true;
    const auto state = encodeRequestedModes(mRequested, mInteractiveKnown);
    if (state == mSavedModes) return true;
    if (!mModeStore->save(state)) {
        ++mModeSaveFailures;
        return false;
    }
    mSavedModes = state;
    return true;
}

bool HintController::supportsMode(const std::string& name) const {
    return std::find(kModes.begin(), kModes.end(), name) != kModes.end() &&
           mBackend.supports(name);
}

bool HintController::supportsBoost(const std::string& name) const {
    // The shipped configuration defines no other standard boost.
    return name == "INTERACTION" && mBackend.supports(name);
}

int64_t HintController::interactionDurationMs(int32_t durationMs, InteractionConfig config) {
    if (durationMs < 0) return -1;
    config = normalize(config);
    return std::clamp<int64_t>(static_cast<int64_t>(durationMs) + config.offsetMs,
                               config.minMs, config.maxMs);
}

bool HintController::wantedLocked(const std::string& name) const {
    if (!mRequested.contains(name)) return false;
    const bool saver = mRequested.contains("LOW_POWER") || mRequested.contains("Flipendo");
    if (name == "FIXED_PERFORMANCE") {
        return !saver && !mRequested.contains("SUSTAINED_PERFORMANCE");
    }
    if (name == "SUSTAINED_PERFORMANCE") return !saver;
    if (name == "EXPENSIVE_RENDERING") {
        return !mRequested.contains("Flipendo") &&
               (!mInteractiveKnown || mRequested.contains("INTERACTIVE"));
    }
    return true;
}

bool HintController::boostsBlockedLocked() const {
    return (mInteractiveKnown && !mRequested.contains("INTERACTIVE")) ||
           mRequested.contains("Flipendo") || wantedLocked("SUSTAINED_PERFORMANCE");
}

bool HintController::cancelTransientLocked() {
    bool ok = clearPerfLocked();
    // Cancel both on mode transitions, even if the timer already expired. Never
    // replay LAUNCH/INTERACTION when a blocking mode is subsequently released.
    for (const auto* name : {"INTERACTION", "LAUNCH"}) {
        if (mBackend.supports(name)) ok = mBackend.end(name) && ok;
    }
    mInteractionDeadline = {};
    return ok;
}

bool HintController::reconcileLocked() {
    bool ok = true;
    // End conflicting floors before applying saver caps. Only persistent mode
    // requests are restored later; timeout-based hints are not persistent modes.
    for (auto it = mApplied.begin(); it != mApplied.end();) {
        if (!wantedLocked(*it)) {
            if (mBackend.end(*it)) {
                it = mApplied.erase(it);
            } else {
                ok = false;
                ++it;
            }
        } else {
            ++it;
        }
    }
    // If enqueueing a cancellation failed, do not enqueue conflicting new modes.
    if (!ok) return false;
    for (const auto& name : mRequested) {
        if (wantedLocked(name) && !mApplied.contains(name)) {
            if (mBackend.start(name)) mApplied.insert(name);
            else ok = false;
        }
    }
    return ok;
}

bool HintController::setMode(const std::string& name, bool enabled) {
    if (!supportsMode(name)) return false;
    std::lock_guard lock(mMutex);
    if (name == "LAUNCH") {
        if (!enabled) return mBackend.end(name);
        if (boostsBlockedLocked()) return true;
        return mBackend.start(name);  // Per-node JSON timeouts; each call renews.
    }

    if (name == "INTERACTIVE") {
        // Duplicate screen-off callbacks must not refill the global duty budget.
        if (!enabled && (!mInteractiveKnown || mRequested.contains("INTERACTIVE"))) {
            mGoodixScreenOffUsed = false;
        }
        mInteractiveKnown = true;
    }
    if (enabled) mRequested.insert(name);
    else mRequested.erase(name);
    // Save requested intent synchronously, not via asynchronous applied-hint
    // jobs. Even a failed enqueue must not erase a masked request on restart.
    // A property failure is reported, but must not prevent safety cancellations.
    const bool saved = saveModesLocked();

    // Never promote/replay a screen-off lease on wake. Requested sustained
    // cancels it even when LOW_POWER masks the effective sustained mode.
    if (((name == "INTERACTIVE" && enabled) ||
         mRequested.contains("SUSTAINED_PERFORMANCE")) &&
        !cancelGoodixScreenOffLocked()) return false;
    // Conservatively keep the existing cancellation on any blocking mode call,
    // including duplicate screen-off. Cancellation does not refund the budget.
    if (boostsBlockedLocked() && !cancelTransientLocked()) return false;
    return reconcileLocked() && saved;
}

bool HintController::setBoost(const std::string& name, int32_t durationMs) {
    if (!supportsBoost(name)) return false;
    std::lock_guard lock(mMutex);
    if (durationMs < 0) {
        const bool ok = mBackend.end(name);
        if (ok) mInteractionDeadline = {};
        return ok;
    }
    if (boostsBlockedLocked()) return true;
    const auto duration = std::chrono::milliseconds(interactionDurationMs(durationMs, mConfig));
    const auto deadline = std::chrono::steady_clock::now() + duration;
    // RequestGroup extends (never shortens) the same hint. Avoid enqueueing a
    // shorter request already covered by the current one. Capture the deadline
    // before enqueue so this estimate cannot outlive the backend's actual timer.
    if (deadline <= mInteractionDeadline) return true;
    if (!mBackend.startFor(name, duration)) return false;
    mInteractionDeadline = deadline;
    return true;
}

std::string HintController::dump() const {
    std::lock_guard lock(mMutex);
    std::ostringstream out;
    out << "Marble Power HAL: timer-only interaction; ADPF/headroom/composition unsupported\n"
        << "Interaction min/max/offset ms: " << mConfig.minMs << '/' << mConfig.maxMs
        << '/' << mConfig.offsetMs << "\nRequested modes:";
    for (const auto& name : mRequested) out << ' ' << name;
    out << "\nEnqueued modes:";
    for (const auto& name : mApplied) out << ' ' << name;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            mInteractionDeadline - std::chrono::steady_clock::now()).count();
    out << "\nInteraction requested remaining ms: " << std::max<int64_t>(0, remaining)
        << "\nRequested-mode save failures: " << mModeSaveFailures
        << "\nBoosts blocked: " << boostsBlockedLocked()
        << "\nPerf owners/locks: " << mPerfOwners.size() << '/' << mPerfLocks.size()
        << "\nPerf accepted/rejected: " << mPerfAccepted << '/' << mPerfRejected
        << "\nGoodix screenoff budget used: " << mGoodixScreenOffUsed
        << "\nGoodix outcomes:";
    for (const auto& [reason, count] : mGoodixStats) out << ' ' << reason << '=' << count;
    out << "\nNote: enqueue success is not confirmation of a kernel write.\n";
    return out.str();
}
}  // namespace marble::power
