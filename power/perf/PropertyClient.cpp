// SPDX-License-Identifier: Apache-2.0
#include "PerfClientAbi.h"

namespace {
marble::power::PerfPropertyValue defaultProperty(const char* defaultValue) {
    marble::power::PerfPropertyValue result{};
    // Stock returns the caller's default if the perf config store is absent.
    // We do not implement that store. Do not query Android properties instead:
    // similarly named stock config keys need not be system property values.
    if (defaultValue != nullptr) {
        for (std::size_t i = 0; i + 1 < sizeof(result.value) && defaultValue[i] != '\0'; ++i) {
            result.value[i] = defaultValue[i];
        }
    }
    return result;
}
}  // namespace

extern "C" {
marble::power::PerfPropertyValue perf_get_prop(const char*, const char* defaultValue) {
    return defaultProperty(defaultValue);
}

marble::power::PerfPropertyValue perf_wait_get_prop(const char*, const char* defaultValue) {
    // Deliberate non-waiting fallback, not a connection to stock HIDL perfd.
    // No Binder, property reads, heap state or mutex is needed on this path.
    return defaultProperty(defaultValue);
}
}
