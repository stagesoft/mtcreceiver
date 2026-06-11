/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2020-2026 Stage Lab Coop.
 * Authors:
 *   Alex Ramos <alex@stagelab.coop>
 *   Ion Reguera <ion@stagelab.coop>
 *   Adrià Masip <adria@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
// Stage Lab Cuems MTC receiver class source file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include "mtcreceiver.h"

#include <cstdio>
#include <stdexcept>
#include <cstdlib>     // std::llabs — long-diff; <math.h> does not guarantee std::abs(long int)
#include <algorithm>   // std::max

////////////////////////////////////////////
// Initializing static class members
std::atomic<bool> MtcReceiver::isTimecodeRunning(false);
std::atomic<long int> MtcReceiver::mtcHead(0);
std::atomic<unsigned char> MtcReceiver::curFrameRate(25);
std::atomic<bool> MtcReceiver::wasLastUpdateFullFrame(false);
MtcFrame MtcReceiver::curFrame;  // Initialize static curFrame

std::mutex MtcReceiver::callbackMutex_;
MtcReceiver::TickCallback MtcReceiver::tickCallback_;
std::mutex MtcReceiver::curFrameMutex_;

// 24h-wrap continuity accumulator (>24h support). MIDI wire timecode wraps at
// 24h (hours field 0-23); these keep mtcHead/estimatedCurrentHead/callbackMs
// monotonic past 24h. Written from the RtMidi callback thread via applyWrap();
// reset from any thread via resetWrapOffset(); both under wrapStateMutex_.
long int MtcReceiver::wrapOffsetMs_ = 0;
long int MtcReceiver::lastWireMs_ = -1;   // sentinel: no authoritative frame seen yet
std::mutex MtcReceiver::wrapStateMutex_;
static_assert(sizeof(long int) >= 8, "mtcreceiver 24h accumulator needs 64-bit long");

// Configurable timeouts - defaults are for local MIDI
long int MtcReceiver::activeTimeoutNs = 50000000L;    // 50ms
double MtcReceiver::runningTimeoutSec = 0.10;         // 100ms
long int MtcReceiver::jumpThresholdNs = 20000000L;    // 20ms

// Helper function - use steady_clock for monotonic time
static long int ns_now() {
  return chrono::duration_cast<chrono::nanoseconds>(
      chrono::steady_clock::now().time_since_epoch()).count();
}

//////////////////////////////////////////////////////////
// 24h-wrap accumulator. Given the raw WIRE head in ms (0 .. ~86_399_999),
// detect a 24h rollover or a transport reset and return the CONTINUOUS head
// (wire + accumulated 24h offset). Mirrors the engine's
// MtcListener._apply_24h_offset heuristic, in the ms domain (fps-independent:
// 24h = 86_400_000 ms regardless of frame rate).
//
// Called ONLY on authoritative updates (complete quarter-frame and full frame),
// NEVER on per-quarter-frame extrapolation. Detection keys off lastWireMs_,
// which is always the raw WIRE value — never the extrapolated continuous head —
// so a brief extrapolation overshoot past 86_400_000 cannot double-count a wrap.
//
// Lock order: acquires ONLY wrapStateMutex_. Callers MUST NOT hold
// curFrameMutex_ when calling this (compute wireMs inside that lock, release,
// then call applyWrap) — wrapStateMutex_ is never nested inside curFrameMutex_.
long int MtcReceiver::applyWrap(long int wireMs) {
    constexpr long int DAY_MS  = 86400000L;
    constexpr long int HOUR_MS =  3600000L;
    std::lock_guard<std::mutex> lk(wrapStateMutex_);
    if (lastWireMs_ >= 0) {
        long int delta = wireMs - lastWireMs_;
        if (delta < -HOUR_MS) {                       // large backward jump
            if (lastWireMs_ > DAY_MS - HOUR_MS) {     // prev was in the last hour of 24h
                wrapOffsetMs_ += DAY_MS;              // → 24h ROLLOVER: accumulate
            } else if (wireMs < HOUR_MS) {            // new pos near 0 (MANDATORY guard)
                wrapOffsetMs_ = 0;                    // → transport RESET to top
            }
            // else: arbitrary mid-show backward scrub → leave offset unchanged
        }
    }
    lastWireMs_ = wireMs;                             // unconditional (mirrors MtcListener)
    return wireMs + wrapOffsetMs_;
}

//////////////////////////////////////////////////////////
// Clear the accumulated 24h offset (e.g. between projects / on transport reset).
// Resets BOTH wrapOffsetMs_ and lastWireMs_ atomically — zeroing only the offset
// would make the next authoritative frame see a huge negative delta vs the stale
// lastWireMs_ and falsely add another 24h.
void MtcReceiver::resetWrapOffset() {
    std::lock_guard<std::mutex> lk(wrapStateMutex_);
    wrapOffsetMs_ = 0;
    lastWireMs_ = -1;
}

//////////////////////////////////////////////////////////
// Get current MTC frame (like xjadeo's timecode state).
// Returns a consistent snapshot taken under curFrameMutex_.
MtcFrame MtcReceiver::getCurFrame() {
    std::lock_guard<std::mutex> lk(curFrameMutex_);
    return curFrame;
}

//////////////////////////////////////////////////////////
// Register (or clear) the quarter-frame tick callback.
// Holding callbackMutex_ during assignment serializes with the MIDI thread's
// invocation path, so on return (with an empty callable) the previous
// callback is guaranteed to have finished and will not fire again.
void MtcReceiver::setTickCallback(TickCallback cb) {
    std::lock_guard<std::mutex> lk(callbackMutex_);
    tickCallback_ = std::move(cb);
}

#ifdef MTCRECV_TESTING
void MtcReceiver::invokeTickForTesting(long mtcHeadMs, bool isCompleteFrame) {
    std::lock_guard<std::mutex> lk(callbackMutex_);
    if (tickCallback_) tickCallback_(mtcHeadMs, isCompleteFrame);
}

// Test-only ctor: initialize the RtMidi backend but do NOT open any port and
// do NOT start the checker thread. The decoder can be driven directly via
// decodeQuarterFrameForTesting / decodeFullFrameForTesting.
MtcReceiver::MtcReceiver(SkipPortOpenTag,
                         RtMidi::Api api,
                         const std::string& clientName,
                         unsigned int queueSizeLimit) :
    RtMidiIn(api, clientName, queueSizeLimit) {
    clientStartTimestamp = ns_now();
}

void MtcReceiver::resetDecoderStateForTesting() {
    quarterFrame = MtcFrame();
    direction = 0;
    lastDataByte = 0x00;
    qfCount = 0;
    firstQFlag = false;
    lastQFlag = false;
    std::lock_guard<std::mutex> lk(activeStateMutex_);
    timecodeTimestamp = 0;
    timecodeStartTimestamp = 0;
    timecodeRunWeight = 0.0;
}

void MtcReceiver::resetStaticStateForTesting() {
    {
        std::lock_guard<std::mutex> lk(callbackMutex_);
        tickCallback_ = TickCallback{};
    }
    {
        std::lock_guard<std::mutex> lk(curFrameMutex_);
        curFrame = MtcFrame();
    }
    isTimecodeRunning.store(false);
    mtcHead.store(0);
    curFrameRate.store(25);
    wasLastUpdateFullFrame.store(false);
    resetWrapOffset();   // clear 24h accumulator (wrapOffsetMs_ + lastWireMs_)
    // Restore the local-mode timeout defaults too — a test that bumped these
    // (e.g. setNetworkMode/setActiveTimeout) must not bleed into later tests
    // and mis-scale tolMs. (Plan 5 / audit)
    activeTimeoutNs = 50000000L;     // 50ms
    runningTimeoutSec = 0.10;        // 100ms
    jumpThresholdNs = 20000000L;     // 20ms
}
#endif

//////////////////////////////////////////////////////////
MtcReceiver::MtcReceiver( 	RtMidi::Api api,
							const std::string& clientName,
							unsigned int queueSizeLimit,
							unsigned int portIndex) :
							RtMidiIn( api, clientName, queueSizeLimit ) {
    // Check for midi ports available
    if ( RtMidiIn::getPortCount() == 0 ) {
		CuemsLogger::getLogger()->logError("No midi ports found.");
        throw std::runtime_error("MtcReceiver: no MIDI ports available");
    }

	// Set and detach our threaded checker loop
	std::thread checkerThread( &MtcReceiver::threadedChecker, this );
	checkerThread.detach();

    // Set the midi callback function to process incoming midi messages
    RtMidiIn::setCallback( &midiCallback, (void*) this );

    // Don't ignore sysex, timing, or active sensing messages
    RtMidiIn::ignoreTypes( false, false, false );
	CuemsLogger::getLogger()->logInfo("going to open midi port");

    // Then, at last, open midi port (portIndex selects which port to use)
    RtMidiIn::openPort( portIndex, "MTC recv port");

	if (!RtMidiIn::isPortOpen()){
		CuemsLogger::getLogger()->logWarning("first try to open midi port failed, trying again");
		RtMidiIn::openPort( portIndex, "MTC recv port");
	}
    
    clientStartTimestamp = ns_now();
}

//////////////////////////////////////////////////////////
MtcReceiver::~MtcReceiver( void ) {
	checkerOn = false;
    RtMidiIn::closePort();
}

//////////////////////////////////////////////////////////
std::string MtcFrame::toString() const {
	std::stringstream stream;
	stream << std::setw(2) << std::setfill('0') << hours << ":"
	       << std::setw(2) << std::setfill('0') << minutes << ":"
	       << std::setw(2) << std::setfill('0') << seconds << ":"
	       << std::setw(2) << std::setfill('0') << frames;
	return stream.str();
}

//////////////////////////////////////////////////////////
long int MtcFrame::toSeconds() const {
	long int time = hours * 3600.0;
	time += minutes * 60.0;
	time += seconds;
	time += frames / getFps();
	return time;
}

//////////////////////////////////////////////////////////
long int MtcFrame::toMilliseconds() const {
	double time = hours * 3600.0;
	time += minutes * 60.0;
	time += seconds;
	time += frames / getFps();
	time *= 1000;
	return (long int) time;
}

//////////////////////////////////////////////////////////
void MtcFrame::fromSeconds( long int s ) {
	// `s` is whole seconds, so no fractional component to convert into frames.
	seconds = (int)(s % 60);
	minutes = (int)((s / 60) % 60);
	hours   = (int)((s / 3600) % 24);
	frames  = 0;
}

//////////////////////////////////////////////////////////
long int MtcFrame::msToFrames( long int ms ) {
	return (long int)(ms * (getFps() / 1000.0));
}

//////////////////////////////////////////////////////////
float MtcFrame::getFps( void ) const {
	// NOTE: this function returns a float due to the decimals
	// in the 29.97 mode, it could be that this value is somewhere
	// truncated... 
	// TO DO : 	review it carefully in the future to avoid problems
	// 			in different calculus using this function
	switch ( rate ) {
		case FR_24:
			return 24;
		case FR_25:
			return 25;
		case FR_29:
			return 29.97;
		case FR_30:
		default:
			return 30;
	}
}

//////////////////////////////////////////////////////////
// RtMidi callback - Static member function
void MtcReceiver::midiCallback( double deltatime, std::vector< unsigned char > *m, void * data ) {
    MtcReceiver *mtcr = (MtcReceiver*) data;
    std::vector< unsigned char > message = *m;

	// First of all just a small message time gap check
	// Use configurable timeout for network transport compatibility
	if ( deltatime > runningTimeoutSec )
		isTimecodeRunning.store(false);
	else 
		isTimecodeRunning.store(true);

    // So, we have a new mide message and we check whether it is time
    // information and in that case store it in our current frame and 
    // quarter frame data structures
    mtcr->decodeNewMidiMessage(message);
}

//////////////////////////////////////////////////////////
bool MtcReceiver::isFullFrame(std::vector<unsigned char> &message) {
	return
		(message.size() == FF_LEN) && 	// Message length is right
		(message[1] == 0x7F) && 		// universal message
		(message[2] == 0x7F) && 		// global broadcast
		(message[3] == 0x01) && 		// time code
		(message[4] == 0x01) && 		// full frame
		(message[9] == 0xF7);   		// end of sysex
}

//////////////////////////////////////////////////////////
void MtcReceiver::decodeQuarterFrame(std::vector<unsigned char> &message) {
	bool complete = false;
	unsigned char dataByte = message[1];
	unsigned char msgType = dataByte & 0xF0;
	
	if(direction == 0 && qfCount > 1) {
		// If not set direction and we are already counting quarters...
		// let's update the last message type flag
		unsigned char lastMsgType = lastDataByte & 0xF0;
		if(lastMsgType < msgType) {
			// Forwards
			direction = 1;
		}
		else if(lastMsgType > msgType) {
			// Backwards
			direction = -1;
		}
	}

	// Calculate expected quarter frame count for sequence validation
	int n = msgType >> 4; // 0 - 7
	int expected_qf_count = (-1 == direction ? QF_LEN - n : 1 + n);

	switch(msgType) {
		case 0x00: // frame LSB
			quarterFrame.frames = (int)(dataByte & 0x0F);
			qfCount += 1;

			// Yes, it is a first quarter
			firstQFlag = true;

			// Check if we are going backwards, we have all the
			// quarters and so we have a complete MTC code
			if(qfCount >= QF_LEN && direction == -1 && lastQFlag) {
				complete = true;
			}
			break;
		case 0x10: // frame MSB
			quarterFrame.frames |= (int)((dataByte & 0x01) << 4);
			qfCount += 1;
			break;
		case 0x20: // second LSB
			quarterFrame.seconds = (int)(dataByte & 0x0F);
			qfCount += 1;
			break;
		case 0x30: // second MSB
			quarterFrame.seconds |= (int)((dataByte & 0x03) << 4);
			qfCount += 1;
			break;
		case 0x40: // minute LSB
			quarterFrame.minutes = (int)(dataByte & 0x0F);
			qfCount += 1;
			break;
		case 0x50: // minute MSB
			quarterFrame.minutes |= (int)((dataByte & 0x03) << 4);
			qfCount += 1;
			break;
		case 0x60: // hours LSB
			quarterFrame.hours = (int)(dataByte & 0x0F);
			qfCount += 1;
			break;
		case 0x70: // hours MSB & framerate
			quarterFrame.hours |= (int)((dataByte & 0x01) << 4);
			quarterFrame.rate = (dataByte & 0x06) >> 1;
			qfCount += 1;

			// Yes, it is a last quarter 
			lastQFlag = true;

			// Check if we are going forwards, we have all the
			// quarters and so we have a complete MTC code
			if( qfCount >= QF_LEN && direction == 1 && firstQFlag ) {
				complete = true;
			}
			break;
		default:
			return;
	}

	// Anton: we should only update the head on valid quarter frames,
	//        so I moved this section after the above switch.
	// Each time we process a quarter we assume that the head is
	// stil going on...
	// TO DO : adjust for both directions
	// 1/4 * 1000 milliseconds * (1 / framerate)
	lastDataByte = dataByte;

	bool reset = (qfCount != expected_qf_count);

	// Extrapolated head advance, used when the frame isn't yet complete.
	// curFrame.getFps() is safe to read here without the mutex: this thread
	// is the only writer, and readers of curFrame already go through the
	// locked getCurFrame() path.
	mtcHead.store(mtcHead.load() + static_cast<long int>(250 / curFrame.getFps()));
	long int callbackMs = mtcHead.load();

	// Update time using the (hopefully) complete message.
	if (complete) {
		{
			std::lock_guard<std::mutex> lk(curFrameMutex_);
			curFrame = quarterFrame;
		}

		// We have complete valid MTC time info so, we can update
		// our MTC head position to the authoritative value. Route it through
		// the 24h-wrap accumulator so mtcHead — and this callbackMs, which
		// drives the tick callback (fade engine) at :383 — stay CONTINUOUS past
		// the 24h wire wrap. curFrameMutex_ is NOT held here (its scope ended
		// above) so applyWrap()'s wrapStateMutex_ is not nested inside it.
		callbackMs = applyWrap(curFrame.toMilliseconds());
		mtcHead.store(callbackMs);
		curFrameRate.store(static_cast<unsigned char>(curFrame.getFps()));
		wasLastUpdateFullFrame.store(false); // Quarter-frame update (not full SYSEX)

		// Reset quarter frame structure and detection flags
		quarterFrame = MtcFrame();
		direction = 0;
		qfCount = 0;
		lastQFlag = firstQFlag = false;
	}

	// Single fire per QF: one invocation carrying a flag that distinguishes
	// extrapolated ticks (QFs 1-7) from the authoritative tick (QF 8 after
	// full decode). Suppressed entirely when the QF sequence is invalid so
	// consumers never see phantom ticks.
	if (!reset) {
		std::lock_guard<std::mutex> lk(callbackMutex_);
		if (tickCallback_) tickCallback_(callbackMs, complete);
	}

	// Note down the timestamp when the last QFrame message arrived, and run
	// the averaging logic under activeStateMutex_ so const query methods
	// (isTimecodeActive / estimatedCurrentHead) see a consistent snapshot.
	{
		std::lock_guard<std::mutex> lk(activeStateMutex_);

		timecodeTimestamp = ns_now();

		// W_MAX controls maximum averaging length.
		// Higher values give better accuracy but slower adaptation.
		constexpr const double W_MAX = 100.0;
		long int ts_start = timecodeTimestamp - mtcHead.load() * static_cast<long int>(1e6);

		// Reset accumulated average if time jumps or quarter frame sequence is broken
		// Use configurable threshold for network transport compatibility
		if (reset || (ts_start - timecodeStartTimestamp > jumpThresholdNs) || (0.0 == timecodeRunWeight)) {
			timecodeStartTimestamp = ts_start;
			if (complete) {
				timecodeRunWeight = 1.0;
#ifdef MTCRECV_VERBOSE_DIAG
				fprintf(stderr, "MTC Receiver: set start time: %.3f\n",
					(ts_start - clientStartTimestamp) * 1e-9);
#endif
			}
			else {
#ifdef MTCRECV_VERBOSE_DIAG
				if (0 < timecodeRunWeight) {
					fprintf(stderr, "MTC Receiver: reset start time (QFrame): %.3f\n",
						(ts_start - clientStartTimestamp) * 1e-9);
				}
#endif
				timecodeRunWeight = 0.0;
			}
		}
		else {
			// Regular correction
			timecodeRunWeight += (1 - timecodeRunWeight / W_MAX);
			timecodeStartTimestamp += static_cast<long int>((ts_start - timecodeStartTimestamp) / timecodeRunWeight);
		}
	}
}

//////////////////////////////////////////////////////////
void MtcReceiver::decodeFullFrame(std::vector<unsigned char> &message) {
	// Capture the continuous head BEFORE overwriting it, to classify this full
	// frame as a resync (position unchanged) vs a real seek (position jump).
	long int oldHead = mtcHead.load();

	// Compute the raw wire head + fps INSIDE curFrameMutex_, then release it
	// before calling applyWrap() — wrapStateMutex_ is never nested inside
	// curFrameMutex_ (lock-order rule).
	long int wireMs;
	double fps;
	{
		std::lock_guard<std::mutex> lk(curFrameMutex_);
		curFrame.hours = (int)(message[5] & 0x1F);
		curFrame.rate = (int)((message[5] & 0x60) >> 5);
		curFrame.minutes = (int)(message[6]);
		curFrame.seconds = (int)(message[7]);
		curFrame.frames = (int)(message[8]);
		wireMs = curFrame.toMilliseconds();
		fps = curFrame.getFps();
	}

	// A full message is always valid whole MTC time info. Route through the
	// 24h-wrap accumulator so mtcHead stays continuous past 24h.
	long int continuousMs = applyWrap(wireMs);
	mtcHead.store(continuousMs);
	wasLastUpdateFullFrame.store(true); // Full SYSEX frame marker (like xjadeo's tick=0)

	// Position-aware resync handling. libmtcmaster (jitter_improvement) sends a
	// periodic full frame (~every 2s) at the CURRENT position. A resync matches
	// where the quarter-frames already advanced us; a real seek does not.
	// Tolerance scales with the active-timeout so network extrapolation jitter
	// doesn't misclassify a late resync as a seek (floored at ~2 frames for
	// local mode). std::llabs — both operands are long int (NOT std::abs).
	long int tolMs = std::max<long int>(
		static_cast<long int>(2000.0 / (fps > 0.0 ? fps : 25.0)),
		activeTimeoutNs * 8 / 10 / 1000000L);
	// Strict `<`: at the exact-equality boundary (|delta| == tolMs, the 2-frame
	// budget) classify as a SEEK (re-anchor averaging), the safer default when
	// the QF extrapolation overshoot exactly equals the tolerance. (Plan 5 / audit)
	bool isResync = std::llabs(continuousMs - oldHead) < tolMs;

	if (!isResync) {
		// Real seek → reset the averaging for the new position (BOTH fields
		// together under activeStateMutex_).
		std::lock_guard<std::mutex> lk(activeStateMutex_);
		timecodeStartTimestamp = ns_now() - mtcHead.load() * static_cast<long int>(1e6);
		timecodeRunWeight = 0.0;
#ifdef MTCRECV_VERBOSE_DIAG
		fprintf(stderr, "MTC Receiver: Reset start time (FFrame seek): %.3f\n",
			(timecodeStartTimestamp - clientStartTimestamp) * 1e-9);
#endif
	}
	// On a resync we INTENTIONALLY leave timecodeStartTimestamp and
	// timecodeRunWeight untouched. Previously every full frame zeroed the
	// weight → isTimecodeActive() flips false; with the ~2s periodic resync that
	// would gate dmxplayer scene output off every 2s. Keeping them stable on a
	// resync preserves isTimecodeActive()==true (intentional contract change).

	// A full frame is a seek / resync: any in-flight quarter-frame sequence
	// is now stale. Clear the decoder state so the next QF stream starts
	// fresh — otherwise direction / qfCount / firstQFlag / lastQFlag leak
	// from before the seek and invalidate the sequence-validity check.
	quarterFrame = MtcFrame();
	direction = 0;
	qfCount = 0;
	lastQFlag = firstQFlag = false;
	lastDataByte = 0x00;
}

//////////////////////////////////////////////////////////
bool MtcReceiver::decodeNewMidiMessage( std::vector<unsigned char> &message ) {
    // Is it a time codification frame??
	if( message[0] == MIDI_TIME_CODE ) {
        // A MTC quarter frame?
		decodeQuarterFrame(message);
		return true;
	}
	else if( message[0] == MIDI_SYSEX && isFullFrame(message) ) {
        // A SysEx full frame?
        decodeFullFrame(message);
		return true;
	}

    return false;
}

//////////////////////////////////////////////////////////
bool MtcReceiver::isTimecodeActive() const
{
	std::lock_guard<std::mutex> lk(activeStateMutex_);
	if (0.0 == timecodeRunWeight) {
		return false;
	}
	// Check if we got a quarter frame within the configured timeout
	// Use configurable timeout for network transport compatibility
	return (std::abs(ns_now() - timecodeTimestamp) < activeTimeoutNs);
}

//////////////////////////////////////////////////////////
long int MtcReceiver::estimatedCurrentHead() const
{
	std::lock_guard<std::mutex> lk(activeStateMutex_);
	// If MTC was stopped with FFrame or reset, return the last position
	if (0.0 == timecodeRunWeight) {
		return mtcHead.load();
	}
	// Otherwise return extrapolated time (we have to convert ns to ms below)
	return static_cast<long int>((ns_now() - timecodeStartTimestamp) * 1e-6);
}

//////////////////////////////////////////////////////////
void MtcReceiver::threadedChecker( void ) {
	while ( checkerOn ) {
		if ( isTimecodeRunning.load() ) {
			long int timecodeNow = ns_now();
			long int ts;
			{
				std::lock_guard<std::mutex> lk(activeStateMutex_);
				ts = timecodeTimestamp;
			}
			long int timecodeDiff = (timecodeNow - ts);

			// Use configurable timeout (convert to ns for comparison)
			if ( timecodeDiff > activeTimeoutNs )
				isTimecodeRunning.store(false);
		}

		std::this_thread::sleep_for( std::chrono::milliseconds(20) );
	}
}

//////////////////////////////////////////////////////////
// Network configuration setters
void MtcReceiver::setNetworkMode(bool enabled) {
	if (enabled) {
		// Recommended settings for network transport (rtpmidid)
		activeTimeoutNs = 150000000L;    // 150ms - accounts for network jitter
		runningTimeoutSec = 0.20;        // 200ms - more tolerant of gaps
		jumpThresholdNs = 50000000L;     // 50ms - network can cause larger jumps
#ifdef MTCRECV_VERBOSE_DIAG
		fprintf(stderr, "MTC Receiver: Network mode enabled (150ms timeout, 50ms jump threshold)\n");
#endif
	} else {
		// Default settings for local MIDI
		activeTimeoutNs = 50000000L;     // 50ms
		runningTimeoutSec = 0.10;        // 100ms
		jumpThresholdNs = 20000000L;     // 20ms
#ifdef MTCRECV_VERBOSE_DIAG
		fprintf(stderr, "MTC Receiver: Local mode (50ms timeout, 20ms jump threshold)\n");
#endif
	}
}

void MtcReceiver::setActiveTimeout(long int ms) {
	activeTimeoutNs = ms * 1000000L;
}

void MtcReceiver::setRunningTimeout(double seconds) {
	runningTimeoutSec = seconds;
}

void MtcReceiver::setJumpThreshold(long int ms) {
	jumpThresholdNs = ms * 1000000L;
}
