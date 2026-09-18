// SPDX-License-Identifier: Apache-2.0
// Clean-room, deliberately partial Qualcomm client ABI. No sysfs access here.
#include "../PerfProfiles.h"
#include "ClientLease.h"
#include "ChiLease.h"
#include "ClientMutex.h"
#include <aidl/vendor/marble/power/IPerf.h>
#include <aidl/vendor/marble/power/IPerfSession.h>
#include <android-base/properties.h>
#include <android/binder_manager.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <unistd.h>
#include <vector>

namespace {
using aidl::vendor::marble::power::IPerf;
using aidl::vendor::marble::power::IPerfSession;
constexpr char kService[] = "vendor.marble.power.IPerf/default";
struct Client {
    // Never reuse inherited Binder objects/locks after fork. Clients must exec.
    const pid_t pid = getpid();
    std::timed_mutex mutex;
    std::shared_ptr<IPerfSession> session;
    std::map<int, marble::power::ClientLease> handles;
    std::map<int, marble::power::ChiLease> chiHandles;
    int64_t nextHandle = 1;
    std::chrono::steady_clock::time_point nextConnect{};
};
Client& client() { static Client value; return value; }
int failure(int code) { errno = code; return -1; }
int unsupported() { return failure(ENOTSUP); }
void disconnect(Client& c) {
    c.handles.clear();
    c.chiHandles.clear();
    c.session.reset();
    c.nextConnect = std::chrono::steady_clock::now() + std::chrono::seconds(1);
}
bool connect(Client& c) {
    if (c.session) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now < c.nextConnect) { errno = EAGAIN; return false; }
    c.nextConnect = now + std::chrono::seconds(1);
    // No waitForService or Binder threadpool changes in vendor caller processes.
    ndk::SpAIBinder binder(AServiceManager_checkService(kService));
    auto service = IPerf::fromBinder(binder);
    if (!service) { errno = ENODEV; return false; }
    auto status = service->openSession(&c.session);
    if (!status.isOk() || !c.session) {
        c.session.reset();
        errno = status.getExceptionCode() == EX_SERVICE_SPECIFIC
                ? status.getServiceSpecificError() : EIO;
        return false;
    }
    return true;
}
}

extern "C" {
int perf_lock_acq(int handle, int duration, int resources[], int count) {
    if (resources == nullptr || count <= 0 || count > 64 || (count & 1) || duration < 0) {
        return failure(EINVAL);
    }
    auto& c = client();
    if (getpid() != c.pid) return failure(ECHILD);
    const std::span<const int32_t> request(resources, static_cast<size_t>(count));
    const auto profile = marble::power::matchPerfProfile(request);
    if (profile.empty()) return unsupported();
    const bool goodix = profile == marble::power::kGoodixProfile;
    if (goodix && !::android::base::GetBoolProperty(
            marble::power::kGoodixEnableProperty, false)) return unsupported();
    auto lock = marble::power::lockClient(c.mutex);
    if (!lock.owns_lock()) return failure(EAGAIN);
    if (!::android::base::GetBoolProperty("vendor.powerhal.perf.enabled", false)) {
        disconnect(c);
        return unsupported();
    }
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(c.handles, [&](const auto& entry) { return !entry.second.retain(now); });
    int remoteHandle = 0;
    if (handle > 0) {
        auto it = c.handles.find(handle);
        if (it == c.handles.end() || !c.session) return failure(ENOENT);
        if (!it->second.accepts(goodix)) return failure(EINVAL);
        remoteHandle = it->second.remoteForUpdate(now);
    } else {
        // Retained Goodix aliases have no live vote after expiry. Reclaim them
        // under capacity pressure rather than allowing unbounded alias growth.
        if (c.handles.size() + c.chiHandles.size() >= 16) {
            std::erase_if(c.handles, [&](const auto& entry) { return entry.second.expired(now); });
        }
        if (c.handles.size() + c.chiHandles.size() >= 16) return failure(ENOSPC);
        if (c.nextHandle > std::numeric_limits<int>::max()) return failure(EOVERFLOW);
        if (!connect(c)) return -1;
    }
    const std::vector<int32_t> args(request.begin(), request.end());
    int32_t result = -EIO;
    auto status = c.session->acquire(remoteHandle, duration, args, &result);
    if (!status.isOk()) { disconnect(c); return failure(EIO); }
    // The server's deadline starts before the client sees the reply. Retry an
    // expired/cancelled *known Goodix alias* once as a new bounded lease. This
    // requires a fresh caller request; it never replays a vote automatically.
    // Unknown aliases, camera requests and service death do not take this path.
    if (goodix && remoteHandle > 0 && result == -ENOENT) {
        status = c.session->acquire(0, duration, args, &result);
        if (!status.isOk()) { disconnect(c); return failure(EIO); }
    }
    if (result <= 0) {
        if (result == -ESTALE) {
            disconnect(c);
            return failure(-result);
        }
        if (handle > 0 && result != -EAGAIN) {
            c.handles.erase(handle);
        }
        return failure(result < 0 ? -result : EIO);
    }
    if (handle <= 0) handle = static_cast<int>(c.nextHandle++);
    const auto lifetime = std::chrono::milliseconds(marble::power::boundedPerfDuration(duration, profile));
    c.handles[handle] = marble::power::ClientLease{
            result, std::chrono::steady_clock::now() + lifetime, goodix};
    return handle;
}

int perf_lock_rel(int handle) {
    auto& c = client();
    if (getpid() != c.pid) return failure(ECHILD);
    auto lock = marble::power::lockClient(c.mutex);
    // On contention, leave mappings intact so release can be retried. Backend
    // leases remain finite even if the caller chooses not to retry release.
    if (!lock.owns_lock()) return failure(EAGAIN);
    auto chi = c.chiHandles.find(handle);
    if (chi != c.chiHandles.end()) {
        if (!c.session) return failure(ENOENT);
        int32_t result = -EIO;
        auto status = c.session->release(chi->second.remote, &result);
        if (!status.isOk()) { disconnect(c); return failure(EIO); }
        if (result == -ESTALE) { disconnect(c); return failure(ESTALE); }
        // A known CHI alias still needs a successful local release after its
        // timed vote expired or policy cancelled it. Unknown aliases never do.
        if (result != 0 && result != -ENOENT) {
            return failure(result < 0 ? -result : EIO);
        }
        c.chiHandles.erase(chi);
        return 0;
    }
    auto it = c.handles.find(handle);
    if (it == c.handles.end() || !c.session) return failure(ENOENT);
    int32_t result = -EIO;
    auto status = c.session->release(it->second.remote, &result);
    c.handles.erase(it);
    if (!status.isOk()) { disconnect(c); return failure(EIO); }
    if (result == -ESTALE) { disconnect(c); return failure(ESTALE); }
    return result == 0 ? 0 : failure(result < 0 ? -result : EIO);
}

// These symbol names are retained for dlopen/dlsym compatibility. Only the
// separately gated finite CHI subset is supported; all other hints fail closed.
int perf_hint_renew(int handle, int hint, const char* userData, int duration,
                    int type, int count, int extras[]) {
    const auto* profile = marble::power::matchChiProfile(hint, type, duration);
    if (!profile) return unsupported();
    if (handle < -1 || userData != nullptr || count != 0 || extras != nullptr) {
        return failure(EINVAL);
    }
    auto& c = client();
    if (getpid() != c.pid) return failure(ECHILD);
    auto lock = marble::power::lockClient(c.mutex);
    if (!lock.owns_lock()) return failure(EAGAIN);
    if (!::android::base::GetBoolProperty("vendor.powerhal.perf.enabled", false)) {
        disconnect(c);
        return unsupported();
    }
    if (!::android::base::GetBoolProperty(marble::power::kChiEnableProperty, false)) {
        return unsupported();
    }
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(c.handles, [&](const auto& entry) { return !entry.second.retain(now); });
    int remoteHandle = 0;
    if (handle > 0) {
        const auto it = c.chiHandles.find(handle);
        if (it == c.chiHandles.end() || !c.session) return failure(ENOENT);
        if (!it->second.accepts(hint)) return failure(EINVAL);
        remoteHandle = it->second.remoteForUpdate(now);
    } else {
        if (c.handles.size() + c.chiHandles.size() >= 16) {
            std::erase_if(c.handles, [&](const auto& entry) { return entry.second.expired(now); });
        }
        // CHI tombstones are not evicted: CHI must still be able to release its
        // refcounted alias. All raw + CHI aliases share one bounded budget.
        if (c.handles.size() + c.chiHandles.size() >= 16) return failure(ENOSPC);
        if (c.nextHandle > std::numeric_limits<int>::max()) return failure(EOVERFLOW);
        if (!connect(c)) return -1;
    }
    int32_t result = -EIO;
    auto status = c.session->acquireHint(remoteHandle, duration, hint, type, &result);
    if (!status.isOk()) { disconnect(c); return failure(EIO); }
    // Server time starts earlier; policy cancellation also removes remote votes.
    // One fresh request can rearm a KNOWN alias, but never replay after death.
    if (remoteHandle > 0 && result == -ENOENT) {
        status = c.session->acquireHint(0, duration, hint, type, &result);
        if (!status.isOk()) { disconnect(c); return failure(EIO); }
    }
    // Revoked owners cannot recover in-place. Drop ALL aliases and back off;
    // only a later fresh caller request may open a session, never replay here.
    if (result == -ESTALE) { disconnect(c); return failure(ESTALE); }
    // Preserve the old alias on a rejected renewal. Server rollback preserves
    // an unexpired previous CHI lease if applying its replacement failed.
    if (result <= 0) return failure(result < 0 ? -result : EIO);
    if (handle <= 0) handle = static_cast<int>(c.nextHandle++);
    c.chiHandles.insert_or_assign(handle, marble::power::ChiLease{
            result, hint, std::chrono::steady_clock::now() + std::chrono::milliseconds(duration)});
    return handle;
}
int perf_hint(int hint, const char* userData, int duration, int type) {
    return perf_hint_renew(0, hint, userData, duration, type, 0, nullptr);
}
int perf_get_feedback(int, const char*) { return unsupported(); }
int perf_get_feedback_extn(int, const char*, unsigned int, char*) { return unsupported(); }
int perf_lock_use_profile(int, int) { return unsupported(); }
void perf_event(int, const char*, int, int*) { errno = ENOTSUP; }
void perf_lock_cmd(int) { errno = ENOTSUP; }
// Structure-return property APIs are implemented separately in PropertyClient.cpp.
}
