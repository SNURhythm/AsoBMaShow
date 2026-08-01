# Frame-Independent Practice Sessions

## Goal

Move session-backed practice gameplay onto the existing realtime gameplay
authority. Manual practice, practice autoplay, loops, and practice-result
retries must use audio-clock timing instead of render-frame timing wherever the
platform already has a native asynchronous input source. Practice autoplay
uses the platform-neutral realtime worker on every platform.

Inputs during a practice count-in may judge notes. The selected practice range
remains half-open, so only notes in `[startMicros, endMicros)` are eligible.

## Existing divergence

Records-modal autoplay launches a normal full-chart attempt and is eligible for
the realtime worker. Chart Viewer autoplay launches `autoPlay=true` with a
`practice::Session`. `resolvePlayStartInputDevices()` converts every such launch
to `practiceMode=true`, while `GamePlayScene::startRealtimeGameplayAuthority()`
rejects both `practiceMode` and `practiceSession`. The scene therefore falls
back to its frame-driven practice path.

The existing worker also receives an open-ended range beginning at the start
position, cannot finalize a bounded practice range, and the scene treats a
`PracticeComplete` terminal as an integrity failure. Autoplay practice loops do
not restart the worker from `reset()` because autoplay has no input handler.

The count-in has a separate blocker. The realtime worker's activation gate
drops all input before the start-lane indicator ends, while the legacy path
converts preparation presses and releases into visual-only replay events.

## Realtime authority eligibility

A non-replay attempt backed by `practice::Session` is eligible for the existing
realtime gameplay authority.

- Practice autoplay creates a worker without physical or touch input claims,
  matching normal realtime autoplay.
- Manual practice creates the same physical and touch routers as normal manual
  gameplay on platforms whose native asynchronous sources are implemented.
- Manual practice on platforms without a native realtime source retains its
  existing fallback path.
- Legacy `practiceMode` attempts without `practice::Session` remain on the
  legacy authority because they do not carry a bounded session lifecycle.
- Replay playback remains excluded.

This keeps one gameplay authority per attempt and avoids a second practice-only
timing thread.

## Practice timing and completion

The worker simulation receives the exact session range
`[configuration.startMicros, configuration.endMicros)`, not the open-ended
start-position range used by ordinary partial starts.

The worker receives an explicit practice completion boundary. When the audio
clock reaches `endMicros`, it first advances automatic deadlines through
`endMicros - 1`, then calls `GameplaySimulation::finalizePracticeRange()` at
that final eligible microsecond. The resulting misses, gauge changes, replay
events, note state, and `PracticeComplete` terminal are published through the
same bounded snapshot and transaction channels as ordinary realtime gameplay.

The scene recognizes `PracticeComplete`, stops the worker, transfers its final
snapshot, replay events, and gauge history, and completes the practice attempt
without invoking legacy range finalization a second time. On looping sessions,
the existing scene reset occurs when the next rendered frame observes the
terminal. That reset creates a fresh worker for both manual and autoplay
practice. Non-looping sessions continue to the existing result flow. The
next-loop transition is intentionally allowed to wait for a rendered frame;
gameplay inside each attempt is not.

Survival-gauge failure and bounded-capacity faults retain their existing
fail-closed behavior.

## Count-in input

Session-backed practice does not use the worker activation gate. The simulation
is initialized at `startMicros`, which excludes identities before the selected
range while leaving the first eligible note available to timestamped early
input. A press during count-in may therefore judge the first practice note only
if its compensated input time falls inside that note's early judge window.
Presses outside a valid window cannot judge it.

The legacy manual-practice fallback follows the same rule: preparation presses
and releases for a session-backed practice attempt go through normal lane
judgement instead of the visual-only preparation-event branch. Normal
non-practice preparation continues to use the activation gate and visual-only
events.

Input-triggered keysound reservation, gameplay mutation, audio commit, replay
recording, and judgement publication retain the realtime transaction ordering.
An accepted count-in hit therefore cannot produce a note judgement without its
matching realtime keysound when input-triggered keysounds are enabled.

## Scene synchronization

Rendered notes, beams, counters, gauge, and practice HUD remain snapshot
consumers. A low frame rate may delay their visual presentation but cannot
delay input acceptance, keysound submission, autoplay judgement, miss
expiration, gauge mutation, or the practice completion signal.

The scene remains responsible for UI-safe operations: pause controls, result
navigation, practice-session bookkeeping, and rebuilding a loop on the next
frame.

## Verification

Regression coverage will establish:

- a bounded manual worker accepts an early count-in press against the first
  in-range note;
- a press earlier than the valid judge window cannot hit that note;
- notes before the selected start and at the exclusive end remain ineligible;
- manual and autoplay workers finalize the range from audio time and publish
  `PracticeComplete` without a frame pump;
- finalization transfers misses and replay/gauge state exactly once;
- practice autoplay restart eligibility does not depend on an input handler;
- normal non-practice activation gating remains unchanged;
- existing simulation, realtime worker, practice boundary, replay, and audio
  tests continue to pass.

Desktop compilation and a non-deploying iOS build check will verify integration.
