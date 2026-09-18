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

    bool expired(std::chrono::steady_clock::time_point now) const { return deadline <= now; }
    bool retain(std::chrono::steady_clock::time_point now) const { return goodix || !expired(now); }
    int remoteForUpdate(std::chrono::steady_clock::time_point now) const {
        return goodix && expired(now) ? 0 : remote;
    }
    // Never silently turn a camera lease into a fingerprint lease or vice versa.
    bool accepts(bool requestedGoodix) const { return goodix == requestedGoodix; }
};
}  // namespace marble::power
