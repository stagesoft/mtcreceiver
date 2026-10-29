/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * >24h MTC continuity: the wire wraps at 24h (hours 0-23), but mtcHead /
 * estimatedCurrentHead() / the QF tick callbackMs must stay monotonic past 24h.
 * These tests exercise the applyWrap() accumulator and the canonical reset
 * predicate (mirror of the engine MtcListener._apply_24h_offset).
 *
 * Day alignment (12-22): dayShift()/toLocalDay() convert an engine time into
 * this process's day, and the head-authoritative flag that gates them.
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

// ═══ Day alignment ═══════════════════════════════════════════════════════════

namespace {

static_assert(MtcReceiver::DAY_MS == DAY_MS, "one day, one constant");
constexpr long int HALF_DAY = DAY_MS / 2;
constexpr long int HOUR = 3600000L;

void feedQF(MtcReceiver& r, int h, int m, int s, int f, int rate = 1) {
    for (auto& msg : fwdSeq(h, m, s, f, rate)) r.decodeQuarterFrameForTesting(msg);
}

// Put the receiver in day 1: wire wrap seen once, head = DAY_MS + 00:m:s.
void enterDay1(MtcReceiver& r, int m = 0, int s = 0) {
    feedFF(r, 23, 59, 59, 24);
    feedFF(r, 0, 0, 0, 0);
    if (m || s) feedFF(r, 0, m, s, 0);
}

} // namespace

// ─── 12. (k, δ) matrix: aligned → k = 0, engine value untouched ──────────────
TEST(DayShift, AlignedIsIdentity) {
    const long heads[] = {10, 5 * HOUR, DAY_MS + 3 * HOUR, 30 * DAY_MS + 5 * HOUR};
    const long deltas[] = {0, 40, -40, 5000, -5000, HALF_DAY - 1, -HALF_DAY};
    for (long h : heads) {
        for (long d : deltas) {
            EXPECT_EQ(MtcReceiver::dayShift(h + d, h), 0) << "head " << h << " delta " << d;
        }
    }
}

// ─── 13. consumer behind / ahead by n days → k = ±n ──────────────────────────
TEST(DayShift, BehindAndAheadByWholeDays) {
    const long heads[] = {10, 5 * HOUR, DAY_MS + 3 * HOUR, 30 * DAY_MS + 5 * HOUR};
    const long deltas[] = {0, 5000, -5000, HALF_DAY - 1, -HALF_DAY};
    for (long h : heads) {
        for (long n : {1L, 2L, 30L}) {
            for (long d : deltas) {
                // engine ahead of this process (Dipòsit: player born after the crossing)
                EXPECT_EQ(MtcReceiver::dayShift(h + n * DAY_MS + d, h), n)
                    << "head " << h << " n " << n << " delta " << d;
                // engine behind (VC alive across midnight, node engine restarted);
                // engine values go negative here: floor division, not truncation
                EXPECT_EQ(MtcReceiver::dayShift(h - n * DAY_MS + d, h), -n)
                    << "head " << h << " n " << n << " delta " << d;
            }
        }
    }
}

// ─── 14. ties and ±12 h edges ────────────────────────────────────────────────
TEST(DayShift, TiesAndEdges) {
    const long h = 7 * HOUR;
    EXPECT_EQ(MtcReceiver::dayShift(h + HALF_DAY, h), 1);            // +12 h → next day
    EXPECT_EQ(MtcReceiver::dayShift(h - HALF_DAY, h), 0);            // −12 h → same day
    EXPECT_EQ(MtcReceiver::dayShift(h + HALF_DAY - 1, h), 0);
    EXPECT_EQ(MtcReceiver::dayShift(h - HALF_DAY - 1, h), -1);
    EXPECT_EQ(MtcReceiver::dayShift(h + 3 * HALF_DAY, h), 2);        // +36 h
    EXPECT_EQ(MtcReceiver::dayShift(h - 3 * HALF_DAY, h), -1);       // −36 h
    // negative operands straight through the floor division
    EXPECT_EQ(MtcReceiver::dayShift(-1, 0), 0);
    EXPECT_EQ(MtcReceiver::dayShift(-HALF_DAY, 0), 0);
    EXPECT_EQ(MtcReceiver::dayShift(-HALF_DAY - 1, 0), -1);
    EXPECT_EQ(MtcReceiver::dayShift(-DAY_MS, 0), -1);
}

// ─── 15. the result always lands in [head − 12 h, head + 12 h) ───────────────
TEST(DayShift, ResultWindowProperty) {
    const long heads[] = {0, 10, 86399959, DAY_MS, DAY_MS + 3 * HOUR, 30 * DAY_MS + 5 * HOUR};
    for (long h : heads) {
        for (long e = h - 3 * DAY_MS; e <= h + 3 * DAY_MS; e += 7777777L) {
            long k = MtcReceiver::dayShift(e, h);
            long local = e - k * DAY_MS;
            EXPECT_GE(local - h, -HALF_DAY) << "head " << h << " engine " << e;
            EXPECT_LT(local - h, HALF_DAY) << "head " << h << " engine " << e;
        }
    }
}

// ─── 16. wrap race and the Dipòsit case ──────────────────────────────────────
TEST(DayShift, WrapRaceAndDipositCase) {
    // Engine stamped 10 ms before its wrap, a fresh consumer already saw the
    // wire at 00:00:00.010: the start was 20 ms ago (local -10), not 24 h ahead.
    EXPECT_EQ(MtcReceiver::dayShift(86399990L, 10), 1);
    // Dipòsit 10-25 P2: player spawned at wire 00:24:26 after the engine
    // passed 24 h; the engine's start is day 1 + 00:24:28 → 2 s ahead locally.
    EXPECT_EQ(MtcReceiver::dayShift(DAY_MS + 1468000L, 1466000L), 1);
}

// ─── 17. not authoritative before the first complete frame (QF ramp) ─────────
TEST_F(Wrap24hFixture, NotAuthoritativeOnQuarterFrameRamp) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    long local = -7, k = -7;
    EXPECT_FALSE(MtcReceiver::isHeadAuthoritative());
    EXPECT_FALSE(MtcReceiver::toLocalDay(DAY_MS + 5000, local, k));

    auto seq = fwdSeq(1, 0, 0, 0);
    for (size_t i = 0; i + 1 < seq.size(); ++i) r.decodeQuarterFrameForTesting(seq[i]);
    EXPECT_GT(MtcReceiver::mtcHead.load(), 0) << "QF extrapolation ramp moved the head";
    EXPECT_FALSE(MtcReceiver::toLocalDay(DAY_MS + 5000, local, k))
        << "a QF ramp from 0 must never decide k";
    EXPECT_EQ(local, -7);
    EXPECT_EQ(k, -7) << "outputs untouched while not authoritative";

    // The flag is up BEFORE the completing frame's tick fires: a consumer that
    // converts inside the tick callback (gradient-motiond) must succeed there.
    int completeTicks = 0;
    bool tickResolved = false;
    long tickK = -7;
    MtcReceiver::setTickCallback([&](long, bool isComplete) {
        if (!isComplete) return;
        ++completeTicks;
        long l = 0;
        tickResolved = MtcReceiver::toLocalDay(DAY_MS + HOUR + 5000, l, tickK);
    });
    r.decodeQuarterFrameForTesting(seq.back());              // complete: 01:00:00:00
    MtcReceiver::setTickCallback({});
    ASSERT_EQ(completeTicks, 1);
    EXPECT_TRUE(tickResolved);
    EXPECT_EQ(tickK, 1);
    EXPECT_EQ(MtcReceiver::mtcHead.load(), HOUR);
    EXPECT_TRUE(MtcReceiver::isHeadAuthoritative());
    ASSERT_TRUE(MtcReceiver::toLocalDay(HOUR + 5000, local, k));
    EXPECT_EQ(k, 0);
    EXPECT_EQ(local, HOUR + 5000);
    ASSERT_TRUE(MtcReceiver::toLocalDay(DAY_MS + HOUR + 5000, local, k));
    EXPECT_EQ(k, 1);
    EXPECT_EQ(local, HOUR + 5000);
}

// ─── 18. full-frame seek publishes; reset lowers until the next frame ────────
TEST_F(Wrap24hFixture, ResetLowersFlagUntilNextAuthoritativeFrame) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();
    long local = 0, k = 0;

    feedFF(r, 2, 0, 0, 0);
    EXPECT_TRUE(MtcReceiver::isHeadAuthoritative()) << "a full-frame seek is authoritative";

    MtcReceiver::resetWrapOffset();
    EXPECT_FALSE(MtcReceiver::isHeadAuthoritative());
    EXPECT_FALSE(MtcReceiver::toLocalDay(2 * HOUR, local, k));

    feedFF(r, 3, 0, 0, 0);                                    // seek → publish
    ASSERT_TRUE(MtcReceiver::toLocalDay(3 * HOUR + 40, local, k));
    EXPECT_EQ(k, 0);

    MtcReceiver::resetWrapOffset();
    feedQF(r, 3, 0, 1, 0);                                    // complete QF → publish
    EXPECT_TRUE(MtcReceiver::isHeadAuthoritative());
}

// ─── 19. a RESYNC full frame does not raise the flag ─────────────────────────
TEST_F(Wrap24hFixture, ResyncReliesOnEarlierPublish) {
    MtcReceiver::setActiveTimeout(5000);
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    feedQF(r, 0, 0, 1, 0);
    feedQF(r, 0, 0, 2, 0);
    EXPECT_TRUE(MtcReceiver::isHeadAuthoritative());

    MtcReceiver::resetWrapOffset();                           // day 0: domain unchanged
    feedFF(r, 0, 0, 2, 0);                                    // same position → RESYNC
    EXPECT_TRUE(r.isTimecodeActive()) << "classified as a resync";
    EXPECT_FALSE(MtcReceiver::isHeadAuthoritative()) << "RESYNC never publishes";

    feedQF(r, 0, 0, 3, 0);
    EXPECT_TRUE(MtcReceiver::isHeadAuthoritative());

    MtcReceiver::setActiveTimeout(50);
}

// ─── 20. day 1 consumer, engine reset (VC alive, node engine restarted) ──────
TEST_F(Wrap24hFixture, ConsumerAheadOfEngine) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();
    enterDay1(r, 10);                                         // head = DAY + 00:10:00
    ASSERT_EQ(MtcReceiver::mtcHead.load(), DAY_MS + 600000L);

    long local = 0, k = 0;
    ASSERT_TRUE(MtcReceiver::toLocalDay(DAY_MS + 605000L, local, k));   // same day
    EXPECT_EQ(k, 0);
    EXPECT_EQ(local, DAY_MS + 605000L);
    ASSERT_TRUE(MtcReceiver::toLocalDay(605000L, local, k));            // engine in day 0
    EXPECT_EQ(k, -1);
    EXPECT_EQ(local, DAY_MS + 605000L);
}

// ─── 21. reset between applyWrap() and the publish (both decode paths) ───────
TEST_F(Wrap24hFixture, ResetBetweenApplyWrapAndPublishFullFrame) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();
    enterDay1(r);
    ASSERT_TRUE(MtcReceiver::isHeadAuthoritative());

    bool fired = false;
    MtcReceiver::setPublishHookForTesting([&] {
        if (!fired) { fired = true; MtcReceiver::resetWrapOffset(); }
    });
    feedFF(r, 0, 10, 0, 0);                                   // computed in the day-1 domain
    MtcReceiver::setPublishHookForTesting({});
    ASSERT_TRUE(fired);
    EXPECT_EQ(MtcReceiver::mtcHead.load(), DAY_MS + 600000L) << "store unchanged from today";
    EXPECT_FALSE(MtcReceiver::isHeadAuthoritative()) << "pre-reset head must not be authoritative";
    long local = 0, k = 0;
    EXPECT_FALSE(MtcReceiver::toLocalDay(DAY_MS + 605000L, local, k));

    feedFF(r, 0, 10, 1, 0);                                   // next frame, day-0 domain
    EXPECT_EQ(MtcReceiver::mtcHead.load(), 601000L);
    ASSERT_TRUE(MtcReceiver::toLocalDay(DAY_MS + 605000L, local, k));
    EXPECT_EQ(k, 1);
    EXPECT_EQ(local, 605000L);
}

TEST_F(Wrap24hFixture, ResetBetweenApplyWrapAndPublishQuarterFrame) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();
    enterDay1(r);

    bool fired = false;
    MtcReceiver::setPublishHookForTesting([&] {
        if (!fired) { fired = true; MtcReceiver::resetWrapOffset(); }
    });
    feedQF(r, 0, 10, 0, 0);
    MtcReceiver::setPublishHookForTesting({});
    ASSERT_TRUE(fired);
    EXPECT_EQ(MtcReceiver::mtcHead.load(), DAY_MS + 600000L);
    EXPECT_FALSE(MtcReceiver::isHeadAuthoritative());

    feedQF(r, 0, 10, 1, 0);                                   // next frame, day-0 domain
    EXPECT_EQ(MtcReceiver::mtcHead.load(), 601000L);
    EXPECT_TRUE(MtcReceiver::isHeadAuthoritative());
}

// Same race with a real second thread doing the reset while the decoding
// thread is parked between applyWrap() and the publish.
TEST_F(Wrap24hFixture, ResetVsPublishTwoThreads) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();
    enterDay1(r);

    std::atomic<bool> parked{false}, release{false};
    MtcReceiver::setPublishHookForTesting([&] {
        parked.store(true);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
    });
    std::thread decoder([&] { feedFF(r, 0, 10, 0, 0); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!parked.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(parked.load());
    MtcReceiver::resetWrapOffset();                           // the other thread
    release.store(true);
    decoder.join();
    MtcReceiver::setPublishHookForTesting({});

    EXPECT_EQ(MtcReceiver::mtcHead.load(), DAY_MS + 600000L);
    EXPECT_FALSE(MtcReceiver::isHeadAuthoritative());
}

// ─── 22. reader: reset + republish between its loads is detected ─────────────
TEST_F(Wrap24hFixture, ReaderDetectsResetAndRepublish) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();
    enterDay1(r, 10);                                         // head = DAY + 00:10:00

    bool fired = false;
    MtcReceiver::setReaderHookForTesting([&] {
        if (!fired) {
            fired = true;
            MtcReceiver::resetWrapOffset();
            feedFF(r, 0, 20, 0, 0);                           // republished, day 0
        }
    });
    long local = -7, k = -7;
    // The head loaded before the hook is day 1 → k would be 0 → 24 h late.
    EXPECT_FALSE(MtcReceiver::toLocalDay(DAY_MS + 1205000L, local, k));
    MtcReceiver::setReaderHookForTesting({});
    ASSERT_TRUE(fired);
    EXPECT_EQ(local, -7);
    EXPECT_TRUE(MtcReceiver::isHeadAuthoritative()) << "a bare flag would look unchanged";

    ASSERT_TRUE(MtcReceiver::toLocalDay(DAY_MS + 1205000L, local, k));
    EXPECT_EQ(k, 1);
    EXPECT_EQ(local, 1205000L);
}

// Consumer-test pattern: poke mtcHead and the flag directly.
TEST_F(Wrap24hFixture, TestingSetterDrivesToLocalDay) {
    MtcReceiver::mtcHead.store(2 * DAY_MS + HOUR);
    long local = 0, k = 0;
    EXPECT_FALSE(MtcReceiver::toLocalDay(HOUR, local, k));
    MtcReceiver::setHeadAuthoritativeForTesting(true);
    ASSERT_TRUE(MtcReceiver::toLocalDay(HOUR + 40, local, k));
    EXPECT_EQ(k, -2);
    EXPECT_EQ(local, 2 * DAY_MS + HOUR + 40);
}

// Reader, decoder and resets concurrently: a ThreadSanitizer smoke test only,
// like test 11 (no value is checked here; the races are pinned by the
// deterministic tests 21-22). Every decoder pass is a real rollover, so between
// two resets the head climbs one day per pass and k can be any day count.
TEST_F(Wrap24hFixture, ReaderDecoderResetConcurrently) {
    MtcReceiver r(MtcReceiver::SkipPortOpenTag{});
    r.resetDecoderStateForTesting();

    std::atomic<bool> stop{false};
    std::thread decoder([&] {
        while (!stop.load()) {
            feedFF(r, 23, 59, 59, 24);
            feedFF(r, 0, 0, 0, 0);
            feedQF(r, 0, 0, 1, 0);
        }
    });
    std::thread reader([&] {
        long local = 0, k = 0;
        while (!stop.load()) {
            (void)MtcReceiver::toLocalDay(DAY_MS + 1000, local, k);
        }
    });
    for (int i = 0; i < 2000; ++i) MtcReceiver::resetWrapOffset();
    stop.store(true);
    decoder.join();
    reader.join();
}
