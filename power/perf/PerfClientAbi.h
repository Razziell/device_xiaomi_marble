// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <type_traits>

namespace marble::power {
// Stock perf HAL 2.3 property functions return a 92-byte character aggregate.
// Verified against both ARM64 (hidden result in x8) and ARM32 Thumb (r0)
// binaries. Let the compiler implement the platform's aggregate-return ABI;
// never expose the hidden result pointer as an explicit public parameter.
// Keep this independent of Android PROPERTY_VALUE_MAX and C++ library layouts.
struct PerfPropertyValue {
    char value[92];
};
static_assert(sizeof(PerfPropertyValue) == 92);
static_assert(alignof(PerfPropertyValue) == alignof(char));
static_assert(offsetof(PerfPropertyValue, value) == 0);
static_assert(std::is_trivial_v<PerfPropertyValue>);
static_assert(std::is_standard_layout_v<PerfPropertyValue>);
}  // namespace marble::power

extern "C" {
marble::power::PerfPropertyValue perf_get_prop(const char* name, const char* defaultValue);
marble::power::PerfPropertyValue perf_wait_get_prop(const char* name, const char* defaultValue);
}
