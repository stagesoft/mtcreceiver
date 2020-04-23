//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
// Stage Lab SysQ MTC receiver class header file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#ifndef MTCRECEIVER_CLASS_H
#define MTCRECEIVER_CLASS_H

//////////////////////////////////////////////////////////
// Preprocessor definitions
// #define TIXML_USE_STL
// #define __LINUX_ALSA__
// #define __UNIX_JACK__

#define FF_LEN 10
#define QF_LEN 8

#include <atomic>
#include <thread>
#include <math.h>
#include <chrono>
#include <vector>
#include <iostream>
#include <iomanip>
#include <rtmidi/RtMidi.h>

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
	unsigned char rate = MtcFrameRate::FR_30; //< 0x0: 24, 0x1: 25, 0x2: 29.97, 0x3: 30

	// Get the framerate value in fps
	double getFps( void ) const;
	// Convert to a string: hh:mm:ss:ff
	std::string toString( void ) const;
	// Convert to time in seconds
	double toSeconds( void ) const;
	// Convert from time in seconds
	void fromSeconds( double s );
	// Convert from milliseconds to frames due to a given rate
    int msToFrames( double ms );

};

class MtcReceiver : public RtMidiIn
{
    public:
        MtcReceiver(    RtMidi::Api api = LINUX_ALSA,
                        const std::string& clientName = "SysQ Mtc Receiver",
                        unsigned int queueSizeLimit = 100 );
        ~MtcReceiver( void );

        // RtMidiIn midi; // = { RtMidi::Api::LINUX_ALSA, "SysQ audioplayer", 100 };

        // Stream control vars
        static bool isTimecodeRunning;      // Is the timecode sync running?
        static double mtcHead;              // Time code head in milliseconds

    private:
        // MIDI TIMECODE DATA
        // ofxMidiTimecode timecode;        // Timecode message parser
        double timecodeTimestamp = 0;       // When last quarter frame message was received
        MtcFrame curFrame;                  // Timecode frame data, ie. H M S frame rate
        MtcFrame quarterFrame;              // Last quarter frame received
        int direction = 0;                  // Direction indicator
        unsigned int lastDataByte = 0x00;   // Last quarter received data byte, to recognize dir
        int qfCount = 0;                    // Quarters count
        bool firstQFlag = false;            // First quarter received flag
        bool lastQFlag = false;             // Last quarter received flag

        // Usefull functions
        bool decodeNewMidiMessage( std::vector<unsigned char> &message );
        bool isFullFrame( std::vector<unsigned char> &message );
        bool decodeFullFrame( std::vector<unsigned char> &message );
        bool decodeQuarterFrame( std::vector<unsigned char> &message) ;

        // RtMidi callback function
        static void midiCallback( double deltatime, std::vector< unsigned char > *message, void *data );

        bool checkerOn = true;
        void threadedChecker( void );

};

#endif // MTCRECEIVER_CLASS_H
