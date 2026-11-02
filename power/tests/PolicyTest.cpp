// SPDX-License-Identifier: Apache-2.0
#include "HintController.h"
#include "InteractionPolicy.h"
#include "perf/ClientLease.h"

#include <gtest/gtest.h>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <thread>

namespace marble::power {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct Backend : HintBackend {
    std::map<std::string, unsigned> starts, ends;
    std::set<std::string> active;
    bool reject = false;
    bool supports(const std::string&) const override { return true; }
    bool start(const std::string& hint) override { return startFor(hint, 0ms); }
    bool startFor(const std::string& hint, std::chrono::milliseconds) override {
        ++starts[hint];
        if (reject) return false;
        active.insert(hint);
        return true;
    }
    bool end(const std::string& hint) override {
        ++ends[hint]; active.erase(hint); return true;
    }
};

TEST(InteractionPolicy, BoundaryAndExpiry) {
    const auto now = Clock::time_point{} + 1s;
    EXPECT_TRUE(shouldEnqueueInteraction(now, {}, now + 300ms));
    EXPECT_FALSE(shouldEnqueueInteraction(now, now + 300ms, now + 299ms));
    EXPECT_FALSE(shouldEnqueueInteraction(now, now + 300ms, now + 324ms));
    EXPECT_TRUE(shouldEnqueueInteraction(now, now + 300ms, now + 325ms));
    EXPECT_TRUE(shouldEnqueueInteraction(now, now + 24ms, now + 25ms));
    EXPECT_TRUE(shouldEnqueueInteraction(now, now, now + 1ms));
}

TEST(InteractionPolicy, RepeatedRequestsDoNotAdvanceSkippedDeadline) {
    const auto start = Clock::time_point{} + 1s;
    Clock::time_point applied;
    unsigned accepted = 0;
    for (int i = 0; i < 1000; ++i) {
        const auto now = start + std::chrono::milliseconds(i);
        const auto requested = now + 300ms;
        if (shouldEnqueueInteraction(now, applied, requested)) {
            applied = requested;
            ++accepted;
        }
        EXPECT_LE(applied, requested);
        EXPECT_LT(requested - applied, 25ms);
    }
    EXPECT_EQ(accepted, 40u); // Synthetic 1kHz, not a measured Android call rate.
}

TEST(InteractionController, CoalescesButForwardsLongerRequestsAndCancel) {
    Backend backend;
    HintController controller(backend, {1, 1500, 0});
    ASSERT_TRUE(controller.setBoost("INTERACTION", 300));
    ASSERT_TRUE(controller.setBoost("INTERACTION", 1));
    EXPECT_EQ(backend.starts["INTERACTION"], 1u);
    ASSERT_TRUE(controller.setBoost("INTERACTION", 1500));
    EXPECT_GE(backend.starts["INTERACTION"], 2u);
    ASSERT_TRUE(controller.setBoost("INTERACTION", -1));
    EXPECT_EQ(backend.ends["INTERACTION"], 1u);
    const auto count = backend.starts["INTERACTION"];
    ASSERT_TRUE(controller.setBoost("INTERACTION", 300));
    EXPECT_EQ(backend.starts["INTERACTION"], count + 1);
    EXPECT_NE(controller.dump().find("Interaction enqueued/coalesced:"), std::string::npos);
}

TEST(InteractionController, FailedEnqueueDoesNotAdvanceDeadline) {
    Backend backend;
    HintController controller(backend);
    backend.reject = true;
    EXPECT_FALSE(controller.setBoost("INTERACTION", 300));
    backend.reject = false;
    EXPECT_TRUE(controller.setBoost("INTERACTION", 300));
    EXPECT_EQ(backend.starts["INTERACTION"], 2u);
}

TEST(RenderingPolicy, SaverSuppressesAndRestoresRequestedRendering) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("EXPENSIVE_RENDERING", true));
    EXPECT_TRUE(backend.active.contains("EXPENSIVE_RENDERING"));
    ASSERT_TRUE(controller.setMode("LOW_POWER", true));
    EXPECT_FALSE(backend.active.contains("EXPENSIVE_RENDERING"));
    EXPECT_TRUE(backend.active.contains("LOW_POWER"));
    ASSERT_TRUE(controller.setMode("LOW_POWER", false));
    EXPECT_TRUE(backend.active.contains("EXPENSIVE_RENDERING"));
}

TEST(RenderingPolicy, DisablingMaskedRequestPreventsReplay) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("LOW_POWER", true));
    ASSERT_TRUE(controller.setMode("EXPENSIVE_RENDERING", true));
    EXPECT_FALSE(backend.active.contains("EXPENSIVE_RENDERING"));
    ASSERT_TRUE(controller.setMode("EXPENSIVE_RENDERING", false));
    ASSERT_TRUE(controller.setMode("LOW_POWER", false));
    EXPECT_FALSE(backend.active.contains("EXPENSIVE_RENDERING"));
}

TEST(RenderingPolicy, ScreenOffStillMasksAfterLeavingSaver) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("EXPENSIVE_RENDERING", true));
    ASSERT_TRUE(controller.setMode("LOW_POWER", true));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    ASSERT_TRUE(controller.setMode("LOW_POWER", false));
    EXPECT_FALSE(backend.active.contains("EXPENSIVE_RENDERING"));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    EXPECT_TRUE(backend.active.contains("EXPENSIVE_RENDERING"));
}

TEST(RenderingPolicy, FixedAndSustainedScreenOffContractUnchanged) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("FIXED_PERFORMANCE", true));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    EXPECT_TRUE(backend.active.contains("FIXED_PERFORMANCE"));
    ASSERT_TRUE(controller.setMode("SUSTAINED_PERFORMANCE", true));
    EXPECT_TRUE(backend.active.contains("SUSTAINED_PERFORMANCE"));
    EXPECT_FALSE(backend.active.contains("FIXED_PERFORMANCE"));
}

TEST(GoodixAliasPolicy, ReclaimsOnlyExpiredRejectedGoodix) {
    const auto now = Clock::now();
    EXPECT_TRUE((ClientLease{1, now, true, true}.reclaimOnFreshGoodix(now)));
    EXPECT_FALSE((ClientLease{1, now + 1ms, true, true}.reclaimOnFreshGoodix(now)));
    EXPECT_FALSE((ClientLease{1, now, true, false}.reclaimOnFreshGoodix(now)));
    EXPECT_FALSE((ClientLease{1, now, false, true}.reclaimOnFreshGoodix(now)));
}

}  // namespace
}  // namespace marble::power
