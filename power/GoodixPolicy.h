// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string_view>

namespace marble::power {
inline constexpr const char* kGoodixScreenOffProfile = "PERF_FINGERPRINT_GOODIX_SCREEN_OFF";
inline constexpr const char* kGoodixScreenOffProperty = "vendor.powerhal.perf.goodix.screenoff.enabled";
inline constexpr int kGoodixScreenOffMaxMs = 500;

// Populated by the server, never by a client-controlled AIDL parameter.
// Main perf gate and session UID/PID ownership are checked before this route.
struct GoodixAccess {
    bool fingerprintCaller = false;
    bool enabled = false;
    bool screenOffEnabled = false;
};
inline bool isGoodixFingerprintSid(const char* sid) {
    return sid != nullptr && std::string_view(sid) == "u:r:hal_fingerprint_default:s0";
}
}  // namespace marble::power
