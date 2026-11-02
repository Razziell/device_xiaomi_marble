// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>

namespace marble::power {
// Requests extending an active vote by less than 25ms may share that vote.
// Do not update the recorded backend deadline for skipped requests: this bounds
// the omitted tail and ensures a later call at/after expiry starts a new vote.
inline bool shouldEnqueueInteraction(std::chrono::steady_clock::time_point now,
                                     std::chrono::steady_clock::time_point current,
                                     std::chrono::steady_clock::time_point requested) {
    constexpr auto kExtensionStep = std::chrono::milliseconds(25);
    if (requested <= current) return false;
    if (current <= now) return true;
    // Near expiry, always forward an extension rather than letting a short
    // remaining vote stand in for a full interaction request.
    return current - now < kExtensionStep || requested - current >= kExtensionStep;
}
}  // namespace marble::power
