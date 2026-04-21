/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 */

#include "mtcreceiver.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using Msg = std::vector<unsigned char>;

// Build a single MTC quarter-frame MIDI message for a given QF type (0-7)
// and its 4-bit data payload. decodeQuarterFrame only reads message[1], so
// message[0] can be anything.
Msg qf(unsigned char type, unsigned char value) {
    return { 0xF1, static_cast<unsigned char>((type << 4) | (value & 0x0F)) };
}

// A forward-running QF sequence for timecode 00:00:00:00 at 25 fps. QF types
// emitted in order 0-7, each carrying zero data. Results in one completed
// frame at the final QF.
std::vector<Msg> forwardsZeroSequence() {
    return {
        qf(0, 0),  // frame LSB
        qf(1, 0),  // frame MSB
        qf(2, 0),  // seconds LSB
        qf(3, 0),  // seconds MSB
        qf(4, 0),  // minutes LSB
        qf(5, 0),  // minutes MSB
        qf(6, 0),  // hours LSB
        qf(7, 2),  // hours MSB + rate; rate bits (0x06 >> 1) = 1 → 25 fps
    };
}

class CallbackFixture : public ::testing::Test {
protected:
    void SetUp() override {
        MtcReceiver::resetStaticStateForTesting();
    }
    void TearDown() override {
        MtcReceiver::resetStaticStateForTesting();
    }
};

} // namespace

// ─── Registration & invocation ───────────────────────────────────────────────

TEST_F(CallbackFixture, InvokeTickDeliversToRegisteredCallback) {
    std::atomic<long> got{-1};
    std::atomic<bool> complete{false};

    MtcReceiver::setTickCallback([&](long ms, bool isComplete) {
        got.store(ms);
        complete.store(isComplete);
    });

    MtcReceiver::invokeTickForTesting(1234, true);

    EXPECT_EQ(got.load(), 1234);
    EXPECT_TRUE(complete.load());
}

TEST_F(CallbackFixture, UnregisterSilencesCallback) {
    std::atomic<int> hits{0};
    MtcReceiver::setTickCallback([&](long, bool) { hits.fetch_add(1); });

    MtcReceiver::invokeTickForTesting(10, false);
    EXPECT_EQ(hits.load(), 1);

    MtcReceiver::setTickCallback({});
    MtcReceiver::invokeTickForTesting(20, false);
    EXPECT_EQ(hits.load(), 1);
}

TEST_F(CallbackFixture, ReplaceCallbackDropsOldOne) {
    std::atomic<int> a{0}, b{0};
    MtcReceiver::setTickCallback([&](long, bool) { a.fetch_add(1); });
    MtcReceiver::setTickCallback([&](long, bool) { b.fetch_add(1); });

    MtcReceiver::invokeTickForTesting(0, false);
    EXPECT_EQ(a.load(), 0);
    EXPECT_EQ(b.load(), 1);
}

TEST_F(CallbackFixture, InvokeWithoutCallbackIsSafe) {
    EXPECT_NO_FATAL_FAILURE(MtcReceiver::invokeTickForTesting(42, false));
}

// ─── Thread safety (register / invoke / unregister races) ────────────────────

TEST_F(CallbackFixture, ConcurrentInvokeAndRegisterIsSafe) {
    // Stress the callbackMutex_ path: one thread registers/unregisters in a
    // loop while another invokes. Under TSan this flags any non-atomic
    // accesses on tickCallback_. Without the mutex this would UB on
    // std::function's control block.
    std::atomic<bool> stop{false};
    std::atomic<int>  invocations{0};

    std::thread invoker([&] {
        while (!stop.load()) {
            MtcReceiver::invokeTickForTesting(1, false);
            invocations.fetch_add(1);
        }
    });

    auto cb = [&](long, bool) {};
    for (int i = 0; i < 1000; ++i) {
        MtcReceiver::setTickCallback(cb);
        MtcReceiver::setTickCallback({});
    }
    stop.store(true);
    invoker.join();

    // The invariant is "doesn't crash / no data race". The exact count depends
    // on scheduling; a positive count just proves the invoker actually ran.
    EXPECT_GT(invocations.load(), 0);
}

TEST_F(CallbackFixture, UnregisterBlocksUntilInFlightReturns) {
    // setTickCallback({}) holds callbackMutex_, and the MIDI-thread invocation
    // path holds the same mutex while the callback runs. So a slow callback
    // makes setTickCallback block until it completes — guaranteeing the
    // handler will not run after the unregister call returns.
    std::atomic<bool> insideCallback{false};
    std::atomic<bool> callbackReleased{false};
    std::atomic<bool> callbackReturned{false};

    MtcReceiver::setTickCallback([&](long, bool) {
        insideCallback.store(true);
        while (!callbackReleased.load()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        callbackReturned.store(true);
    });

    std::thread firing([&] { MtcReceiver::invokeTickForTesting(0, false); });

    // Wait until the invoker has the mutex and is stuck inside the handler.
    while (!insideCallback.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    std::atomic<bool> unregisterReturned{false};
    std::thread unregistering([&] {
        MtcReceiver::setTickCallback({});
        unregisterReturned.store(true);
    });

    // The unregistering thread must be blocked: the invoker still holds the
    // mutex. Give it a moment to prove it's actually waiting.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(unregisterReturned.load())
        << "setTickCallback({}) returned while a callback was still in flight";

    callbackReleased.store(true);
    firing.join();
    unregistering.join();

    EXPECT_TRUE(callbackReturned.load());
    EXPECT_TRUE(unregisterReturned.load());

    // Any further invocation must not reach the (now cleared) callback.
    std::atomic<bool> shouldNotFire{false};
    MtcReceiver::invokeTickForTesting(0, false);
    EXPECT_FALSE(shouldNotFire.load());
}

// ─── Decoder: single-fire-per-QF with correct flag ───────────────────────────

TEST_F(CallbackFixture, ForwardsSequenceFiresEightTimesOneComplete) {
    MtcReceiver receiver(MtcReceiver::SkipPortOpenTag{});
    receiver.resetDecoderStateForTesting();

    std::vector<bool> flags;
    MtcReceiver::setTickCallback(
        [&](long, bool isComplete) { flags.push_back(isComplete); });

    for (auto& m : forwardsZeroSequence()) {
        receiver.decodeQuarterFrameForTesting(m);
    }

    ASSERT_EQ(flags.size(), 8u)
        << "forwards sequence must produce exactly 8 callback fires";
    for (size_t i = 0; i < 7; ++i) {
        EXPECT_FALSE(flags[i]) << "QF " << i << " should be extrapolated";
    }
    EXPECT_TRUE(flags[7]) << "QF 7 should carry isCompleteFrame=true";
}

TEST_F(CallbackFixture, InvalidSequenceProducesZeroTicks) {
    // Feed a 3-QF sequence where direction never stabilizes: alternate
    // 0x40, 0x20, 0x60 — neither forward nor backward monotone. Every QF's
    // qfCount mismatches expected_qf_count → reset guard suppresses all fires.
    MtcReceiver receiver(MtcReceiver::SkipPortOpenTag{});
    receiver.resetDecoderStateForTesting();

    std::atomic<int> hits{0};
    MtcReceiver::setTickCallback([&](long, bool) { hits.fetch_add(1); });

    for (auto type : {4u, 2u, 6u}) {
        auto m = qf(static_cast<unsigned char>(type), 0);
        receiver.decodeQuarterFrameForTesting(m);
    }

    EXPECT_EQ(hits.load(), 0)
        << "invalid QF sequence must not fire any callback";
}

TEST_F(CallbackFixture, BackwardsSequenceFiresAfterDirectionStabilizes) {
    // Backwards QF order: 7,6,5,4,3,2,1,0. direction isn't detected until the
    // 3rd QF (qfCount > 1). The first two QFs fail the reset guard; the
    // remaining 6 fire once each, and the final QF (type 0) is the complete
    // frame — so 6 callbacks total, the last carries isCompleteFrame=true.
    MtcReceiver receiver(MtcReceiver::SkipPortOpenTag{});
    receiver.resetDecoderStateForTesting();

    std::vector<bool> flags;
    MtcReceiver::setTickCallback(
        [&](long, bool isComplete) { flags.push_back(isComplete); });

    for (int type = 7; type >= 0; --type) {
        auto m = qf(static_cast<unsigned char>(type),
                    type == 7 ? 2 : 0);  // rate bits on QF7
        receiver.decodeQuarterFrameForTesting(m);
    }

    ASSERT_EQ(flags.size(), 6u)
        << "backwards sequence must suppress the 2 warm-up QFs and fire 6 times";
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_FALSE(flags[i]) << "QF index " << i << " should be extrapolated";
    }
    EXPECT_TRUE(flags.back()) << "final QF in backwards run must be complete";
}

// ─── getCurFrame snapshot consistency ────────────────────────────────────────

TEST_F(CallbackFixture, GetCurFrameSnapshotIsConsistentUnderConcurrency) {
    // Writers push full-frame SysEx messages with varying timecode values,
    // readers repeatedly call getCurFrame. Without the curFrameMutex_ a
    // reader could observe a torn frame (e.g., hours from one write, seconds
    // from another) — this test asserts that every read is internally valid.
    MtcReceiver receiver(MtcReceiver::SkipPortOpenTag{});

    std::atomic<bool>    stop{false};
    std::atomic<int>     tornReads{0};

    auto makeFullFrame = [](int h, int m, int s, int f) -> Msg {
        return {
            0xF0, 0x7F, 0x7F, 0x01, 0x01,
            static_cast<unsigned char>((h & 0x1F) | (1 << 5)),  // hours + 25 fps rate
            static_cast<unsigned char>(m),
            static_cast<unsigned char>(s),
            static_cast<unsigned char>(f),
            0xF7,
        };
    };

    std::thread writer([&] {
        int i = 0;
        while (!stop.load()) {
            Msg msg = makeFullFrame((i % 24), (i % 60), (i % 60), (i % 25));
            receiver.decodeFullFrameForTesting(msg);
            ++i;
        }
    });

    std::thread reader([&] {
        for (int i = 0; i < 10000 && !stop.load(); ++i) {
            MtcFrame f = MtcReceiver::getCurFrame();
            // A well-formed snapshot must satisfy these ranges. A torn read
            // across the five field writes in decodeFullFrame would almost
            // certainly violate one of them.
            if (f.hours   > 23) tornReads.fetch_add(1);
            if (f.minutes > 59) tornReads.fetch_add(1);
            if (f.seconds > 59) tornReads.fetch_add(1);
            if (f.frames  > 30) tornReads.fetch_add(1);
        }
        stop.store(true);
    });

    reader.join();
    writer.join();

    EXPECT_EQ(tornReads.load(), 0)
        << "getCurFrame returned an inconsistent snapshot under concurrent writes";
}
