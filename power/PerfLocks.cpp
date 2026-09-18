// SPDX-License-Identifier: Apache-2.0
#include "HintController.h"
#include "PerfProfiles.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <optional>

namespace marble::power {
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

HintController::~HintController() {
    {
        std::lock_guard lock(mMutex);
        mPerfStop = true;
        mPerfCv.notify_all();
    }
    if (mPerfThread.joinable()) mPerfThread.join();
    // Backend outlives the controller. All vendor requests are finite even if
    // cancellation cannot be queued during shutdown.
    clearPerfLocked();
}

uint64_t HintController::openPerfOwner() {
    std::lock_guard lock(mMutex);
    if (mPerfOwners.size() >= 32 || mNextPerfOwner == 0) return 0;
    const auto id = mNextPerfOwner++;
    mPerfOwners.emplace(id, PerfOwner{});
    return id;
}

void HintController::expirePerfLocked() {
    const auto now = Clock::now();
    std::erase_if(mPerfLocks, [&](const auto& item) { return item.second.deadline <= now; });
}

bool HintController::reconcilePerfLocked() {
    expirePerfLocked();
    const auto now = Clock::now();
    // Do not retry failed cancellations forever after the logical lease ends.
    // A last best-effort EndHint is enough here: every underlying vote also
    // has a finite libperfmgr timeout, including one applied late by its queue.
    for (auto it = mPerfApplied.begin(); it != mPerfApplied.end();) {
        if (it->second <= now) {
            mBackend.end(it->first);
            it = mPerfApplied.erase(it);
        } else {
            ++it;
        }
    }
    std::map<std::string, Clock::time_point> desired;
    for (const auto& [handle, lease] : mPerfLocks) {
        (void)handle;
        auto& deadline = desired[lease.profile];
        deadline = std::max(deadline, lease.deadline);
    }
    bool ok = true;
    for (const auto* profile : kPerfProfiles) {
        auto old = mPerfApplied.find(profile);
        auto wanted = desired.find(profile);
        if (old != mPerfApplied.end() && wanted != desired.end() && old->second == wanted->second) {
            continue;
        }
        // Same-profile extensions update the existing libperfmgr vote without
        // a cancellation/default-write job in between. On enqueue failure keep
        // the old applied deadline so CHI rollback can retain its previous vote.
        // Removal/contraction still needs End: RequestGroup only extends.
        if (old != mPerfApplied.end() &&
            (wanted == desired.end() || wanted->second < old->second)) {
            if (!mBackend.end(profile)) { ok = false; continue; }
            mPerfApplied.erase(old);
        }
        if (wanted != desired.end()) {
            auto remaining = std::chrono::ceil<Ms>(wanted->second - Clock::now());
            if (remaining.count() <= 0) continue;
            if (mBackend.startFor(profile, remaining)) mPerfApplied[profile] = wanted->second;
            else ok = false;
        }
    }
    return ok;
}

bool HintController::clearPerfLocked() {
    mPerfLocks.clear();
    mPerfCv.notify_all();
    return reconcilePerfLocked();
}

bool HintController::cancelGoodixScreenOffLocked() {
    std::erase_if(mPerfLocks, [](const auto& item) {
        return item.second.profile == kGoodixScreenOffProfile;
    });
    mPerfCv.notify_all();
    return reconcilePerfLocked();
}

int HintController::acquireGoodix(uint64_t owner, int handle, int durationMs,
                                  GoodixAccess access) {
    std::lock_guard lock(mMutex);
    const auto reject = [&](const char* reason, int error) {
        ++mGoodixStats[reason];
        ++mPerfRejected;
        return -error;
    };
    if (!access.fingerprintCaller) return reject("sid", EACCES);
    if (!access.enabled) return reject("gate", ENOTSUP);
    if (!mInteractiveKnown) return reject("unknown_state", EAGAIN);
    const bool screenOff = !mRequested.contains("INTERACTIVE");
    if (screenOff) {
        if (!access.screenOffEnabled) return reject("screenoff_gate", ENOTSUP);
        // Requested sustained is deliberately stricter than effective sustained:
        // LOW_POWER must not make this exception eligible by hiding that request.
        if (mRequested.contains("Flipendo") || mRequested.contains("SUSTAINED_PERFORMANCE")) {
            return reject("screenoff_policy", EAGAIN);
        }
        if (durationMs <= 0 || durationMs > kGoodixScreenOffMaxMs) {
            return reject("screenoff_duration", EINVAL);
        }
        if (mGoodixScreenOffUsed) return reject("screenoff_budget", EAGAIN);
    } else if (boostsBlockedLocked()) {
        return reject("interactive_policy", EAGAIN);
    }
    const auto* profile = screenOff ? kGoodixScreenOffProfile : kGoodixProfile;
    const int result = acquirePerfLocked(owner, handle, durationMs, profile, screenOff);
    if (result > 0) ++mGoodixStats[screenOff ? "screenoff_accepted" : "interactive_accepted"];
    else if (result == -ENOENT) ++mGoodixStats["stale_handle"];
    else if (result == -EIO) ++mGoodixStats["backend"];
    else if (result == -EAGAIN) ++mGoodixStats["rate"];
    else ++mGoodixStats["invalid_or_capacity"];
    return result;
}

int HintController::acquirePerf(uint64_t owner, int handle, int durationMs,
                                const std::string& profile) {
    std::lock_guard lock(mMutex);
    return acquirePerfLocked(owner, handle, durationMs, profile);
}

int HintController::acquirePerfLocked(uint64_t owner, int handle, int durationMs,
                                      const std::string& profile, bool allowScreenOff) {
    const auto fail = [&](int error) { ++mPerfRejected; return -error; };
    auto ownerIt = mPerfOwners.find(owner);
    if (ownerIt == mPerfOwners.end()) return fail(EINVAL);
    auto& budget = ownerIt->second;
    const auto now = Clock::now();
    if (now - budget.window >= std::chrono::seconds(1)) {
        budget.window = now;
        budget.requests = 0;
    }
    if (budget.requests >= 64) return fail(EAGAIN);
    ++budget.requests;
    if (std::find(kPerfProfiles.begin(), kPerfProfiles.end(), profile) == kPerfProfiles.end() ||
        !mBackend.supports(profile)) return fail(ENOTSUP);
    if (handle < 0 || durationMs < 0) return fail(EINVAL);
    const bool chi = isChiProfile(profile);
    if (chi && (durationMs == 0 || durationMs > kChiMaxDurationMs)) return fail(EINVAL);
    const bool screenOff = profile == kGoodixScreenOffProfile;
    if (screenOff) {
        // Reserved internal route: generic acquirePerf cannot enable this profile.
        if (!allowScreenOff) return fail(EACCES);
        if (!mInteractiveKnown || mRequested.contains("INTERACTIVE") ||
            mRequested.contains("Flipendo") || mRequested.contains("SUSTAINED_PERFORMANCE")) {
            return fail(EAGAIN);
        }
        if (durationMs <= 0 || durationMs > kGoodixScreenOffMaxMs) return fail(EINVAL);
        if (mGoodixScreenOffUsed) return fail(EAGAIN);
    } else {
        if (!mInteractiveKnown || boostsBlockedLocked()) return fail(EAGAIN);
    }
    expirePerfLocked();
    std::optional<PerfLock> previousChiLease;
    if (handle > 0) {
        auto it = mPerfLocks.find(handle);
        if (it == mPerfLocks.end() || it->second.owner != owner) return fail(ENOENT);
        // Goodix state changes cancel remote leases; never morph a live handle
        // across screen-off, raw or CHI profiles. Known stale aliases retry as new.
        if ((chi || isChiProfile(it->second.profile) || screenOff ||
             it->second.profile == kGoodixScreenOffProfile) && it->second.profile != profile) {
            return fail(EINVAL);
        }
        if (chi) previousChiLease = it->second;
    } else {
        if (mPerfLocks.size() >= 128 ||
            std::count_if(mPerfLocks.begin(), mPerfLocks.end(), [&](const auto& item) {
                return item.second.owner == owner;
            }) >= 16) return fail(ENOSPC);
        if (mNextPerfHandle > std::numeric_limits<int>::max()) return fail(EOVERFLOW);
        handle = static_cast<int>(mNextPerfHandle++);
    }
    // Xiaomi: zero -> 2s, cap 5s. Goodix: default/cap 500ms. CHI: 1..2000ms.
    const int bounded = boundedPerfDuration(durationMs, profile);
    if (screenOff) mGoodixScreenOffUsed = true;
    if (!mPerfThread.joinable()) mPerfThread = std::thread(&HintController::runPerfTimer, this);
    mPerfLocks.insert_or_assign(handle, PerfLock{owner, profile, Clock::now() + Ms(bounded)});
    if (!reconcilePerfLocked()) {
        // Roll back a CHI replacement to its old deadline (never extend it).
        // A failed new request has no lease. Backend recovery is best-effort;
        // finite libperfmgr durations remain the final safety bound.
        mPerfLocks.erase(handle);
        if (previousChiLease) mPerfLocks.emplace(handle, *previousChiLease);
        reconcilePerfLocked();
        mPerfCv.notify_all();
        return fail(EIO);
    }
    ++mPerfAccepted;
    mPerfCv.notify_all();
    return handle;
}

int HintController::releasePerf(uint64_t owner, int handle) {
    std::lock_guard lock(mMutex);
    expirePerfLocked();
    auto it = mPerfLocks.find(handle);
    if (it == mPerfLocks.end() || it->second.owner != owner) return -ENOENT;
    mPerfLocks.erase(it);
    mPerfCv.notify_all();
    return reconcilePerfLocked() ? 0 : -EIO;
}

void HintController::closePerfOwner(uint64_t owner) {
    std::lock_guard lock(mMutex);
    mPerfOwners.erase(owner);
    std::erase_if(mPerfLocks, [&](const auto& item) { return item.second.owner == owner; });
    reconcilePerfLocked();
    mPerfCv.notify_all();
}

void HintController::runPerfTimer() {
    std::unique_lock lock(mMutex);
    while (!mPerfStop) {
        const bool ok = reconcilePerfLocked();
        if (!ok) {
            // Cancellation retries are bounded in rate, while payload votes
            // have their own libperfmgr timeout as a fail-safe.
            mPerfCv.wait_for(lock, Ms(100));
        } else if (mPerfLocks.empty()) {
            mPerfCv.wait(lock);
        } else {
            auto deadline = Clock::time_point::max();
            for (const auto& [handle, lease] : mPerfLocks) {
                (void)handle;
                deadline = std::min(deadline, lease.deadline);
            }
            mPerfCv.wait_until(lock, deadline);
        }
    }
}
}  // namespace marble::power
