// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string_view>

namespace marble::power {
inline constexpr const char* kChiEnableProperty = "vendor.powerhal.perf.chi.enabled";
inline constexpr int kChiMaxDurationMs = 2000;
inline constexpr const char* kChiOpenProfile = "PERF_CHI_OPEN";
inline constexpr const char* kChiCloseProfile = "PERF_CHI_CLOSE";
inline constexpr const char* kChiSnapshotProfile = "PERF_CHI_SNAPSHOT";

// Exact finite CHI lifecycle subset, NOT generic Qualcomm hint semantics.
// Zero is deliberately rejected: no persistent preview/video lifetime model.
inline const char* matchChiProfile(int32_t hint, int32_t type, int32_t duration) {
    if (type != 0 || duration <= 0 || duration > kChiMaxDurationMs) return nullptr;
    switch (hint) {
        case 0x1337: return kChiOpenProfile;
        case 0x1338: return kChiCloseProfile;
        case 0x1339: return kChiSnapshotProfile;
        default: return nullptr;
    }
}
inline bool isChiProfile(std::string_view profile) {
    return profile == kChiOpenProfile || profile == kChiCloseProfile ||
           profile == kChiSnapshotProfile;
}
// Kernel-provided Binder SID, never a caller-supplied string. Fail closed if
// unavailable; fingerprint/system UID alone is not camera authorization.
inline bool isChiCameraSid(const char* sid) {
    return sid != nullptr && std::string_view(sid) == "u:r:hal_camera_default:s0";
}
}  // namespace marble::power
