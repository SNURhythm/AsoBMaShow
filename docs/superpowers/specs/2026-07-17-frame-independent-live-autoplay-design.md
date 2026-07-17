# Frame-Independent Live Autoplay Design

**Date:** 2026-07-17
**Status:** Approved

## Scope

Live, non-replay autoplay will advance independently of engine and render frame
rate on every platform. Replay-watch judgement retains its existing authority,
while replay-watch keysounds become frame-independent. Practice session
autoplay remains on its existing practice authority until that authority can use
the realtime worker without changing practice-range completion behavior.

## Root cause

`GameplaySimulation::advanceTo()` already resolves autoplay presses, long-note
releases, landmines, missed-note expiration, gauge changes, and completion from
song time. `RealtimeGameplayWorker` calls that function every millisecond using
the audio clock, independently of rendering.

`GamePlayScene` nevertheless excludes `options.autoPlay` from creating the
realtime authority, requires an input handler that autoplay intentionally does
not create, and passes `autoPlay = false` to the worker simulation. Autoplay
therefore remains in the legacy per-frame `checkPassedTimeline()` path.

## Design

For an ordinary live autoplay attempt, `GamePlayScene` will create the existing
`RealtimeGameplayWorker` even when no physical input handler exists and even on
platforms without a native realtime input backend. It will pass
`options.autoPlay` into `GameplayAttemptOptions::autoPlay`.

An autoplay-only session will not create a physical-input router, subscribe to
realtime device input, install an SDL event watch, or claim any device class.
The worker remains the sole gameplay-state writer and advances from the audio
clock on its existing one-millisecond loop. Manual live play retains the current
iOS and Windows native-input requirements.

Automatic transactions containing a `soundNoteId` will use the worker's
realtime audio sink when input-triggered keysounds are configured. The worker
will reserve and commit the audio command before publishing the matching
transaction. Capacity or commit failure invalidates the attempt rather than
silently producing a note without its keysound. Normal autoplay launches use
the existing pre-scheduled auto-keysound path, so the worker must not duplicate
those sounds.

The scene will continue consuming immutable worker snapshots. Slow frames may
delay visuals, but the retained transaction history will replay press/release
effects in order while score, judgement, replay state, and completion remain
authoritative in the worker.

## Replay-watch keysounds

Replay-watch judgement is already independent of keysound delivery and remains
unchanged. Its keysounds are currently triggered by
`GamePlayScene::processReplayKeySounds()` during scene updates, so a low frame
rate delays sound even though the recorded judgement time is preserved.

Before Jukebox playback starts, the scene will resolve each eligible recorded
Press event to its chart note and convert the event's gameplay timestamp to raw
song time using the active audio offset. Those resolved WAV/time pairs will be
added as `ReplayKeysound` entries to Jukebox's existing complete timestamped
audio schedule. The audio callback will therefore activate them at the recorded
time without waiting for an engine frame.

The scheduled path will preserve the current replay eligibility and note
resolution rules, classify replay sounds on the keysound bus, and rely on
Jukebox's existing pause, playback-rate, restart, and seek transitions. The
scene's per-frame replay-keysound cursor will be removed so it cannot duplicate
the scheduled sound.

## Error and lifecycle behavior

Pause and resume use the existing worker suspend handshake. Audio-clock,
automatic-result, replay, gauge-history, and realtime-audio failures keep the
existing fail-closed behavior. A successful chart completion transfers the
worker replay and gauge history through the existing stop path.

## Verification

- Add a worker regression proving autoplay claims and judges notes after only
  the fake audio clock advances, without any frame pump or input event.
- Add a worker regression proving an automatic input-triggered keysound is
  committed exactly once before its transaction is published.
- Add a replay schedule regression proving Press events resolve to keysound-bus
  entries at raw song time after applying the audio offset.
- Verify replay scheduling ignores non-Press events, unresolved notes, and WAV-
  less notes, matching the existing runtime behavior.
- Preserve the existing tests for miss/landmine deadlines, pause behavior,
  transaction history, and autoplay long-note behavior.
- Build the desktop `main` target and run the focused gameplay worker,
  simulation, and automatic-authority suites.
