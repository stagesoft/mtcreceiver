/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 */

#include "mtcreceiver.h"

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

namespace {

using Msg = std::vector<unsigned char>;

Msg qf(unsigned char type, unsigned char value) {
    return { 0xF1, static_cast<unsigned char>((type << 4) | (value & 0x0F)) };
}

// 8 quarter-frame messages for wire-MTC hh=00 mm=00 ss=04 ff=00 @ 25 fps.
// Each 4-bit value carries the corresponding LSB/MSB nibble of h/m/s/f, and
// QF 7 carries (rate_code << 1) | hours_msb_bit where rate_code 1 = 25 fps.
std::vector<Msg> fourSecondsZeroFramesAt25Fps() {
    return {
        qf(0, 0),  // frames  LSB (ff = 0)
        qf(1, 0),  // frames  MSB
        qf(2, 4),  // seconds LSB (ss = 4)
        qf(3, 0),  // seconds MSB
        qf(4, 0),  // minutes LSB
        qf(5, 0),  // minutes MSB
        qf(6, 0),  // hours   LSB
        qf(7, 2),  // hours MSB + rate: (1 << 1) | 0 = 25 fps, hours MSB = 0
    };
}

class NoPlusTwoFixture : public ::testing::Test {
protected:
    void SetUp() override {
        MtcReceiver::resetStaticStateForTesting();
    }
    void TearDown() override {
        MtcReceiver::setTickCallback({});
        MtcReceiver::resetStaticStateForTesting();
    }
};

} // namespace

// The callback value on the complete-frame tick must equal wire-MTC, not
// wire-MTC + 80 ms. Pre-fix (when decodeQuarterFrame had `quarterFrame.frames
// += 2;`) the callback fired at 4080 ms for this sequence; post-fix it fires
// at 4000 ms.
TEST_F(NoPlusTwoFixture, CompleteFrameTickEqualsWireMtc) {
    MtcReceiver receiver(MtcReceiver::SkipPortOpenTag{});
    receiver.resetDecoderStateForTesting();

    std::atomic<long> observedMs{-1};
    std::atomic<bool> completeSeen{false};

    MtcReceiver::setTickCallback([&](long ms, bool isComplete) {
        if (isComplete) {
            observedMs.store(ms);
            completeSeen.store(true);
        }
    });

    for (auto& m : fourSecondsZeroFramesAt25Fps()) {
        receiver.decodeQuarterFrameForTesting(m);
    }

    ASSERT_TRUE(completeSeen.load())
        << "No complete-frame tick fired — QF sequence did not assemble";
    EXPECT_EQ(observedMs.load(), 4000L)
        << "Post-fix mtcHead must equal wire-MTC (no +2 frame / 80 ms bias). "
        << "Observed 4080 means `quarterFrame.frames += 2;` was re-introduced "
        << "in decodeQuarterFrame.";
}
