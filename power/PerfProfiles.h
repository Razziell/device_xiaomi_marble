// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ChiProfiles.h"
#include "GoodixPolicy.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace marble::power {
// Exact request signature from MiaPerfLock::AcquirePerfLock (Ghidra).
// This is a device policy translation, NOT an implementation of these opcodes.
// Never ignore unknown pairs or infer cluster order from old Qualcomm headers.
inline constexpr std::array<int32_t, 46> kCameraRequest = {
    0x40400001, 1, 0x40c00000, 3,
    0x40804000, 4095, 0x40800000, 4095, 0x40804100, 4095,
    0x40800100, 4095, 0x40800200, 4095, 0x40804200, 4095,
    0x41004100, 4, 0x41004000, 3, 0x41004200, 1,
    0x4303c000, 2092000, 0x43460000, 1555000,
    0x43484000, 3196000, 0x43488000, 3196000,
    0x4345c000, 600000, 0x4348c000, 1066000, 0x41844000, 1066000,
    0x43458000, 1651200, 0x43480000, 1651200,
    0x43c10000, 4096, 0x43c20000, 2048, 0x43c0c000, 512,
};
// libgf_hal.so irq_process: exact 28-word finger-down request, duration 500ms.
// Ghidra image address 0x00117c1c; no generic opcode semantics are inferred.
inline constexpr std::array<int32_t, 28> kGoodixRequest = {
    0x40c00000, 1, 0x40804000, 4095, 0x40804100, 4095,
    0x40804200, 4095, 0x40800000, 2649, 0x40800100, 4095,
    0x40800200, 4095, 0x41848000, 1066000, 0x41844000, 1066000,
    0x40400000, 1, 0x42c10000, 1, 0x43460000, 3200000,
    0x43458000, 1651000, 0x4281c000, 2000,
};
inline constexpr const char* kGoodixProfile = "PERF_FINGERPRINT_GOODIX";
inline constexpr const char* kGoodixEnableProperty = "vendor.powerhal.perf.goodix.enabled";
inline constexpr std::array<const char*, 7> kPerfProfiles = {
    "PERF_CAMERA", "PERF_CAMERA_SR", kGoodixProfile,
    kChiOpenProfile, kChiCloseProfile, kChiSnapshotProfile, kGoodixScreenOffProfile,
};
inline int boundedPerfDuration(int durationMs, const std::string& profile) {
    if (profile == kGoodixScreenOffProfile) {
        return durationMs < kGoodixScreenOffMaxMs ? durationMs : kGoodixScreenOffMaxMs;
    }
    // CHI callers are validated separately; zero/oversized requests are rejected.
    if (isChiProfile(profile)) return durationMs < kChiMaxDurationMs ? durationMs : kChiMaxDurationMs;
    // Goodix uses a timed, not release-controlled, request. Never copy the
    // blob's aggressive CPU/GPU/resource values directly into kernel nodes.
    const int maximum = profile == kGoodixProfile ? 500 : 5000;
    const int fallback = profile == kGoodixProfile ? 500 : 2000;
    return durationMs == 0 ? fallback : (durationMs < maximum ? durationMs : maximum);
}
inline std::string matchPerfProfile(std::span<const int32_t> resources) {
    if (resources.size() == kGoodixRequest.size()) {
        for (std::size_t i = 0; i < kGoodixRequest.size(); ++i) {
            if (resources[i] != kGoodixRequest[i]) return {};
        }
        return kGoodixProfile;
    }
    if (resources.size() != 46 && resources.size() != 48) return {};
    for (std::size_t i = 0; i < kCameraRequest.size(); ++i) {
        if (resources[i] != kCameraRequest[i]) return {};
    }
    if (resources.size() == 46) return kPerfProfiles[0];
    // MiAlgoAcquirePerfLock(SR) appends GPU_MIN_POWER_LEVEL=3.
    if (resources[46] == 0x42804000 && resources[47] == 3) return kPerfProfiles[1];
    return {};
}
}  // namespace marble::power
