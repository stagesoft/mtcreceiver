# TODO: Use wasLastUpdateFullFrame in cuems-audioplayer and cuems-dmxplayer

## New Feature Added

A new static member `MtcReceiver::wasLastUpdateFullFrame` has been added to mtcreceiver to provide explicit detection of full SYSEX MTC frames (like xjadeo's `tick=0` marker).

## What It Does

- `wasLastUpdateFullFrame = true` when a full SYSEX frame is received (used for seeking)
- `wasLastUpdateFullFrame = false` when quarter-frames complete (normal playback)

## Where to Use It

### cuems-audioplayer

**Location**: Wherever audio playback position is updated based on MTC

**Usage**:
```cpp
// Check if we should seek (full frame) or play continuously (quarter-frame)
if (MtcReceiver::wasLastUpdateFullFrame) {
    // Full SYSEX frame received - seek to new position
    // This matches xjadeo behavior: "seek frame - if transport is not rolling"
    seekToPosition(MtcReceiver::mtcHead);
} else {
    // Quarter-frame update - normal playback sync
    updatePlaybackPosition(MtcReceiver::mtcHead);
}
```

### cuems-dmxplayer

**Location**: Wherever DMX timeline position is updated based on MTC

**Usage**:
```cpp
// Check if we should seek (full frame) or play continuously (quarter-frame)
if (MtcReceiver::wasLastUpdateFullFrame) {
    // Full SYSEX frame received - seek to new position
    seekToTimecode(MtcReceiver::mtcHead);
} else {
    // Quarter-frame update - normal playback sync
    updateTimecodePosition(MtcReceiver::mtcHead);
}
```

## Benefits

- **100% accurate detection** (no heuristics needed)
- **Matches xjadeo's proven approach** (tick=0 marker)
- **Enables accurate seeking** when full SYSEX frames are received
- **Backward compatible** - existing code continues to work

## Implementation Status

- ✅ **mtcreceiver**: Implemented
- ✅ **cuems-videocomposer**: Implemented and using it
- ⏳ **cuems-audioplayer**: TODO - Add seeking logic using wasLastUpdateFullFrame
- ⏳ **cuems-dmxplayer**: TODO - Add seeking logic using wasLastUpdateFullFrame

