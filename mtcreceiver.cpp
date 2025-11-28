/* LICENSE TEXT

    audioplayer for linux based using RtAudio and RtMidi libraries to
    process audio and receive MTC sync. It also uses oscpack to receive
    some configurations through osc commands.
    Copyright (C) 2020  Stage Lab & bTactic.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/

//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
// Stage Lab Cuems MTC receiver class source file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include "mtcreceiver.h"

////////////////////////////////////////////
// Initializing static class members
std::atomic<bool> MtcReceiver::isTimecodeRunning(false);
std::atomic<long int> MtcReceiver::mtcHead(0);
std::atomic<unsigned char> MtcReceiver::curFrameRate(25);
bool MtcReceiver::wasLastUpdateFullFrame = false;
MtcFrame MtcReceiver::curFrame;  // Initialize static curFrame

// Helper function - use steady_clock for monotonic time
static long int ns_now() {
  return chrono::duration_cast<chrono::nanoseconds>(
      chrono::steady_clock::now().time_since_epoch()).count();
}

//////////////////////////////////////////////////////////
// Get current MTC frame (like xjadeo's timecode state)
MtcFrame MtcReceiver::getCurFrame() {
    return curFrame;
}

//////////////////////////////////////////////////////////
MtcReceiver::MtcReceiver( 	RtMidi::Api api, 
							const std::string& clientName,
							unsigned int queueSizeLimit) :
							RtMidiIn( api, clientName, queueSizeLimit ) {
    // Check for midi ports available
    if ( RtMidiIn::getPortCount() == 0 ) {
		CuemsLogger::getLogger()->logError("No midi ports found.");

        exit(CUEMS_EXIT_NO_MIDI_PORTS_FOUND);
    }

	// Set and detach our threaded checker loop
	std::thread checkerThread( &MtcReceiver::threadedChecker, this );
	checkerThread.detach();

    // Set the midi callback function to process incoming midi messages
    RtMidiIn::setCallback( &midiCallback, (void*) this );

    // Don't ignore sysex, timing, or active sensing messages
    RtMidiIn::ignoreTypes( false, false, false );

    // Then, at last, open midi default port
    RtMidiIn::openPort( 0, "MTC recv port");

	if (!RtMidiIn::isPortOpen()){
		CuemsLogger::getLogger()->logError("first try to open midi port failed, trying again");
		RtMidiIn::openPort( 0, "MTC recv port");
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
	seconds = (int)s % 60;
	minutes = (int)( (s - seconds) * (1 / 60) ) % 60;
	hours = (int)(s * (1 / 3600) ) % 60;

	// round fractional part of seconds for ms
	long int ms = (int)(floor((s - floor(s)) * 1000.0) + 0.5);
	frames = msToFrames(ms);
}

//////////////////////////////////////////////////////////
long int MtcFrame::msToFrames( long int ms ) {
	return ( ms / ( getFps() / 1000.0) );
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
	if ( deltatime > 0.10 )
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
	mtcHead.store(mtcHead.load() + static_cast<long int>(250 / curFrame.getFps()));

	// Updating lastDataByte flag
	lastDataByte = dataByte;

	bool reset = (qfCount != expected_qf_count);

	// Update time using the (hopefully) complete message
	if(complete) {
		// Add a 2 frames adjust to compensate the time 
		// it takes to receive all 8 QF messages
		quarterFrame.frames += 2;

		// Our current frame is the current quarter frame structure
		curFrame = quarterFrame;

		// We have complete valid MTC time info so, we can update 
		// our MTC head position
		mtcHead.store(curFrame.toMilliseconds());
		curFrameRate.store(static_cast<unsigned char>(curFrame.getFps()));
		wasLastUpdateFullFrame = false; // Quarter-frame update (not full SYSEX)

		// Reset quarter frame structure and detection flags
		quarterFrame = MtcFrame();
		direction = 0;
		qfCount = 0;
		lastQFlag = firstQFlag = false;
	}

	// Note down the timestamp when the last QFrame message arrived
	timecodeTimestamp = ns_now();

	// W_MAX controls maximum averaging length.
	// Higher values give better accuracy but slower adaptation.
	constexpr const double W_MAX = 100.0;
	long int ts_start = timecodeTimestamp - mtcHead.load() * static_cast<long int>(1e6);

	// Reset accumulated average if time jumps or quarter frame sequence is broken
	if (reset || (ts_start - timecodeStartTimestamp > 20e6) || (0.0 == timecodeRunWeight)) {
		timecodeStartTimestamp = ts_start;
		if (complete) {
			timecodeRunWeight = 1.0;

			std::cout << std::fixed << std::setprecision(3);
			std::cout << "MTC Receiver: set start time: "
				<< (ts_start - clientStartTimestamp) * 1e-9
				<< std::endl;
		}
		else {
			if (0 < timecodeRunWeight) {
				std::cout << std::fixed << std::setprecision(3);
				std::cout << "MTC Receiver: reset start time (QFrame): "
					<< (ts_start - clientStartTimestamp) * 1e-9
					<< std::endl;
			}
			timecodeRunWeight = 0.0;
		}
	}
	else {
		// Regular correction
		timecodeRunWeight += (1 - timecodeRunWeight / W_MAX);
		timecodeStartTimestamp += static_cast<long int>((ts_start - timecodeStartTimestamp) / timecodeRunWeight);
	}
}

//////////////////////////////////////////////////////////
void MtcReceiver::decodeFullFrame(std::vector<unsigned char> &message) {
	curFrame.hours = (int)(message[5] & 0x1F);
	curFrame.rate = (int)((message[5] & 0x60) >> 5);
	curFrame.minutes = (int)(message[6]);
	curFrame.seconds = (int)(message[7]);
	curFrame.frames = (int)(message[8]);

	// A full message is always valid whole MTC time info so
	// we can update our MTC head position
	wasLastUpdateFullFrame = true; // Full SYSEX frame marker (like xjadeo's tick=0)
	mtcHead.store(curFrame.toMilliseconds());

	// Reset the averaging for the new position
	timecodeStartTimestamp = ns_now() - mtcHead.load() * static_cast<long int>(1e6);
	timecodeRunWeight = 0.0;
	std::cout << "MTC Receiver: Reset start time (FFrame): "
		<< (timecodeStartTimestamp - clientStartTimestamp) * 1e-9 << std::endl;
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
	if (0.0 == timecodeRunWeight) {
		return false;
	}
	// Check if we got a quarter frame in less than 50 ms
	return (std::abs(ns_now() - timecodeTimestamp) < 50e6);
}

//////////////////////////////////////////////////////////
long int MtcReceiver::estimatedCurrentHead() const
{
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
			
			long int timecodeDiff = (timecodeNow - timecodeTimestamp) / static_cast<long int>(1E6);

			if ( timecodeDiff > 50 )
				isTimecodeRunning.store(false);
		}

		std::this_thread::sleep_for( std::chrono::milliseconds(20) );
	}
}
