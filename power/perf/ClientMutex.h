// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <mutex>

namespace marble::power {
inline constexpr auto kClientMutexWait = std::chrono::milliseconds(10);
inline std::unique_lock<std::timed_mutex> lockClient(std::timed_mutex& mutex) {
    std::unique_lock<std::timed_mutex> lock(mutex, std::defer_lock);
    // Stock also limits acquisition of its client mutex to approximately 10ms.
    // This bounds the requested wait for contention, NOT Binder RPC latency;
    // scheduler delays can make the observed elapsed time longer.
    (void)lock.try_lock_for(kClientMutexWait);  // Caller checks owns_lock().
    return lock;
}
}  // namespace marble::power
