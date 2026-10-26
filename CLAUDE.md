<!-- SPDX-FileContributor: Ion Reguera <ion@stagelab.coop> -->

# mtcreceiver

Part of the **CUEMS** ecosystem — see the [`cuems-RELATIONS`](https://github.com/stagesoft/cuems-RELATIONS) repo for the system index, architecture diagram, and protocol/port map.

## Role

MTC (MIDI Time Code) **receiver** class, based on RtMidi. Added to a project as a **git submodule** and compiled with it — vendored inside `cuems-videocomposer`, `cuems-dmxplayer`, `cuems-audioplayer`, and `gradient-motion-engine`. Decodes MTC quarter-frames (QF) and full-frames from ALSA `Midi Through Port-0` into an `mtcHead` timebase; consumers gate playback on `isTimecodeActive()` (`timecodeRunWeight==1.0`, set only when a complete 8-QF sequence decodes).

**Always operate on the top-level checkout, never the nested submodule copies** inside the players (committing there would also dirty the parent's submodule pointer).

## Timebase model (Phase 2 onward)

Commit `021b689` (2026-04-22, "Phase 2") **removed** the MTC-spec `quarterFrame.frames += 2` compensation — the receiver now returns raw wire-MTC and consumers own pipeline-latency compensation. Consequence: the QF-assembled `mtcHead` structurally **lags wire-MTC by a constant ~3 frames (~80–120ms)** while a full-frame carries the instantaneous position.

## Day alignment of engine times (24 h wrap) — `toLocalDay`

The engine stamps every cue time in **its own** 24h-extended MTC domain (the wraps its `MtcListener` has seen since the last load); `mtcHead` counts the wraps **this process** has seen. The two day counts differ for a process born after the engine passed 24 h (per-cue audio players), or after a STOP at wire 23–24 h / 0–1 h after a real wrap, or a restart of either side past 24 h — and a raw engine time is then a day off: silence, black, a frozen fade or a scene that never fires, **with no log** (lleida-diposit 2026-10-24).

**Never move `mtcHead`; convert each engine time instead:** `MtcReceiver::toLocalDay(engineMs, localMs, k)` (nearest
whole day, symmetric 12 h rule: `k = floorDiv(engine − head + 12 h, 24 h)`, `local = engine − k·24 h`; ties +12 h → 1,
−12 h → 0). Lock-free and wait-free (safe from an RT audio callback and the tick callback). It returns **false** while the
head is not authoritative: before this process's first complete frame (the head is then a 0–70 ms quarter-frame ramp),
after `resetWrapOffset()` until the next complete frame, or when a reset races the call. Consumers must **fail closed**
then (keep the raw value pending, do not build on it) and retry. `headAuthoritative_` is raised together with the head
store (complete QF frame, full-frame SEEK) under `wrapStateMutex_`; a RESYNC full frame does not raise it;
`resetWrapOffset()` lowers it and bumps an epoch so a reader also catches reset + republish. `dayShift()` is the pure rule;
`isHeadAuthoritative()` the flag. Test hooks under `MTCRECV_TESTING`: `setHeadAuthoritativeForTesting`,
`setPublishHookForTesting`, `setReaderHookForTesting`. Since `e720276` (`fix/1amfwm7fbqy-mtc-day-align`). Plan:
cuems-RELATIONS `Plans/2026-10-25-mtc-day-align-24h-wrap.md`.

## Field notes / gotchas

- **2s synchronized skip (FIXED — `rc_1` `aa44894`, "hold QF timebase on periodic full-frame resync").** libmtcmaster's periodic full-frame resync (every 2s) was stored into `mtcHead` unconditionally → a +80–120ms snap every 2s (the ~3-frame QF lag), which VC's anti-drift hard-snapped and audioplayer `seekg`'d → visible/audible lockstep skip across all outputs + audio. The fix classifies FF as RESYNC vs SEEK and on RESYNC does **not** publish the FF into `mtcHead` (holds the continuous QF timebase); only genuine seeks reposition. Key details: tolerance floor 2→5 frames (the old 2-frame floor tied with the measured 3-frame lag → every resync mis-classified as SEEK); cold-attach guard `isResync = (timecodeRunWeight!=0) && |Δ|<tolMs` (a fresh process's first FF near TC0 is a SEEK, not a silent drop); permanent verdict log `MTC-RX: FullFrame delta=.. -> RESYNC/SEEK`. Packet-loss recovery is preserved (lost QFs → head stalls → next FF Δ>tol → SEEK). Steady-state timing is bit-identical. Consumer bumps: VC `main` `118e06d`, audioplayer `master`, dmxplayer `main`.
- **Duplicate/doubled QFs** (200/s from a doubled ALSA subscription) push `qfCount` past the expected count so `complete` never fires → `isTimecodeActive()` permanently false → DMX gate never opens (video still rides the per-QF tick). Root cause + fix are on the cuems-midi-connector / libmtcmaster side; see the dmxplayer CLAUDE.md.
- The standalone `/casas/dion/src/cuems/mtcreceiver` checkout was historically a stale 2020 vestige — the live release line is `rc_1` / `main` (`origin/master` deleted). Verify you're on the right branch before working here.
