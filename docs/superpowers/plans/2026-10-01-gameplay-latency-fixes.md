# Gameplay latency fixes implementation plan

**Goal:** Fix the six findings in the accepted gameplay latency audit for iOS,
macOS, Windows, and Linux, with measurable hot-path improvements and truthful
latency reporting.

**Spec:** `docs/reviews/2026-10-01-gameplay-latency-audit.md`; the user requested
"fix all findings" after reviewing the investigation.

**Architecture:** Preserve a generated-sample cursor for audio scheduling and
publish output-presentation anchors for gameplay/input mapping. Native input
routes retain source time and use independent delivery where supported; fallback
routes remain usable without additional permissions. Callback scheduling and
gameplay presentation update only consumed/changed data. iOS keeps UIKit/Metal
ownership on the main thread and presents the current frame immediately.

## Constraints

- Work on `investigate/gameplay-audio-display-input-latency`; no new worktrees.
- Android-specific changes and distribution are excluded.
- No synthetic delay compensation: use native timestamps when available, add no
  guessed latency offsets or artificial input/audio/display waits, and expose
  unavailable output timing as unknown.
- Preserve replay, judgement, seek/pause/rate, retirement, and input ownership
  semantics; no whole-file formatting or parser/dependency replacement.
- Serialize every CMake/Ninja build in the existing directory. Independent
  source ownership is used for parallel work; root owns final integration.
- No physical input-to-photon/acoustic latency claim without hardware evidence.

## Work and checks

- [x] Audio timing: extend backend callback timing without breaking fake backend
  injection; consume PortAudio DAC timing and native iOS output timing; retain
  generation time for scheduled PCM and expose audible time for gameplay.
  Regression cases: nonzero output lead, delayed callbacks, startup/seek/pause,
  rate changes, missing/invalid timestamps, and route changes.
- [x] Presentation: enable iOS flip-after-render and bound desktop frame latency
  where supported. Retain run-loop servicing and test frame-cap/input servicing
  behavior without relocating UIKit ownership.
- [x] Input: native macOS/Linux keyboard ingress with safe focus/permission
  fallback, direct macOS MIDI routing, and source timestamps throughout legacy
  transitions. Test duplication, focus/device loss, callback teardown, and
  delivery during render stalls.
- [x] Buffer/reporting: request a smaller iOS callback period, expose real accepted
  callback sizes separately from estimated output latency, tune desktop requests,
  and report underruns/clock timing quality. Validate settings rollback and
  unknown values instead of inventing effective frames.
- [x] Audio schedule: remove O(remaining chart events) callback compaction;
  preallocate callback scratch. Test insertion, partial consumption, capacity,
  owner removal, reset, and oversized callbacks without allocation.
- [x] Snapshots: publish/apply sparse note changes safely across skipped
  generations and rotating buffers. Test slow consumers and full-state recovery;
  rerun chart-size scaling probes.
- [x] Observability and integration: bounded callback/input/frame timing counters,
  correct misleading scheduler telemetry, rerun focused and full desktop tests,
  desktop compile and unsigned iOS verification, review the whole change, update
  the audit with measured results and remaining hardware validation limits.

## Review focus

1. Output timestamp advances must not move chart scheduling or truncate startup
   lead-in, including after rate changes and device restarts.
2. Callback paths must not allocate, lock a main-thread mutex, or log synchronously.
3. Native plus fallback input must never double-judge or remain held after focus
   loss; stale callbacks must not reach a destroyed gameplay session.
4. Sparse consumers must recover after missing any number of published snapshots.
5. Unknown latency must be shown as unknown; callback period and acoustic/output
   latency must not be conflated.
