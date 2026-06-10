/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * >24h MTC continuity: the wire wraps at 24h (hours 0-23), but mtcHead /
 * estimatedCurrentHead() / the QF tick callbackMs must stay monotonic past 24h.
 * These tests exercise the applyWrap() accumulator and the canonical reset
 * predicate (mirror of the engine MtcListener._apply_24h_offset).
 */

#include "mtcreceiver.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

using Msg = std::vector<unsigned char>;

constexpr long int DAY_MS  = 86400000L;
// 25 fps, last frame before wrap (23:59:59:24): ~86_399_959 ms. NOTE
// MtcFrame::toMilliseconds() truncates the FP product (24/25*1000), so it is
// 86399959, not 86399960 — pre-wrap assertions use a range, not an exact value.
// The wrap-relevant assertions (post-wrap head) are exact (applyWrap(0)=offset).
constexpr long int PRE_WRAP_MIN = 86399900L;

// Full-frame SYSEX for h:m:s:f at a given MTC rate (0=24,1=25,2=29.97,3=30).
Msg fullFrame(int h, int m, int s, int f, int rate = 1) {
    return {
        0xF0, 0x7F, 0x7F, 0x01, 0x01,
        static_cast<unsigned char>((h & 0x1F) | (rate << 5)),
        static_cast<unsigned char>(m),
        static_cast<unsigned char>(s),
        static_cast<unsigned char>(f),
        0xF7,
    };
}

Msg qf(unsigned char type, unsigned char value) {
    return { 0xF1, static_cast<unsigned char>((type << 4) | (value & 0x0F)) };
}

// A forward QF sequence (types 0-7) encoding h:m:s:f at the given rate. Drives
// the authoritative complete-frame path (and the per-QF tick callback).
std::vector<Msg> fwdSeq(int h, int m, int s, int f, int rate = 1) {
    return {
        qf(0, f & 0x0F),
        qf(1, (f >> 4) & 0x01),
        qf(2, s & 0x0F),
        qf(3, (s >> 4) & 0x03),
        qf(4, m & 0x0F),
        qf(5, (m >> 4) & 0x03),
        qf(6, h & 0x0F),
        qf(7, static_cast<unsigned char>(((h >> 4) & 0x01) | (rate << 1))),
    };
}

class Wrap24hFixture : public ::testing::Test {
protected:
    void SetUp() override { MtcReceiver::resetStaticStateForTesting(); }
    void TearDown() override { MtcReceiver::resetStaticStateForTesting(); }
};

// Feed a full frame through the (test-only) decode entry point.
void feedFF(MtcReceiver& r, int h, int m, int s, int f, int rate = 1) {
    Msg msg = fullFrame(h, m, s, f, rate);
    r.decodeFullFrameForTesting(msg);
}

} // namespace

// ─── 1. Single rollover ──────────────────────────────────────────────────────
TEST_F(Wrap24hFixture, SingleRolloverIsContinuous) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    feedFF(r, 23, 59, 59, 24);
    long pre = MtcReceiver::mtcHead.load();
    EXPECT_GE(pre, PRE_WRAP_MIN);
    EXPECT_LT(pre, DAY_MS);

    feedFF(r, 0, 0, 0, 0);                       // wire wraps to 0
    EXPECT_EQ(MtcReceiver::mtcHead.load(), DAY_MS)
        << "head must be continuous (86_400_000), not ~0";
}

// ─── 2. Double rollover ──────────────────────────────────────────────────────
TEST_F(Wrap24hFixture, DoubleRolloverAccumulatesTwice) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    feedFF(r, 23, 59, 59, 24);
    feedFF(r, 0, 0, 0, 0);                        // wrap 1
    feedFF(r, 23, 59, 59, 24);
    feedFF(r, 0, 0, 0, 0);                        // wrap 2
    EXPECT_EQ(MtcReceiver::mtcHead.load(), 2 * DAY_MS);
}

// ─── 3. Reset predicate (corrected scenario) ─────────────────────────────────
TEST_F(Wrap24hFixture, ResetPredicateZeroesOffsetFromNonBoundary) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    feedFF(r, 23, 59, 59, 24);
    feedFF(r, 0, 0, 0, 0);                        // wrap → offset = 24h
    feedFF(r, 2, 0, 0, 0);                        // advance to 2h post-wrap (NOT near boundary)
    EXPECT_EQ(MtcReceiver::mtcHead.load(), DAY_MS + 7200000L);

    feedFF(r, 0, 0, 0, 0);                        // FF→0 from non-boundary → RESET
    EXPECT_EQ(MtcReceiver::mtcHead.load(), 0L)
        << "transport reset to top must zero the offset";
}

TEST_F(Wrap24hFixture, FFToZeroNearBoundaryIsWrapNotReset) {
    // Distinguish the two branches: FF→0 while prev is near the 24h boundary
    // must WRAP, not reset.
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();
    feedFF(r, 23, 59, 59, 24);
    feedFF(r, 0, 0, 0, 0);
    EXPECT_EQ(MtcReceiver::mtcHead.load(), DAY_MS);   // wrapped, not 0
}

// ─── 4. Mid-show backward scrub leaves the offset unchanged ───────────────────
TEST_F(Wrap24hFixture, MidShowScrubDoesNotChangeOffset) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    feedFF(r, 10, 0, 0, 0);                       // 10h, offset 0
    EXPECT_EQ(MtcReceiver::mtcHead.load(), 36000000L);
    feedFF(r, 8, 0, 0, 0);                        // scrub back to 8h (prev not near boundary, new not <1h)
    EXPECT_EQ(MtcReceiver::mtcHead.load(), 28800000L)
        << "a mid-show backward scrub must follow the wire, not wrap/reset";
}

// ─── 5. resetWrapOffset() clears BOTH fields ──────────────────────────────────
TEST_F(Wrap24hFixture, ResetWrapOffsetClearsOffsetAndLastWire) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    feedFF(r, 23, 59, 59, 24);
    feedFF(r, 0, 0, 0, 0);                        // offset = 24h, lastWire = 0
    EXPECT_EQ(MtcReceiver::mtcHead.load(), DAY_MS);

    MtcReceiver::resetWrapOffset();               // offset = 0, lastWire = -1
    feedFF(r, 2, 0, 0, 0);                        // must NOT re-wrap off a stale lastWire
    EXPECT_EQ(MtcReceiver::mtcHead.load(), 7200000L);
}

// ─── 6. Extrapolation-cross: exactly one wrap ─────────────────────────────────
TEST_F(Wrap24hFixture, ExtrapolationPast24hCountsOneWrap) {
    // FF sets the head to 86_399_960; the QF extrapolation (+10ms/QF @25fps)
    // then pushes the *continuous* head past 86_400_000 before the authoritative
    // 00:00:00:00 complete frame arrives. Detection keys off the raw wire
    // lastWireMs_ (86_399_960), not the extrapolated head, so exactly ONE wrap.
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    feedFF(r, 23, 59, 59, 24);                    // head = 86_399_960, lastWire = 86_399_960
    for (auto& m : fwdSeq(0, 0, 0, 0)) {          // forward QF run completing at 00:00:00:00
        r.decodeQuarterFrameForTesting(m);
    }
    EXPECT_EQ(MtcReceiver::mtcHead.load(), DAY_MS)
        << "exactly one wrap (86_400_000), not a double-count (172_800_000)";
}

// ─── 7. callbackMs continuity (fade-engine tick path) ─────────────────────────
TEST_F(Wrap24hFixture, TickCallbackMsIsContinuousAcrossWrap) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    long lastCompleteMs = -1;
    MtcReceiver::setTickCallback([&](long ms, bool isComplete) {
        if (isComplete) lastCompleteMs = ms;
    });

    for (auto& m : fwdSeq(23, 59, 59, 24)) r.decodeQuarterFrameForTesting(m);
    EXPECT_GE(lastCompleteMs, PRE_WRAP_MIN);
    EXPECT_LT(lastCompleteMs, DAY_MS);

    for (auto& m : fwdSeq(0, 0, 0, 0)) r.decodeQuarterFrameForTesting(m);
    EXPECT_EQ(lastCompleteMs, DAY_MS)
        << "the complete-frame tick (fade engine) must be continuous, not ~0";

    MtcReceiver::setTickCallback({});
}

// ─── 8/9. Resync keeps isTimecodeActive() true; real seek clears it ───────────
TEST_F(Wrap24hFixture, ResyncKeepsTimecodeActiveSeekClearsIt) {
    MtcReceiver::setActiveTimeout(5000);          // 5s, avoid timing flakiness
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    // Build weight with a couple of forward complete frames.
    for (auto& m : fwdSeq(0, 0, 1, 0)) r.decodeQuarterFrameForTesting(m);
    for (auto& m : fwdSeq(0, 0, 2, 0)) r.decodeQuarterFrameForTesting(m);
    EXPECT_TRUE(r.isTimecodeActive());

    feedFF(r, 0, 0, 2, 0);                         // resync at current position
    EXPECT_TRUE(r.isTimecodeActive())
        << "a same-position resync must NOT deactivate the timecode (dmxplayer gate)";

    feedFF(r, 5, 0, 0, 0);                         // real seek (far)
    EXPECT_FALSE(r.isTimecodeActive())
        << "a real seek re-anchors averaging (weight 0) → inactive until QFs resume";

    MtcReceiver::setActiveTimeout(50);             // restore default
}

// ─── 10. 29.97 DF: post-wrap head exact, pre-wrap one-frame step ──────────────
TEST_F(Wrap24hFixture, Rollover2997PostWrapExact) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    feedFF(r, 23, 59, 59, 29, /*rate=*/2);         // 29.97
    long pre = MtcReceiver::mtcHead.load();
    EXPECT_GE(pre, 86399900L);
    EXPECT_LT(pre, DAY_MS);                         // one-frame step below the boundary

    feedFF(r, 0, 0, 0, 0, /*rate=*/2);
    EXPECT_EQ(MtcReceiver::mtcHead.load(), DAY_MS)  // applyWrap(0) = exactly one DAY
        << "post-wrap head is exact regardless of fps";
}

// ─── 11. resetWrapOffset() racing a decode is safe (TSan) ─────────────────────
TEST_F(Wrap24hFixture, ResetRacingDecodeIsThreadSafe) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    std::atomic<bool> stop{false};
    std::thread decoder([&] {
        int i = 0;
        while (!stop.load()) {
            feedFF(r, (i % 24), 0, 0, 0);
            ++i;
        }
    });
    for (int i = 0; i < 2000; ++i) MtcReceiver::resetWrapOffset();
    stop.store(true);
    decoder.join();
    SUCCEED();   // invariant: no data race / crash under TSan
}
