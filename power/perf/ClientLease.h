// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>

namespace marble::power {
// A client-local alias is not itself a backend vote. Goodix retains its alias
// between finger-down events; its remote 500ms lease may already have expired.
struct ClientLease {
    int remote;
    std::chrono::steady_clock::time_point deadline;
    bool goodix;
    // An EAGAIN renewal may be retried with this alias. If the caller instead
    // opens a fresh Goodix lease after expiry, the rejected alias is reclaimable.
    bool renewalRejected = false;

    bool reclaimOnFreshGoodix(std::chrono::steady_clock::time_point now) const {
        return goodix && renewalRejected && expired(now);
    }
    bool expired(std::chrono::steady_clock::time_point now) const { return deadline <= now; }
    bool retain(std::chrono::steady_clock::time_point now) const { return goodix || !expired(now); }
    int remoteForUpdate(std::chrono::steady_clock::time_point now) const {
        return goodix && expired(now) ? 0 : remote;
    }
    // Never silently turn a camera lease into a fingerprint lease or vice versa.
    bool accepts(bool requestedGoodix) const { return goodix == requestedGoodix; }
};
}  // namespace marble::power
