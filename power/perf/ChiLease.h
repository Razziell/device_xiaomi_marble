// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>

namespace marble::power {
// A bounded local alias survives finite vote expiry until explicit release or
// disconnect. Only a new validated caller request can renew it; no heartbeat.
struct ChiLease {
    int remote;
    int hint;
    std::chrono::steady_clock::time_point deadline;

    bool accepts(int requestedHint) const { return hint == requestedHint; }
    int remoteForUpdate(std::chrono::steady_clock::time_point now) const {
        return deadline <= now ? 0 : remote;
    }
};
}  // namespace marble::power
