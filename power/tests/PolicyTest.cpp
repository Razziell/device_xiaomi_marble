// SPDX-License-Identifier: Apache-2.0
#include "DisplayUpdatePolicy.h"
#include "HintController.h"
#include "InteractionPolicy.h"
#include "perf/ClientLease.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace marble::power {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct Backend : HintBackend {
    std::map<std::string, unsigned> starts, ends;
    std::set<std::string> active;
    std::vector<std::string> log;  // Accepted calls in enqueue order.
    std::map<std::string, std::chrono::milliseconds> lastDuration;
    bool reject = false;
    // Atomic: a failed mode call arms the controller retry thread, which may
    // call end() while the test thread flips this flag.
    std::atomic<bool> rejectEnd{false};
    bool supports(const std::string&) const override { return true; }
    bool start(const std::string& hint) override { return startFor(hint, 0ms); }
    bool startFor(const std::string& hint, std::chrono::milliseconds duration) override {
        ++starts[hint];
        if (reject) return false;
        log.push_back("start:" + hint);
        lastDuration[hint] = duration;
        active.insert(hint);
        return true;
    }
    bool end(const std::string& hint) override {
        ++ends[hint];
        if (rejectEnd) return false;
        log.push_back("end:" + hint);
        active.erase(hint);
        return true;
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

TEST(ScreenOffPolicy, MasksFixedAndSustainedFloorsAndRestoresOnWake) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("FIXED_PERFORMANCE", true));
    EXPECT_TRUE(backend.active.contains("FIXED_PERFORMANCE"));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    EXPECT_FALSE(backend.active.contains("FIXED_PERFORMANCE"));
    // A request made while the screen is off is remembered, not applied.
    ASSERT_TRUE(controller.setMode("SUSTAINED_PERFORMANCE", true));
    EXPECT_FALSE(backend.active.contains("SUSTAINED_PERFORMANCE"));
    EXPECT_EQ(backend.starts["SUSTAINED_PERFORMANCE"], 0u);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    EXPECT_TRUE(backend.active.contains("SUSTAINED_PERFORMANCE"));
    EXPECT_FALSE(backend.active.contains("FIXED_PERFORMANCE"));  // Sustained still wins.
    ASSERT_TRUE(controller.setMode("SUSTAINED_PERFORMANCE", false));
    EXPECT_TRUE(backend.active.contains("FIXED_PERFORMANCE"));
}

TEST(ScreenOffPolicy, UnknownScreenStateMasksNothing) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("FIXED_PERFORMANCE", true));
    EXPECT_TRUE(backend.active.contains("FIXED_PERFORMANCE"));
    ASSERT_TRUE(controller.setMode("FIXED_PERFORMANCE", false));
    ASSERT_TRUE(controller.setMode("SUSTAINED_PERFORMANCE", true));
    EXPECT_TRUE(backend.active.contains("SUSTAINED_PERFORMANCE"));
}

TEST(ScreenOffPolicy, RequestDisabledWhileScreenOffIsNotReplayed) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("SUSTAINED_PERFORMANCE", true));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    EXPECT_FALSE(backend.active.contains("SUSTAINED_PERFORMANCE"));
    ASSERT_TRUE(controller.setMode("SUSTAINED_PERFORMANCE", false));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    EXPECT_FALSE(backend.active.contains("SUSTAINED_PERFORMANCE"));
    EXPECT_EQ(backend.starts["SUSTAINED_PERFORMANCE"], 1u);
}

TEST(SaverPolicy, LowPowerKeepsCapsAcrossScreenCycleAndAllowsInteraction) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("SUSTAINED_PERFORMANCE", true));
    ASSERT_TRUE(controller.setMode("LOW_POWER", true));
    EXPECT_TRUE(backend.active.contains("LOW_POWER"));
    EXPECT_FALSE(backend.active.contains("SUSTAINED_PERFORMANCE"));
    // The saver masks the sustained floors, so boosts are not blocked by them.
    ASSERT_TRUE(controller.setBoost("INTERACTION", 300));
    EXPECT_EQ(backend.starts["INTERACTION"], 1u);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    EXPECT_TRUE(backend.active.contains("LOW_POWER"));
    EXPECT_FALSE(backend.active.contains("INTERACTION"));
    ASSERT_TRUE(controller.setBoost("INTERACTION", 300));  // Dropped while off.
    EXPECT_EQ(backend.starts["INTERACTION"], 1u);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    EXPECT_TRUE(backend.active.contains("LOW_POWER"));
    EXPECT_FALSE(backend.active.contains("SUSTAINED_PERFORMANCE"));
    ASSERT_TRUE(controller.setMode("LOW_POWER", false));
    EXPECT_TRUE(backend.active.contains("SUSTAINED_PERFORMANCE"));
}

TEST(SaverPolicy, FlipendoBlocksBoostsAndLaunchUnlikeLowPower) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("LOW_POWER", true));
    ASSERT_TRUE(controller.setMode("LAUNCH", true));
    EXPECT_EQ(backend.starts["LAUNCH"], 1u);
    ASSERT_TRUE(controller.setMode("Flipendo", true));
    EXPECT_FALSE(backend.active.contains("LAUNCH"));
    ASSERT_TRUE(controller.setMode("LAUNCH", true));
    ASSERT_TRUE(controller.setBoost("INTERACTION", 300));
    EXPECT_EQ(backend.starts["LAUNCH"], 1u);
    EXPECT_EQ(backend.starts["INTERACTION"], 0u);
    ASSERT_TRUE(controller.setMode("Flipendo", false));
    ASSERT_TRUE(controller.setBoost("INTERACTION", 300));
    EXPECT_EQ(backend.starts["INTERACTION"], 1u);
    EXPECT_TRUE(backend.active.contains("LOW_POWER"));
}

TEST(TransientCancel, RepeatedBlockingModeCallsDoNotRequeueCancellations) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setBoost("INTERACTION", 300));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    EXPECT_EQ(backend.ends["INTERACTION"], 1u);
    EXPECT_EQ(backend.ends["LAUNCH"], 1u);
    // Duplicate screen-off and other mode calls while still blocked.
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    ASSERT_TRUE(controller.setMode("EXPENSIVE_RENDERING", true));
    ASSERT_TRUE(controller.setMode("LOW_POWER", true));
    EXPECT_EQ(backend.ends["INTERACTION"], 1u);
    EXPECT_EQ(backend.ends["LAUNCH"], 1u);
    // A new start re-arms the cancellation for the next blocking call.
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("LAUNCH", true));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    EXPECT_EQ(backend.ends["INTERACTION"], 2u);
    EXPECT_EQ(backend.ends["LAUNCH"], 2u);
    EXPECT_FALSE(backend.active.contains("LAUNCH"));
}

// dump() takes the controller mutex. Poll only its snapshot, never a fake
// backend container while the retry worker may be writing it.
bool waitForRetryCompletion(HintController& controller) {
    const auto until = Clock::now() + 2s;
    do {
        if (controller.dump().find("Mode reconcile failures/pending retries: 1/0\n") !=
            std::string::npos) return true;
        std::this_thread::sleep_for(5ms);
    } while (Clock::now() < until);
    return false;
}

TEST(TransientCancel, SaverRecoversWithoutSecondBlockingCall) {
    Backend backend;
    {
        HintController controller(backend);
        ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
        ASSERT_TRUE(controller.setMode("GAME_LOADING", true));
        ASSERT_TRUE(controller.setBoost("INTERACTION", 300));
        backend.rejectEnd = true;
        EXPECT_FALSE(controller.setMode("Flipendo", true));
        backend.rejectEnd = false;
        ASSERT_TRUE(waitForRetryCompletion(controller));
    }
    EXPECT_FALSE(backend.active.contains("GAME_LOADING"));
    EXPECT_FALSE(backend.active.contains("INTERACTION"));
    EXPECT_TRUE(backend.active.contains("Flipendo"));
}

TEST(WakeBoost, FailedFingerprintCancellationDoesNotLoseGameRestore) {
    Backend backend;
    {
        HintController controller(backend);
        const auto owner = controller.openPerfOwner();
        ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
        ASSERT_TRUE(controller.setMode("GAME", true));
        ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
        ASSERT_GT(controller.acquireGoodix(owner, 0, 500, GoodixAccess{true, true, true}), 0);
        backend.rejectEnd = true;
        EXPECT_FALSE(controller.setMode("INTERACTIVE", true));
        backend.rejectEnd = false;
        ASSERT_TRUE(waitForRetryCompletion(controller));
    }
    EXPECT_TRUE(backend.active.contains("GAME"));
    EXPECT_FALSE(backend.active.contains(kGoodixScreenOffProfile));
}

TEST(ModePersistence, FailedSaveIsRetriedWithoutAnotherModeCall) {
    struct Store : RequestedModeStore {
        std::atomic<unsigned> attempts{0};
        std::string saved;
        std::string load() const override { return {}; }
        bool save(const std::string& value) override {
            if (++attempts == 1) return false;
            saved = value;
            return true;
        }
    } store;
    Backend backend;
    {
        HintController controller(backend, {}, &store);
        EXPECT_FALSE(controller.setMode("INTERACTIVE", true));
        const auto until = Clock::now() + 2s;
        while (store.attempts < 2 && Clock::now() < until) std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(store.saved, encodeRequestedModes({"INTERACTIVE"}, true));
    EXPECT_EQ(backend.starts["INTERACTIVE"], 1u);
}

TEST(TransientCancel, FirstBlockingCallAfterStartupAlwaysCancels) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    EXPECT_EQ(backend.ends["INTERACTION"], 1u);
    EXPECT_EQ(backend.ends["LAUNCH"], 1u);
}

// Quiet gate off: these tests exercise the other display-update rules.
constexpr InteractionConfig kExtras{.minMs = 300, .maxMs = 1500, .offsetMs = 0,
                                    .wakeBoostMs = 600, .displayUpdateBoost = true,
                                    .displayUpdateQuietMs = 0};

size_t indexOf(const std::vector<std::string>& log, const std::string& entry) {
    return static_cast<size_t>(std::find(log.begin(), log.end(), entry) - log.begin());
}

TEST(GamePolicy, GameModeIsMaskedBySaverAndScreenOffAndRestored) {
    Backend backend;
    HintController controller(backend);
    EXPECT_TRUE(controller.supportsMode("GAME"));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("GAME", true));
    EXPECT_TRUE(backend.active.contains("GAME"));
    ASSERT_TRUE(controller.setMode("LOW_POWER", true));
    EXPECT_FALSE(backend.active.contains("GAME"));
    ASSERT_TRUE(controller.setMode("LOW_POWER", false));
    EXPECT_TRUE(backend.active.contains("GAME"));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    EXPECT_FALSE(backend.active.contains("GAME"));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    EXPECT_TRUE(backend.active.contains("GAME"));
    ASSERT_TRUE(controller.setMode("GAME", false));
    EXPECT_FALSE(backend.active.contains("GAME"));
    EXPECT_EQ(backend.starts["GAME"], 3u);
}

TEST(GamePolicy, GameModeDoesNotBlockBoostsAndIsNotPersisted) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("GAME", true));
    ASSERT_TRUE(controller.setBoost("INTERACTION", 300));
    EXPECT_EQ(backend.starts["INTERACTION"], 1u);
    // The restart snapshot format is unchanged: GAME is resent by the framework
    // when the foreground app changes, like LAUNCH it is not stored.
    EXPECT_EQ(encodeRequestedModes({"INTERACTIVE", "GAME", "GAME_LOADING"}, true), "v1:3");
}

TEST(GamePolicy, GameLoadingIsAFiniteBoostWithLaunchRules) {
    Backend backend;
    HintController controller(backend);
    EXPECT_TRUE(controller.supportsMode("GAME_LOADING"));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("GAME_LOADING", true));
    EXPECT_TRUE(backend.active.contains("GAME_LOADING"));
    ASSERT_TRUE(controller.setMode("GAME_LOADING", false));
    EXPECT_FALSE(backend.active.contains("GAME_LOADING"));
    // Screen-off cancels a running one and blocks new ones; it is not replayed.
    ASSERT_TRUE(controller.setMode("GAME_LOADING", true));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    EXPECT_FALSE(backend.active.contains("GAME_LOADING"));
    ASSERT_TRUE(controller.setMode("GAME_LOADING", true));
    EXPECT_EQ(backend.starts["GAME_LOADING"], 2u);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    EXPECT_FALSE(backend.active.contains("GAME_LOADING"));
}

TEST(GamePolicy, BatterySaverEndsAndBlocksGameLoading) {
    for (const char* saver : {"LOW_POWER", "Flipendo"}) {
        Backend backend;
        HintController controller(backend);
        ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
        ASSERT_TRUE(controller.setMode("GAME_LOADING", true));
        ASSERT_TRUE(controller.setMode(saver, true));
        EXPECT_FALSE(backend.active.contains("GAME_LOADING")) << saver;
        ASSERT_TRUE(controller.setMode("GAME_LOADING", true));
        EXPECT_EQ(backend.starts["GAME_LOADING"], 1u) << saver;
        ASSERT_TRUE(controller.setMode(saver, false));
        EXPECT_FALSE(backend.active.contains("GAME_LOADING")) << saver;
        ASSERT_TRUE(controller.setMode("GAME_LOADING", true));
        EXPECT_EQ(backend.starts["GAME_LOADING"], 2u) << saver;
    }
}

TEST(DisplayUpdateBoost, IsNotAdvertisedUnlessEnabled) {
    Backend backend;
    HintController off(backend);
    EXPECT_FALSE(off.supportsBoost("DISPLAY_UPDATE_IMMINENT"));
    EXPECT_FALSE(off.setBoost("DISPLAY_UPDATE_IMMINENT", 0));
    EXPECT_EQ(backend.starts["DISPLAY_UPDATE_IMMINENT"], 0u);
    HintController on(backend, kExtras);
    EXPECT_TRUE(on.supportsBoost("DISPLAY_UPDATE_IMMINENT"));
    EXPECT_TRUE(on.supportsBoost("INTERACTION"));
}

TEST(DisplayUpdateBoost, EveryRequestIsAShortFixedVote) {
    Backend backend;
    HintController controller(backend, kExtras);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    // SurfaceFlinger passes 0; a caller-supplied duration is not honoured.
    ASSERT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 0));
    ASSERT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 60000));
    EXPECT_EQ(backend.starts["DISPLAY_UPDATE_IMMINENT"], 2u);
    EXPECT_EQ(backend.lastDuration["DISPLAY_UPDATE_IMMINENT"], 100ms);
    EXPECT_EQ(backend.starts["INTERACTION"], 0u);
    ASSERT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", -1));
    EXPECT_FALSE(backend.active.contains("DISPLAY_UPDATE_IMMINENT"));
    EXPECT_NE(controller.dump().find("enabled/quiet ms: 1/0, enqueued/not-quiet/other-skipped: 2/0/0"),
              std::string::npos);
}

TEST(DisplayUpdateBoost, SkippedWhileAnInteractionVoteCoversIt) {
    Backend backend;
    HintController controller(backend, kExtras);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setBoost("INTERACTION", 1500));
    ASSERT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 0));
    EXPECT_EQ(backend.starts["DISPLAY_UPDATE_IMMINENT"], 0u);
    // After the interaction vote is cancelled the display boost is needed again.
    ASSERT_TRUE(controller.setBoost("INTERACTION", -1));
    ASSERT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 0));
    EXPECT_EQ(backend.starts["DISPLAY_UPDATE_IMMINENT"], 1u);
}

TEST(DisplayUpdateBoost, InteractionVoteAboutToExpireDoesNotSuppressIt) {
    Backend backend;
    HintController controller(backend, {.minMs = 1, .maxMs = 1500, .displayUpdateBoost = true,
                                        .displayUpdateQuietMs = 0});
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setBoost("INTERACTION", 20));  // Ends before the 100ms vote would.
    ASSERT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 0));
    EXPECT_EQ(backend.starts["DISPLAY_UPDATE_IMMINENT"], 1u);
}

TEST(DisplayUpdateBoost, DroppedInSaverScreenOffAndSustainedAndCancelledWithTheOthers) {
    Backend backend;
    HintController controller(backend, kExtras);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    for (const char* blocker : {"LOW_POWER", "Flipendo", "SUSTAINED_PERFORMANCE"}) {
        ASSERT_TRUE(controller.setMode(blocker, true));
        EXPECT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 0)) << blocker;
        ASSERT_TRUE(controller.setMode(blocker, false));
    }
    EXPECT_EQ(backend.starts["DISPLAY_UPDATE_IMMINENT"], 0u);
    ASSERT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 0));
    EXPECT_TRUE(backend.active.contains("DISPLAY_UPDATE_IMMINENT"));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    EXPECT_FALSE(backend.active.contains("DISPLAY_UPDATE_IMMINENT"));
    EXPECT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 0));
    EXPECT_EQ(backend.starts["DISPLAY_UPDATE_IMMINENT"], 1u);
}

TEST(DisplayUpdatePolicy, OnlyARequestAfterAQuietPeriodIsBoosted) {
    const auto now = Clock::now();
    EXPECT_TRUE(displayUpdateAfterQuiet(now, std::nullopt, 1200ms));  // First request ever.
    EXPECT_FALSE(displayUpdateAfterQuiet(now, now, 1200ms));
    EXPECT_FALSE(displayUpdateAfterQuiet(now, now - 500ms, 1200ms));   // Blinking cursor.
    EXPECT_FALSE(displayUpdateAfterQuiet(now, now - 1000ms, 1200ms));  // Ticking clock.
    EXPECT_FALSE(displayUpdateAfterQuiet(now, now - 1199ms, 1200ms));
    EXPECT_TRUE(displayUpdateAfterQuiet(now, now - 1200ms, 1200ms));
    EXPECT_TRUE(displayUpdateAfterQuiet(now, now - 5s, 1200ms));
    // Zero (or a malformed negative value) turns the gate off.
    EXPECT_TRUE(displayUpdateAfterQuiet(now, now, 0ms));
    EXPECT_TRUE(displayUpdateAfterQuiet(now, now, -1ms));
}

TEST(DisplayUpdateBoost, QuietGateIsOnByDefaultAndBounded) {
    Backend backend;
    HintController byDefault(backend, {.displayUpdateBoost = true});
    EXPECT_NE(byDefault.dump().find("enabled/quiet ms: 1/1200,"), std::string::npos);
    HintController huge(backend, {.displayUpdateBoost = true, .displayUpdateQuietMs = 1 << 30});
    EXPECT_NE(huge.dump().find("enabled/quiet ms: 1/60000,"), std::string::npos);
    HintController negative(backend, {.displayUpdateBoost = true, .displayUpdateQuietMs = -7});
    EXPECT_NE(negative.dump().find("enabled/quiet ms: 1/0,"), std::string::npos);
}

TEST(DisplayUpdateBoost, SteadyStreamOfRequestsIsBoostedOnceUntilItGoesQuiet) {
    Backend backend;
    // Wide margins: gaps of 150ms against a 450ms quiet period.
    HintController controller(backend, {.displayUpdateBoost = true, .displayUpdateQuietMs = 450});
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 0));
    EXPECT_EQ(backend.starts["DISPLAY_UPDATE_IMMINENT"], 1u);
    // Periodic content: 600ms pass since the boost, but never 450ms of silence.
    for (int i = 0; i < 4; ++i) {
        std::this_thread::sleep_for(150ms);
        ASSERT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 0));
    }
    EXPECT_EQ(backend.starts["DISPLAY_UPDATE_IMMINENT"], 1u);
    EXPECT_NE(controller.dump().find("enqueued/not-quiet/other-skipped: 1/4/0"), std::string::npos);
    std::this_thread::sleep_for(500ms);
    ASSERT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 0));
    EXPECT_EQ(backend.starts["DISPLAY_UPDATE_IMMINENT"], 2u);
}

TEST(DisplayUpdateBoost, RequestsDroppedByPolicyStillCountAsActivity) {
    Backend backend;
    HintController controller(backend, {.displayUpdateBoost = true, .displayUpdateQuietMs = 60000});
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("LOW_POWER", true));
    ASSERT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 0));  // Dropped: saver.
    ASSERT_TRUE(controller.setMode("LOW_POWER", false));
    ASSERT_TRUE(controller.setBoost("DISPLAY_UPDATE_IMMINENT", 0));  // Dropped: not quiet.
    EXPECT_EQ(backend.starts["DISPLAY_UPDATE_IMMINENT"], 0u);
    EXPECT_NE(controller.dump().find("enqueued/not-quiet/other-skipped: 0/1/1"), std::string::npos);
}

TEST(WakeBoost, OffByDefault) {
    Backend backend;
    HintController controller(backend);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    EXPECT_EQ(backend.starts["INTERACTION"], 0u);
}

TEST(WakeBoost, OnlyARealScreenOnTransitionStartsOneBoundedInteractionVote) {
    Backend backend;
    HintController controller(backend, kExtras);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));   // First report after boot.
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));   // Duplicate.
    EXPECT_EQ(backend.starts["INTERACTION"], 0u);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    EXPECT_EQ(backend.starts["INTERACTION"], 0u);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    EXPECT_EQ(backend.starts["INTERACTION"], 1u);
    EXPECT_EQ(backend.lastDuration["INTERACTION"], 600ms);
    EXPECT_TRUE(backend.active.contains("INTERACTION"));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));   // Duplicate after wake.
    EXPECT_EQ(backend.starts["INTERACTION"], 1u);
    // A shorter interaction request right after wake shares the wake vote.
    ASSERT_TRUE(controller.setBoost("INTERACTION", 300));
    EXPECT_EQ(backend.starts["INTERACTION"], 1u);
    EXPECT_NE(controller.dump().find("Wake boost ms/started: 600/1"), std::string::npos);
    // Screen-off cancels it like any interaction vote.
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    EXPECT_FALSE(backend.active.contains("INTERACTION"));
}

TEST(WakeBoost, DurationIsBoundedByTheInteractionMaximum) {
    Backend backend;
    HintController controller(backend, {.minMs = 300, .maxMs = 400, .wakeBoostMs = 60000});
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    EXPECT_EQ(backend.lastDuration["INTERACTION"], 400ms);
    Backend other;
    HintController negative(other, {.wakeBoostMs = -5});
    ASSERT_TRUE(negative.setMode("INTERACTIVE", false));
    ASSERT_TRUE(negative.setMode("INTERACTIVE", true));
    EXPECT_EQ(other.starts["INTERACTION"], 0u);
}

TEST(WakeBoost, FollowsBoostBlockingRules) {
    Backend backend;
    HintController controller(backend, kExtras);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    ASSERT_TRUE(controller.setMode("Flipendo", true));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    EXPECT_EQ(backend.starts["INTERACTION"], 0u);
    ASSERT_TRUE(controller.setMode("Flipendo", false));
    // Like any INTERACTION vote it is allowed under LOW_POWER.
    ASSERT_TRUE(controller.setMode("LOW_POWER", true));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    EXPECT_EQ(backend.starts["INTERACTION"], 1u);
    // A failed enqueue does not fail the screen-on call.
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    backend.reject = true;
    controller.setMode("INTERACTIVE", true);
    backend.reject = false;
    EXPECT_NE(controller.dump().find("Wake boost ms/started: 600/1"), std::string::npos);
}

TEST(WakeBoost, IsEnqueuedBeforeTheScreenOffFingerprintLeaseIsCancelled) {
    Backend backend;
    HintController controller(backend, kExtras);
    const auto owner = controller.openPerfOwner();
    ASSERT_NE(owner, 0u);
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    ASSERT_TRUE(controller.setMode("INTERACTIVE", false));
    ASSERT_GT(controller.acquireGoodix(owner, 0, 500, GoodixAccess{true, true, true}), 0);
    ASSERT_TRUE(backend.active.contains(kGoodixScreenOffProfile));
    backend.log.clear();
    ASSERT_TRUE(controller.setMode("INTERACTIVE", true));
    EXPECT_FALSE(backend.active.contains(kGoodixScreenOffProfile));
    const auto boost = indexOf(backend.log, "start:INTERACTION");
    const auto cancel = indexOf(backend.log, std::string("end:") + kGoodixScreenOffProfile);
    ASSERT_LT(boost, backend.log.size());
    ASSERT_LT(cancel, backend.log.size());
    EXPECT_LT(boost, cancel);
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
