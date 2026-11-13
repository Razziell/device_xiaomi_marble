// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <optional>

namespace marble::power {
// SurfaceFlinger asks for the display-update boost on every update that follows
// 80ms without one, so slowly changing content (a blinking cursor, a ticking
// clock, a 10fps animation) asks several times a second. Only a request that
// follows a quiet period is boosted. The period is measured from the previous
// request, boosted or not, so a steady stream never re-arms itself; a rate
// limit measured from the previous boost would still boost it periodically.
// A quiet period of zero is always satisfied, so it boosts every request.
inline bool displayUpdateAfterQuiet(
        std::chrono::steady_clock::time_point now,
        std::optional<std::chrono::steady_clock::time_point> previousRequest,
        std::chrono::milliseconds quiet) {
    return !previousRequest || now - *previousRequest >= quiet;
}
}  // namespace marble::power
