# Asynchronous Gameplay Input — Working Notes

**Date:** 2026-07-17
**Status:** Design sections approved; formal specification drafted for review.

## Goal

Make gameplay input capture and handling independent of rendering frame rate so
that both judgement and input-triggered keysounds remain responsive during low
frame rates or rendering stalls.

Timestamp compensation remains necessary for accurate judgement, but it is not
sufficient for perceived latency because a keysound played only when the next
frame handles input can still feel late.

## Agreed platform sequence

Implement one shared architecture in platform phases:

1. iOS
2. Windows
3. Android, macOS, and Linux

The iOS phase covers every gameplay input class available there: touchscreen,
external keyboard, game controllers, MIDI, and gyroscope input.

## Agreed latency target

- Native callback to completed judgement and queued keysound: under 1 ms at
  p99.
- The target excludes the unavoidable wait for the next audio hardware callback
  and device buffer.
- There is no intentional batching or timestamp-reordering delay.
- The hot path must be bounded, allocation-free after setup, immediately woken,
  and instrumented so the p99 target can be measured.

## Rejected split path

Do not play a speculative keysound immediately and apply judgement/note state
later. That creates a gap between audio and note ownership: rapid presses can
select the same note twice or play a note that the delayed gameplay path has
already killed.

Keysound selection, note claiming, and judgement must be one serialized logical
transaction.

## Gameplay worker ownership

**Selected architecture:** dedicated deterministic gameplay worker. A shared
gameplay mutex and a speculative fast-audio/delayed-commit path were considered
and rejected because they cannot satisfy the agreed latency and correctness
constraints.

A dedicated serial gameplay worker is the sole owner of timing-sensitive
gameplay state. Native callbacks enqueue timestamped physical input to it. For
each accepted input, the worker atomically:

1. Resolves the physical input to logical gameplay actions.
2. Selects and claims the eligible note.
3. Commits judgement and lane/note state.
4. Enqueues the corresponding keysound without blocking.
5. Publishes results/snapshots for rendering and other UI consumers.

The same worker also owns timing-sensitive automatic mutations so they cannot
race input processing:

- miss expiration;
- long-note state and release;
- landmines and other time-driven note mutations;
- score, gauge, and combo;
- replay event ordering.

The engine/render side consumes immutable snapshots or ordered result messages.
It does not mutate worker-owned gameplay state.

The current chart-wide scan in `RhythmLaneInputController::pressLane()` is not
suitable for the latency target. The design must use indexed per-lane cursors or
an equivalent bounded lookup.

### Approved ownership and data flow

- Native iOS callbacks write timestamped physical samples to the preallocated
  shared ingress ring and wake the serial gameplay worker.
- The gameplay worker owns binding resolution, indexed note selection and
  claiming, automatic note deadlines, judgement, score/gauge/combo, replay
  ordering, and keysound selection.
- The worker submits pre-resolved sound handles to an SPSC audio queue and
  publishes immutable snapshots or ordered result messages to the engine/API
  thread.
- The engine/API thread owns `ApplicationContext`, scene/UI management, and
  bgfx API submission. It sends serialized start/pause/resume/retry/stop commands
  to the gameplay worker.
- The parser chart becomes immutable gameplay definition data with stable note
  IDs. Mutable played/dead/holding/judgement state lives in worker-owned runtime
  arrays; rendering never reads mutable parser note objects.
- The worker uses the atomic audio clock as its timeline and wakes for either
  native input or the next automatic gameplay deadline. It never waits for an
  engine frame.
- The input profile is snapshotted at gameplay start and the worker owns its
  resolver. Native gameplay transitions bypass frame-dispatched registry
  delivery, while device status and settings-monitor samples may still use the
  ordinary registry.

## iOS thread model

The current SDL main loop occupies UIKit's main thread and sleeps there for
frame pacing. That prevents UIKit touch/keyboard delivery from being independent
of rendering.

The accepted iOS model follows bgfx's established split:

- UIKit and the display link remain on the iOS main thread.
- The main thread handles native input callbacks and calls only
  `bgfx::renderFrame()` for rendering coordination.
- A dedicated engine/API thread owns `bgfx::init`, scene updates, resource/API
  calls, and `bgfx::frame()` submission.
- `SDL_main` returns control to UIKit after setup instead of running an endless
  loop on the UIKit thread.
- Native UIKit input callbacks enqueue directly to the gameplay worker and do
  not wait for an engine or render frame.
- Touch movement capture includes UIKit coalesced samples and their native
  timestamps where available; predicted touches are not authoritative input.

### Approved iOS integration and lifecycle

- An `IOSRuntimeCoordinator` owns bootstrap and teardown. UIKit creates the SDL
  window/Metal layer, establishes the main thread as bgfx's render thread,
  starts the engine/API thread, installs a display link that calls nonblocking
  `bgfx::renderFrame(0)`, and then returns from `SDL_main`.
- The engine/API thread owns bgfx initialization, `ApplicationContext`, scene
  updates, and `bgfx::frame()` submission. Teardown closes input gates and stops
  gameplay/engine workers before bgfx shutdown and main-thread SDL/UIKit cleanup.
- Touch extends the existing repository SDL UIKit raw-touch hook with a
  registered realtime sink. It publishes began/ended/cancelled samples and all
  authoritative coalesced movement samples; predicted samples are excluded.
- External keyboard input uses `GCKeyboard` change handlers mapped to the
  existing SDL-scancode binding namespace.
- `GCController` connection/value handlers map standard elements to canonical
  SDL controller indices and deterministic extended indices where required.
- CoreMIDI callbacks enqueue fixed timestamped packets for worker-side parsing.
- Existing native gyroscope samples route directly to the gameplay worker.
- Every sample carries a gameplay-session epoch. Sources retain only a closable
  sink gate, never a scene pointer. Activation opens the gate only after worker
  readiness; pause/background/retry/shutdown close it first and serialize reset
  or stop. Late callbacks are rejected by epoch.
- SDL input is suppressed only at the gameplay resolver boundary for natively
  captured classes; normal SDL UI, text, and lifecycle delivery continues.

## SDL scope

Platform-native asynchronous input replaces SDL input only for gameplay. SDL
continues to handle menus, text entry, lifecycle/window events, and other
general UI behavior.

During gameplay, native-captured device classes must not also enter through the
SDL gameplay path. The design must define an explicit source-selection or
deduplication policy so a physical transition is processed exactly once.

## Audio hot path

The worker must submit a pre-resolved sound handle/index through a nonblocking,
preallocated command queue to the audio callback. It must not perform file/path
lookup, resource loading, device lifecycle changes, or contend on the current
general-purpose audio lifecycle/resource locks.

### Approved latency-critical path

- Native callbacks capture/convert the source timestamp, encode a trivially
  copyable fixed-size sample, push it to a preallocated queue, and signal the
  worker only on an empty-to-nonempty transition.
- Callbacks perform no allocation, logging, profile/chart lookup, or
  general-purpose mutex acquisition.
- Use SPSC queues for single callback contexts and bounded MPSC queues only for
  APIs that may invoke concurrently.
- The worker runs at user-interactive QoS and processes every currently visible
  sample immediately, without a batching or cross-source reorder wait. Source
  timestamps determine judgement time; ingress order resolves samples visible
  together.
- The worker maps capture time to song time using the audio callback's published
  clock anchor and playback rate.
- Per-lane sorted indices and advancing cursors replace chart-wide scans.
- Chart loading creates pre-resolved realtime sound handles. The worker submits
  fixed `{handle, bus, startFrame}` commands to an SPSC audio queue, which the
  audio callback drains at the beginning of its next buffer.
- Automatic note deadlines share the worker's wait loop and are independent of
  engine frames.
- Hot-path telemetry records enqueue, wake, transaction, and audio-command
  latency plus queue high-water marks and faults without logging inline.

## Queue overflow policy

The input queue is bounded. Digital press/release edges must never be silently
dropped. If an edge cannot be enqueued, gameplay enters an integrity-fault
state: the attempt is paused or aborted, held inputs are reset, and a diagnostic
is published. Continuous motion and analog samples may be coalesced to the
latest authoritative value, but coalescing must preserve all threshold-crossing
transitions produced by the input resolver.

## Approved shared gameplay core and render publication

- A deterministic `GameplaySimulation` core has no renderer, scene, or jukebox
  dependency and is owned exclusively by the serial gameplay worker.
- Immutable gameplay definitions use stable note IDs. Mutable note state lives
  in worker-owned arrays with indexed per-lane cursors; a press never scans the
  chart from its beginning.
- Bindings compile into fixed lookup tables when a session starts. The input
  hot path performs no allocation or associative-container lookup.
- Each accepted input edge is one worker transaction: resolve the lane, select
  and claim the note, commit judgement/score/gauge/combo/long-note/replay state,
  and enqueue the matching pre-resolved sound handle. A sound request cannot
  exist without its corresponding committed note transition, preventing dead
  sounds and dangling notes during rapid presses.
- The timing-critical transaction ends only after both gameplay state commit
  and audio-command enqueue. Visual snapshot publication occurs afterward and
  can never delay judgement or sound.
- The engine reads immutable, preallocated read-model snapshots containing note
  visual states, lane effects, HUD state, latest judgement, and session outcome.
- Snapshot publication uses nonblocking triple buffering and dirty-page copying.
  If rendering holds all readable buffers, the worker skips that publication;
  gameplay continues, accumulated dirty pages publish later, and rendering may
  temporarily display an older state.
- One-shot visual effects carry sequence numbers and timestamps so a slow frame
  can reconstruct the latest meaningful effect without becoming authoritative
  gameplay state.
- Completion, failure, and integrity faults use a separate small lossless result
  queue. Queue exhaustion is itself an integrity fault.
- Start, pause, resume, retry, and stop enter through the worker's serialized
  control queue. The engine and renderer never mutate live gameplay runtime
  state directly.
- Replay data remains worker-owned during the attempt and transfers to the
  engine only after the attempt ends.

## Approved lifecycle, faults, and fallback policy

- Gameplay activation creates a fresh session epoch, resets queues and held
  states, installs immutable bindings, starts the worker, and opens native input
  gates only after the worker is ready.
- Pause, background, retry, and shutdown close the gates first. Events accepted
  before the boundary are processed, stale events are rejected, and the worker
  then performs the serialized transition.
- Resume creates a new epoch and re-anchors gameplay time to the audio clock.
  Controls already held on resume are suppressed until they return to neutral,
  preventing synthetic presses.
- A physical device disconnect generates worker-side releases for controls
  owned by that device, preventing stuck lanes and long notes. Backend software
  failure is distinct from a normal disconnect and is an integrity fault.
- Before a note-triggering transaction mutates gameplay state, the worker
  reserves any required audio-command slot. State commit and publication of the
  reserved command then contain no fallible operation, so neither can exist
  without the other.
- Digital input overflow, audio-command exhaustion, invalid clock state, worker
  failure, or loss of a required native backend invalidates the attempt. Gates
  close, held states reset, judgement and input-triggered sound stop, and an
  out-of-band latched diagnostic remains observable even if the result queue is
  full.
- A native gameplay backend never silently falls back to frame-polled SDL input.
  If required bindings depend on an unavailable backend, gameplay cannot start
  or the active attempt is invalidated.
- SDL gameplay fallback may exist only as an explicitly selected diagnostic or
  degraded mode. Such attempts are marked latency-degraded and cannot produce a
  valid competitive score or replay.
- SDL menu, text, window, and lifecycle input remains available regardless of
  gameplay-backend status.

## Approved iOS queues, timestamps, and ordering

- iOS uses one preallocated bounded MPSC gameplay-ingress ring. Concurrent
  producers receive a single linearized queue position without a merge or
  reorder stage.
- The initial ingress capacity is 4,096 fixed 64-byte records. Each record
  contains the epoch, numeric device ID, input type, value/phase, source and
  ingress times, and a fixed payload.
- CoreMIDI callback data is copied into fixed continuation records and parsed by
  the worker. The callback performs no string/vector construction, mutex
  acquisition, or heap allocation.
- The first iOS version queues every authoritative sample and does not coalesce
  during normal operation. Coalescing may be introduced only after proof that
  no binding-threshold transition can be lost.
- Lifecycle controls use a fixed 64-entry SPSC queue, audio commands a fixed
  1,024-entry SPSC queue, and critical engine results a fixed 64-entry SPSC
  queue. All storage exists before gameplay begins.
- Producers share an atomic wake latch. The first idle-to-pending transition
  signals a precreated dispatch semaphore. The worker drains work, clears the
  latch with a lost-wakeup-safe recheck, and waits until another signal or its
  next automatic gameplay deadline.
- Gate closure uses an in-flight callback counter. It prevents new acquisition,
  allows already-admitted callbacks to publish, and asynchronously establishes
  the lifecycle boundary only after those callbacks exit. Accepted samples are
  processed before the boundary; later samples fail the epoch/gate check.
- `mach_absolute_time` ticks are the iOS common clock. CoreMIDI host timestamps
  use it directly; UIKit and CoreMotion timestamps use calibrated conversion;
  APIs without event timestamps capture host time immediately on callback entry.
- The audio callback publishes a generation-tagged lock-free clock anchor with
  host time, audio frame, and playback rate. The worker converts input time
  directly to song time from a consistent anchor read.
- MPSC reservation order resolves contention for the same note. Source time
  determines judgement, but the worker never waits to reorder arrived input.
- Published input is handled before automatic deadlines advance to current
  audio time. Gameplay never rewinds after authoritative state has advanced;
  unexpectedly late delivery is measured and bounded by device acceptance tests.

## Approved migration and acceptance gates

Migration order:

1. Extract `GameplaySimulation` and establish parity with existing chart and
   replay fixtures while SDL remains the only producer.
2. Add the worker, fixed queues, immutable snapshots, and realtime audio handles.
   SDL may temporarily feed the worker but may not mutate gameplay directly.
3. Split the iOS UIKit/render thread from the engine/bgfx API thread.
4. Add native iOS touch, keyboard, controller, MIDI, and gyroscope sources.
5. In development builds, compare a side-effect-free shadow simulation against
   authoritative results. It never plays sound, updates score, or creates replay
   output.
6. Select native iOS authority with one explicit switch and suppress equivalent
   SDL gameplay events when enabled.
7. After iOS acceptance, implement Windows, then Android, macOS, and Linux
   adapters against the same worker boundary. Every phase must pass the same
   integrity and latency gates.

Acceptance requirements:

- Native callback entry through completed gameplay commit and audio-command
  enqueue is below 1 ms at p99 on supported devices.
- The target continues to pass with rendering capped at 15 FPS and repeated
  artificial 250 ms engine/render stalls.
- Audio callback pickup is measured separately and occurs at the next available
  buffer; device/buffer latency is not included in the software-input metric.
- Rapid press/release, alternating lane, same-lane retrigger, long-note,
  landmine, simultaneous source, disconnect, lifecycle, retry, and stale-epoch
  tests prove exactly one sound per committed sound-triggering transition and no
  sound for rejected transitions.
- Synthetic load at ten times the expected device rate loses no digital edge.
  Deliberate exhaustion produces the approved integrity fault.
- Allocation and prohibited-lock counters remain zero on callback, worker
  transaction, and realtime audio-command paths.
- Replay outcomes are deterministic across worker scheduling variations, and
  ThreadSanitizer/stress tests find no gameplay state access outside the worker.
- On-device telemetry captures callback, enqueue, wake, transaction, audio
  enqueue/pickup, queue watermarks, late delivery, and faults without hot-path
  logging.
- Backend failure and explicit SDL diagnostic mode cannot save a competitive
  score or valid replay.

## Design status

All incrementally presented design sections have been approved. The next step
is to synthesize and self-review the formal design specification; implementation
planning begins only after the user reviews and approves that specification.
