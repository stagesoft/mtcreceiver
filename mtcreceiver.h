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
// Stage Lab Cuems MTC receiver class header file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#ifndef MTCRECEIVER_H
#define MTCRECEIVER_H

//////////////////////////////////////////////////////////
// Preprocessor definitions
// #define __LINUX_ALSA__

#define FF_LEN 10
#define QF_LEN 8

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <math.h>
#include <chrono>
#include <vector>
#include <iostream>
#include <iomanip>
#include <rtmidi/RtMidi.h>

// RtMidi API compatibility: newer versions rename LINUX_ALSA -> LINUX_ALSA_SEQ
// Adjust the version threshold below when the rename lands in a released version.
#if defined(RTMIDI_VERSION_MAJOR) && RTMIDI_VERSION_MAJOR >= 7
  static constexpr RtMidi::Api MTCRECV_DEFAULT_API = RtMidi::LINUX_ALSA_SEQ;
#else
  static constexpr RtMidi::Api MTCRECV_DEFAULT_API = RtMidi::LINUX_ALSA;
#endif
#ifdef HAVE_CUEMS_LOGGER
#include "../cuemslogger/cuemslogger.h"
#include "../cuems_errors.h"
#else
// Stub logger for standalone use
namespace CuemsLogger {
    class Logger {
    public:
        static Logger* getLogger() { static Logger instance; return &instance; }
        void logError(const std::string& msg) { std::cerr << "[ERROR] " << msg << std::endl; }
        void logWarning(const std::string& msg) { std::cerr << "[WARNING] " << msg << std::endl; }
        void logInfo(const std::string& msg) { std::cout << "[INFO] " << msg << std::endl; }
    };
    // Static function to match cuemslogger API
    inline Logger* getLogger() { return Logger::getLogger(); }
}
// Stub error code
#define CUEMS_EXIT_NO_MIDI_PORTS_FOUND 1
#endif

using namespace std;


//////////////////////////////////////////////////////////
// MIDI status bytes
enum MidiStatus {

    MIDI_UNKNOWN            = 0x00,

    // channel voice messages
    MIDI_NOTE_OFF           = 0x80,
    MIDI_NOTE_ON            = 0x90,
    MIDI_CONTROL_CHANGE     = 0xB0,
    MIDI_PROGRAM_CHANGE     = 0xC0,
    MIDI_PITCH_BEND         = 0xE0,
    MIDI_AFTERTOUCH         = 0xD0, // aka channel pressure
    MIDI_POLY_AFTERTOUCH    = 0xA0, // aka key pressure

    // system messages
    MIDI_SYSEX              = 0xF0,
    MIDI_TIME_CODE          = 0xF1,
    MIDI_SONG_POS_POINTER   = 0xF2,
    MIDI_SONG_SELECT        = 0xF3,
    MIDI_TUNE_REQUEST       = 0xF6,
    MIDI_SYSEX_END          = 0xF7,
    MIDI_TIME_CLOCK         = 0xF8,
    MIDI_START              = 0xFA,
    MIDI_CONTINUE           = 0xFB,
    MIDI_STOP               = 0xFC,
    MIDI_ACTIVE_SENSING     = 0xFE,
    MIDI_SYSTEM_RESET       = 0xFF
};

//////////////////////////////////////////////////////////
// MTC Frame Rates
enum MtcFrameRate {
    FR_24                   = 0x0,
    FR_25                   = 0x1,
    FR_29                   = 0x2,
    FR_30                   = 0x3
};

//////////////////////////////////////////////////////////
// MTC Frame structure
struct MtcFrame {
	int hours = 0;   //< hours 0-23
	int minutes = 0; //< minutes 0-59
	int seconds = 0; //< seconds 0-59
	int frames = 0;  //< frames 0-29 (depending on framerate)
	unsigned char rate = MtcFrameRate::FR_25; //< 0x0: 24, 0x1: 25, 0x2: 29.97, 0x3: 30

	// Get the framerate value in fps
	float getFps( void ) const;
	// Convert to a string: hh:mm:ss:ff
	std::string toString( void ) const;
	// Convert to time in seconds
	long int toSeconds( void ) const;
	// Convert to time in milliseconds, more precise
    long int toMilliseconds() const;
	// Convert from time in seconds
	void fromSeconds( long int s );
	// Convert from milliseconds to frames due to a given rate
    long int msToFrames( long int ms );
};

class MtcReceiver : public RtMidiIn
{
    public:
        MtcReceiver(    RtMidi::Api api = MTCRECV_DEFAULT_API,
                        const std::string& clientName = "Cuems Mtc Receiver",
                        unsigned int queueSizeLimit = 100,
                        unsigned int portIndex = 0 );
        ~MtcReceiver( void );

        // Quarter-frame tick callback signature.
        //   mtcHeadMs       — current MTC head in ms (extrapolated on QFs 1-7,
        //                     authoritative on QF 8 after full decode).
        //   isCompleteFrame — true only on QF 8 when the full frame has just
        //                     been decoded; false on QFs 1-7 (extrapolated).
        // Fires at most once per valid quarter frame. Does NOT fire when the
        // QF sequence is invalid (reset). The callback MUST be lock-free and
        // non-blocking (<100 µs) — it runs on the RtMidi MIDI callback thread.
        // Callers MUST NOT call setTickCallback() from within the handler.
        using TickCallback = std::function<void(long mtcHeadMs, bool isCompleteFrame)>;

        // Register (or clear, with an empty/null callable) the tick callback.
        // Thread-safe: blocks until any in-flight invocation returns, so after
        // this call returns with an empty callable the previous callback is
        // guaranteed to never fire again (no-call-after-unregister).
        static void setTickCallback(TickCallback cb);

#ifdef MTCRECV_TESTING
        // Test-only helper: synthesize a tick as if decodeQuarterFrame() had
        // just invoked the registered callback. Gated behind MTCRECV_TESTING
        // so production consumers cannot call it. Acquires the same mutex the
        // MIDI thread would, so it also exercises the thread-safety path.
        static void invokeTickForTesting(long mtcHeadMs, bool isCompleteFrame);

        // Tag type used to select the test-only constructor that skips both
        // the port-count check and openPort(), so unit tests can drive the
        // decoder without requiring real MIDI hardware.
        struct SkipPortOpenTag {};
        explicit MtcReceiver(SkipPortOpenTag,
                             RtMidi::Api api = MTCRECV_DEFAULT_API,
                             const std::string& clientName = "Cuems Mtc Receiver Test",
                             unsigned int queueSizeLimit = 100);

        // Public forwarders for the private decode entry points, gated behind
        // MTCRECV_TESTING so production consumers can't accidentally drive the
        // decoder from outside. Used by the unit tests to feed synthetic MIDI
        // byte sequences.
        void decodeQuarterFrameForTesting(std::vector<unsigned char>& msg) {
            decodeQuarterFrame(msg);
        }
        void decodeFullFrameForTesting(std::vector<unsigned char>& msg) {
            decodeFullFrame(msg);
        }

        // Reset all per-instance decoder state (direction, qfCount, flags, the
        // running quarterFrame buffer, and the active-state averaging). Lets
        // each test start from a known state without constructing a new
        // receiver.
        void resetDecoderStateForTesting();

        // Clear the static callback + curFrame state that persists across
        // MtcReceiver instances. Call this from test SetUp to isolate tests.
        static void resetStaticStateForTesting();
#endif

        // Stream control vars
        // These are accessed from both MIDI and audio callback threads, so must be atomic
        static std::atomic<bool> isTimecodeRunning;      // Is the timecode sync running?
        static std::atomic<long int> mtcHead;              // Time code head in milliseconds
        static std::atomic<unsigned char> curFrameRate;  // Current MTC frame rate
        static std::atomic<bool> wasLastUpdateFullFrame; // true if last update was full SYSEX frame (for seeking)
                                           // TODO: Use this in cuems-audioplayer and cuems-dmxplayer for accurate seeking

        // Get current MTC frame (like xjadeo's timecode state). Returns a
        // consistent snapshot (h, m, s, f, rate) taken under curFrameMutex_.
        static MtcFrame getCurFrame();

        // New versions of head position tracking with filtering and extrapolation
        bool isTimecodeActive() const;
        long int estimatedCurrentHead() const;
        
        // Network configuration: adjust timeouts for network transport (rtpmidid)
        // Default values are fine for local MIDI, increase for network
        static void setNetworkMode(bool enabled);  // Sets recommended timeouts for network
        static void setActiveTimeout(long int ms);  // Timeout for isTimecodeActive() (default: 50ms)
        static void setRunningTimeout(double seconds);  // Timeout for isTimecodeRunning (default: 0.1s)
        static void setJumpThreshold(long int ms);  // Threshold for averaging reset (default: 20ms)

    private:
        // MIDI TIMECODE DATA
        // ofxMidiTimecode timecode;        // Timecode message parser
        long int timecodeTimestamp = 0;       // When last quarter frame message was received
        static MtcFrame curFrame;            // Timecode frame data, ie. H M S frame rate (static like mtcHead)
        MtcFrame quarterFrame;              // Last quarter frame received
        int direction = 0;                  // Direction indicator
        unsigned int lastDataByte = 0x00;   // Last quarter received data byte, to recognize dir
        int qfCount = 0;                    // Quarters count
        bool firstQFlag = false;            // First quarter received flag
        bool lastQFlag = false;             // Last quarter received flag

        long int timecodeStartTimestamp = 0;
        long int clientStartTimestamp = 0;
        double timecodeRunWeight = 0.0;

        // Protects the three instance members above (timecodeTimestamp,
        // timecodeStartTimestamp, timecodeRunWeight). Mutable so that the
        // const query methods (isTimecodeActive, estimatedCurrentHead) can
        // acquire it.
        mutable std::mutex activeStateMutex_;

        // Configurable timeouts for network transport
        static long int activeTimeoutNs;      // Default: 50ms (50e6 ns)
        static double runningTimeoutSec;      // Default: 0.1 seconds
        static long int jumpThresholdNs;      // Default: 20ms (20e6 ns)

        // Global callback registration state. tickCallback_ is mutated under
        // callbackMutex_; the mutex is also held during invocation so an
        // unregister call waits for any in-flight callback to return before
        // returning itself (no-call-after-unregister guarantee).
        static std::mutex callbackMutex_;
        static TickCallback tickCallback_;

        // Protects reads/writes of the static curFrame (written in
        // decodeQuarterFrame and decodeFullFrame from the MIDI thread, read
        // via getCurFrame() from any thread).
        static std::mutex curFrameMutex_;

        // Usefull functions
        bool decodeNewMidiMessage( std::vector<unsigned char> &message );
        bool isFullFrame( std::vector<unsigned char> &message );
        void decodeFullFrame( std::vector<unsigned char> &message );
        void decodeQuarterFrame( std::vector<unsigned char> &message) ;

        // RtMidi callback function
        static void midiCallback( double deltatime, std::vector< unsigned char > *message, void *data );

        bool checkerOn = true;
        void threadedChecker( void );

};

#endif // MTCRECEIVER_H
