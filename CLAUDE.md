# mtcreceiver

Part of the **CUEMS** ecosystem — see the [`cuems-RELATIONS`](https://github.com/stagesoft/cuems-RELATIONS) repo for the system index, architecture diagram, and protocol/port map.

## Role

MTC (MIDI Time Code) **receiver** class, based on RtMidi. Added to a project as a **git submodule** and compiled with it — vendored inside `cuems-videocomposer`, `cuems-dmxplayer`, `cuems-audioplayer`, and `gradient-motion-engine`. Decodes MTC quarter-frames (QF) and full-frames from ALSA `Midi Through Port-0` into an `mtcHead` timebase; consumers gate playback on `isTimecodeActive()` (`timecodeRunWeight==1.0`, set only when a complete 8-QF sequence decodes).

**Always operate on the top-level checkout, never the nested submodule copies** inside the players (committing there would also dirty the parent's submodule pointer).

## Timebase model (Phase 2 onward)

Commit `021b689` (2026-04-22, "Phase 2") **removed** the MTC-spec `quarterFrame.frames += 2` compensation — the receiver now returns raw wire-MTC and consumers own pipeline-latency compensation. Consequence: the QF-assembled `mtcHead` structurally **lags wire-MTC by a constant ~3 frames (~80–120ms)** while a full-frame carries the instantaneous position.

## Field notes / gotchas

- **2s synchronized skip (FIXED — `rc_1` `aa44894`, "hold QF timebase on periodic full-frame resync").** libmtcmaster's periodic full-frame resync (every 2s) was stored into `mtcHead` unconditionally → a +80–120ms snap every 2s (the ~3-frame QF lag), which VC's anti-drift hard-snapped and audioplayer `seekg`'d → visible/audible lockstep skip across all outputs + audio. The fix classifies FF as RESYNC vs SEEK and on RESYNC does **not** publish the FF into `mtcHead` (holds the continuous QF timebase); only genuine seeks reposition. Key details: tolerance floor 2→5 frames (the old 2-frame floor tied with the measured 3-frame lag → every resync mis-classified as SEEK); cold-attach guard `isResync = (timecodeRunWeight!=0) && |Δ|<tolMs` (a fresh process's first FF near TC0 is a SEEK, not a silent drop); permanent verdict log `MTC-RX: FullFrame delta=.. -> RESYNC/SEEK`. Packet-loss recovery is preserved (lost QFs → head stalls → next FF Δ>tol → SEEK). Steady-state timing is bit-identical. Consumer bumps: VC `main` `118e06d`, audioplayer `master`, dmxplayer `main`.
- **Duplicate/doubled QFs** (200/s from a doubled ALSA subscription) push `qfCount` past the expected count so `complete` never fires → `isTimecodeActive()` permanently false → DMX gate never opens (video still rides the per-QF tick). Root cause + fix are on the cuems-midi-connector / libmtcmaster side; see the dmxplayer CLAUDE.md.
- The standalone `/casas/dion/src/cuems/mtcreceiver` checkout was historically a stale 2020 vestige — the live release line is `rc_1` / `main` (`origin/master` deleted). Verify you're on the right branch before working here.
