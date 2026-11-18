/*
 * Copyright (C) 2018, 2020 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "HintController.h"
#include "DisplayUpdatePolicy.h"
#include "InteractionPolicy.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <string_view>
#include <utility>

namespace marble::power {
namespace {
constexpr std::array<std::string_view, 9> kModes = {
        "INTERACTIVE", "LOW_POWER", "SUSTAINED_PERFORMANCE", "FIXED_PERFORMANCE",
        "EXPENSIVE_RENDERING", "LAUNCH", "Flipendo", "GAME", "GAME_LOADING"};
// Finite hints started by this controller and cancelled together whenever a
// mode blocks boosts. They are never persisted or replayed.
constexpr std::array<const char*, 4> kTransientHints = {
        "INTERACTION", "LAUNCH", "GAME_LOADING", kDisplayUpdateHint};

InteractionConfig normalize(InteractionConfig config) {
    // Bound properties too: no malformed override can create an indefinite boost.
    config.maxMs = std::clamp<int64_t>(config.maxMs, 1, 10000);
    config.minMs = std::clamp<int64_t>(config.minMs, 1, config.maxMs);
    config.offsetMs = std::clamp<int64_t>(config.offsetMs, 0, 10000);
    config.wakeBoostMs = std::clamp<int64_t>(config.wakeBoostMs, 0, config.maxMs);
    config.displayUpdateQuietMs = std::clamp<int64_t>(config.displayUpdateQuietMs, 0, 60000);
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
    const bool reconciled = reconcileLocked();
    noteModeFailureLocked(reconciled);
    return reconciled;
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
    if (name == kDisplayUpdateHint) return mConfig.displayUpdateBoost && mBackend.supports(name);
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
    const bool saver = saverLocked();
    // Frequency floors are for a lit screen. A request left enabled across
    // screen-off stays requested (and persisted) and resumes on wake, exactly
    // like a request masked by a saver; an unknown screen state masks nothing.
    const bool screenOff = mInteractiveKnown && !mRequested.contains("INTERACTIVE");
    if (name == "FIXED_PERFORMANCE") {
        return !saver && !screenOff && !mRequested.contains("SUSTAINED_PERFORMANCE");
    }
    // GAME is requested for as long as a game is the foreground app.
    if (name == "SUSTAINED_PERFORMANCE" || name == "EXPENSIVE_RENDERING" || name == "GAME") {
        return !saver && !screenOff;
    }
    return true;
}

bool HintController::saverLocked() const {
    return mRequested.contains("LOW_POWER") || mRequested.contains("Flipendo");
}

bool HintController::boostsBlockedLocked() const {
    return (mInteractiveKnown && !mRequested.contains("INTERACTIVE")) ||
           mRequested.contains("Flipendo") || wantedLocked("SUSTAINED_PERFORMANCE");
}

bool HintController::reconcileLocked() {
    // Retry failed backend writes, never the original screen transition: replaying
    // clearPerfLocked() could drop a new fingerprint lease admitted since then.
    bool ok = true;
    if (mPerfCancelPending) {
        mPerfCancelPending = !reconcilePerfLocked();
        ok = !mPerfCancelPending;
    }
    for (auto it = mPendingEnds.begin(); it != mPendingEnds.end();) {
        if (mBackend.end(*it)) it = mPendingEnds.erase(it);
        else { ++it; ok = false; }
    }
    if (!ok) return false;
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

void HintController::noteModeFailureLocked(bool reconciled) {
    if (reconciled) {
        mModeRetryPending = 0;
        return;
    }
    ++mModeReconcileFailures;
    mModeRetryPending = 8;
    startTimerLocked();
    mPerfCv.notify_all();
}

bool HintController::setMode(const std::string& name, bool enabled) {
    if (!supportsMode(name)) return false;
    std::lock_guard lock(mMutex);
    if (name == "LAUNCH" || name == "GAME_LOADING") {
        if (!enabled) return mBackend.end(name);
        if (boostsBlockedLocked()) return true;
        // The framework drops LAUNCH itself while battery saver is on; a game
        // loading boost gets the same treatment here.
        if (name == "GAME_LOADING" && saverLocked()) return true;
        mTransientMayBeActive = true;
        if (!mBackend.start(name)) return false;
        mPendingEnds.erase(name);  // A fresh accepted vote supersedes an older failed end.
        return true;
    }

    // Only a real screen-off -> screen-on transition: not the first report
    // after boot and not a duplicate callback.
    const bool wake = name == "INTERACTIVE" && enabled && mInteractiveKnown &&
                      !mRequested.contains("INTERACTIVE");
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
    mSaveRetryPending = saved ? 0 : 8;
    if (!saved) {
        startTimerLocked();
        mPerfCv.notify_all();
    }

    // Enqueued before the screen-off fingerprint lease is cancelled below, so
    // the floors both of them request are not dropped in between.
    if (wake) startWakeBoostLocked();
    // A saver that was just enabled ends a running game loading boost; one
    // enabled earlier already keeps it from starting.
    if (enabled && (name == "LOW_POWER" || name == "Flipendo") &&
        mBackend.supports("GAME_LOADING")) mPendingEnds.insert("GAME_LOADING");

    // Remove logical leases once. Reconciliation retries only the backend delta.
    if (boostsBlockedLocked()) {
        mPerfLocks.clear();
        mPerfCancelPending = true;
        if (mTransientMayBeActive) {
            for (const auto* hint : kTransientHints) {
                if (mBackend.supports(hint)) mPendingEnds.insert(hint);
            }
            mTransientMayBeActive = false;
        }
        mInteractionDeadline = {};
    } else if ((name == "INTERACTIVE" && enabled) ||
               mRequested.contains("SUSTAINED_PERFORMANCE")) {
        std::erase_if(mPerfLocks, [](const auto& item) {
            return item.second.profile == kGoodixScreenOffProfile;
        });
        mPerfCancelPending = true;
    }
    if (mPerfCancelPending) mPerfCv.notify_all();
    const bool reconciled = reconcileLocked();
    noteModeFailureLocked(reconciled);
    return reconciled && saved;
}

bool HintController::setBoost(const std::string& name, int32_t durationMs) {
    if (!supportsBoost(name)) return false;
    std::lock_guard lock(mMutex);
    if (mModeRetryPending > 0 && reconcileLocked()) mModeRetryPending = 0;
    if (name == kDisplayUpdateHint) return setDisplayUpdateBoostLocked(durationMs);
    if (durationMs < 0) {
        const bool ok = mBackend.end(name);
        if (ok) mInteractionDeadline = {};
        return ok;
    }
    if (boostsBlockedLocked()) return true;
    const auto duration = std::chrono::milliseconds(interactionDurationMs(durationMs, mConfig));
    const auto now = std::chrono::steady_clock::now();
    const auto deadline = now + duration;
    // Keep the deadline of the last actual enqueue, not skipped extensions.
    // The omitted tail is <25ms; cancellation and near-expiry requests bypass
    // this coalescing. Enqueue timestamps are captured conservatively.
    if (!shouldEnqueueInteraction(now, mInteractionDeadline, deadline)) {
        ++mInteractionCoalesced;
        return true;
    }
    mTransientMayBeActive = true;
    if (!mBackend.startFor(name, duration)) return false;
    mPendingEnds.erase(name);
    ++mInteractionEnqueued;
    mInteractionDeadline = deadline;
    return true;
}

bool HintController::setDisplayUpdateBoostLocked(int32_t durationMs) {
    if (durationMs < 0) return mBackend.end(kDisplayUpdateHint);
    const auto now = std::chrono::steady_clock::now();
    const auto previousRequest = std::exchange(mDisplayUpdateRequest, now);
    // Not a failure for the caller: the request is dropped by policy. Battery
    // saver gets no extra wakeups from display updates.
    if (boostsBlockedLocked() || saverLocked()) {
        ++mDisplayUpdateSkipped;
        return true;
    }
    if (!displayUpdateAfterQuiet(now, previousRequest,
                                 std::chrono::milliseconds(mConfig.displayUpdateQuietMs))) {
        ++mDisplayUpdateQuietSkipped;
        return true;
    }
    // An INTERACTION vote that outlasts this one already requests more.
    if (mInteractionDeadline >= now + kDisplayUpdateBoost) {
        ++mDisplayUpdateSkipped;
        return true;
    }
    mTransientMayBeActive = true;
    // The caller's duration (always 0 from SurfaceFlinger) is not used.
    if (!mBackend.startFor(kDisplayUpdateHint, kDisplayUpdateBoost)) return false;
    mPendingEnds.erase(kDisplayUpdateHint);
    ++mDisplayUpdateEnqueued;
    return true;
}

void HintController::startWakeBoostLocked() {
    if (mConfig.wakeBoostMs <= 0 || !mBackend.supports("INTERACTION")) return;
    if (boostsBlockedLocked()) return;
    const auto duration = std::chrono::milliseconds(mConfig.wakeBoostMs);
    const auto deadline = std::chrono::steady_clock::now() + duration;
    if (deadline <= mInteractionDeadline) return;
    mTransientMayBeActive = true;
    // Best effort: a failed enqueue must not fail the screen-on mode call.
    if (!mBackend.startFor("INTERACTION", duration)) return;
    mPendingEnds.erase("INTERACTION");
    ++mWakeBoosts;
    mInteractionDeadline = deadline;
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
        << "\nInteraction enqueued/coalesced: " << mInteractionEnqueued
        << '/' << mInteractionCoalesced
        << "\nWake boost ms/started: " << mConfig.wakeBoostMs << '/' << mWakeBoosts
        << "\nDisplay-update boost enabled/quiet ms: " << mConfig.displayUpdateBoost << '/'
        << mConfig.displayUpdateQuietMs << ", enqueued/not-quiet/other-skipped: "
        << mDisplayUpdateEnqueued << '/' << mDisplayUpdateQuietSkipped << '/'
        << mDisplayUpdateSkipped
        << "\nRequested-mode save failures: " << mModeSaveFailures
        << " (pending retries: " << mSaveRetryPending << ")"
        << "\nMode reconcile failures/pending retries: " << mModeReconcileFailures
        << '/' << mModeRetryPending
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
