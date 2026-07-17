# Frame-Independent Asynchronous Gameplay Input Design

**Date:** 2026-07-17
**Status:** Approved

## Summary

Gameplay input capture, judgement, note mutation, and input-triggered keysound
submission will run independently of the engine and render frame rate. A single
high-priority gameplay worker will own every timing-sensitive gameplay mutation.
Platform-native callbacks will publish fixed timestamped records to that worker,
which will resolve the binding, claim the note, commit gameplay state, and queue
the matching sound as one serialized transaction.

iOS is the first platform phase and includes touch, external keyboard,
controllers, MIDI, and gyroscope input. Windows follows after iOS acceptance;
Android, macOS, and Linux follow against the same platform-neutral contracts.

The software latency target is native callback entry through completed gameplay
commit and audio-command enqueue below 1 ms at p99. Audio hardware callback and
buffer latency are measured separately.

## Motivation and SDL event-watch finding

Timestamping an event can compensate judgement time, but it cannot remove a
late keysound if the application does not handle that event until its next
frame. At a low frame rate, that delay is perceptible.

`SDL_AddEventWatch` does not solve the capture problem. Its watcher is called
synchronously when an event is added to SDL's event queue, but the watcher does
not pump the operating-system event source. On the affected desktop and iOS
paths, SDL normally ingests system input during `SDL_PumpEvents`, reached from
`SDL_PollEvent`; a watcher therefore remains dependent on pumping. Some JNI or
producer-thread paths can push an SDL event independently, but a watcher then
runs on that producer context and still is not a safe owner of scene or gameplay
state.

The repository currently reinforces the frame dependency:

- The main loop polls SDL once per frame and performs frame pacing there.
- The iOS SDL main loop occupies UIKit's main thread, preventing normal UIKit
  delivery while it updates, renders, or sleeps.
- `GamePlayScene::pressLane()` performs note handling, sound dispatch, judgement,
  and replay work from the scene path.
- `RhythmLaneInputController::pressLane()` scans chart measures/timelines from
  the beginning for a press.
- `AudioWrapper::playSound()` uses general-purpose lifecycle, resource, lookup,
  and command synchronization that is unsuitable for the input hot path.
- CoreMIDI currently allocates packet storage and defers parsing/publication to
  a later `pump()`; iOS gyroscope gameplay data is also polled from SDL.

The new architecture bypasses frame-dispatched SDL only for authoritative
gameplay input. SDL remains responsible for general application input.

## Goals

- Make capture and handling latency independent of rendering and engine stalls.
- Keep note ownership, judgement, score state, replay ordering, and the matching
  keysound consistent during rapid or simultaneous input.
- Use authoritative native timestamps for judgement without intentionally
  waiting to reorder samples.
- Keep callback, worker transaction, and realtime audio-command paths bounded,
  preallocated, and free of general-purpose locks.
- Preserve SDL behavior for menus, text input, window/lifecycle events, and
  other non-gameplay interaction.
- Share the gameplay worker and simulation across all platforms while allowing
  each platform to use its appropriate asynchronous input source.
- Detect integrity loss instead of silently dropping critical edges or changing
  to a slower input path.

## Non-goals

- Eliminating audio device callback, buffer, or output hardware latency.
- Playing a speculative sound before note ownership is committed.
- Removing SDL from application UI, text, window, or lifecycle handling.
- Waiting for a cross-source timestamp reorder window.
- Silently using frame-polled SDL when a native gameplay backend fails.
- Defining every Windows, Android, macOS, or Linux API adapter in the iOS phase;
  those platform adapters are designed and accepted sequentially.

## Required invariants

1. The gameplay worker is the only writer of timing-sensitive gameplay state.
2. A sound-triggering input either commits both its gameplay transition and its
   audio command or commits neither.
3. Rendering, snapshot publication, telemetry export, and logging cannot delay
   the input transaction.
4. A physical transition is authoritative through exactly one gameplay source.
5. Native callbacks retain a closable sink gate, never a scene pointer.
6. Digital press/release edges are never silently lost.
7. Gameplay time derives from the audio clock, not the render frame.
8. Stale-session callbacks cannot affect a new attempt.

## Architecture

```text
Native UIKit / GameController / CoreMIDI / CoreMotion callbacks
                              |
                              v
                 RealtimeInputGate + session epoch
                              |
                              v
               fixed 4,096-record MPSC ingress ring
                              |
                              v
                  RealtimeGameplayWorker
             +----------------+----------------+
             |                |                |
             v                v                v
   GameplaySimulation   SPSC audio queue   immutable read model
   (sole state owner)          |          + critical result queue
                              v                |
                        audio callback         v
                                         engine/API thread
                                               |
                                               v
                                           rendering
```

The worker wakes for native input, lifecycle control, or its next automatic
gameplay deadline. It never waits for an engine frame.

## Components and ownership

### `GameplayDefinition`

Chart parsing produces immutable gameplay definition data with stable note IDs,
sorted per-lane note indices, timing data, and pre-resolved sound handles. Parser
note objects are not used as cross-thread mutable state.

### `GameplayRuntimeState`

Worker-owned fixed arrays hold played/dead/holding state, judgement state,
per-lane cursors, score, gauge, combo, and other attempt state. Per-lane lookup
advances through sorted indices rather than scanning the chart from its start.

### `CompiledGameplayBindings`

The active input profile, player/key mode, controller mappings, touch hit
geometry, axis thresholds, and device numeric IDs are compiled into fixed lookup
tables before the input gates open. Gameplay callbacks and transactions perform
no string lookup, map insertion, or profile access.

The ordinary `InputDeviceRegistry` remains responsible for settings monitoring,
device status, capture UI, and stable-ID management. At session activation it
supplies a snapshot; it is not in the realtime gameplay-delivery path.

### `GameplaySimulation`

This platform-neutral deterministic core has no scene, renderer, SDL, bgfx,
Jukebox, or general audio-resource dependency. It owns:

- physical-to-logical transition resolution;
- eligible note selection and claiming;
- press and release judgement;
- long-note state;
- miss and landmine deadlines;
- score, gauge, combo, and outcome;
- replay event ordering; and
- the decision that a committed transition requires a specific sound handle.

### `RealtimeGameplayWorker`

One serial user-interactive-QoS worker owns `GameplaySimulation`, the compiled
bindings, runtime state, automatic deadlines, replay accumulation, and
publication. Engine control enters only through its control queue.

### `GameplayControlCoordinator`

UIKit lifecycle requests and engine requests first enter one non-realtime serial
coordinator. It is the sole producer of the worker's SPSC lifecycle queue. Gate
closing itself is immediate and thread-safe; waiting for already-admitted
callbacks is represented as coordinator state, never as a wait on the UIKit
thread. Once the in-flight count reaches zero, the coordinator publishes the
corresponding lifecycle command and boundary.

### `RealtimeAudioCommandQueue`

Chart setup resolves keysounds into stable realtime handles. The worker writes
fixed commands such as `{handle, bus, startFrame}` to an SPSC queue. The audio
callback drains it at the start of its next buffer and mixes through prevalidated
resources without path lookup or lifecycle/resource mutexes.

The existing general audio API remains available outside this hot path.

### `GameplayReadModelPublisher`

The worker publishes immutable preallocated snapshots for scene/UI consumption.
Snapshots include note visual state, lane press/effect times, latest judgement,
score, gauge, combo, and session outcome. Replay storage remains worker-owned
until the attempt ends.

## Input transaction

For every accepted gameplay transition, the worker performs this sequence:

1. Validate the session epoch and device generation.
2. Convert the source timestamp to song time from a consistent audio-clock
   anchor.
3. Resolve the fixed binding and hysteresis state.
4. Select the eligible note using the lane index/cursor.
5. Compute the deterministic judgement and state delta.
6. If the delta requires sound, reserve an audio-command slot before mutation.
7. Commit note, lane, score, gauge, combo, long-note, and replay state.
8. Publish the already-reserved audio command with no fallible work in between.
9. Record fixed telemetry counters.
10. Attempt read-model publication after the timing-critical transaction ends.

Rejected, duplicate, or ineligible transitions do not enqueue sound. Because
audio capacity is reserved before state mutation, queue exhaustion cannot leave
a claimed note without sound or produce a sound for an unclaimed note.

The sound command is made available immediately. Its start frame is clamped to
the earliest frame the audio callback can still render; an old input timestamp
never causes an additional intentional sound delay. The original timestamp is
still used for judgement.

## iOS runtime thread model

`IOSRuntimeCoordinator` owns process bootstrap and teardown:

1. UIKit creates the SDL window and Metal-backed surface on the iOS main thread.
2. The main thread is established as bgfx's render thread.
3. A dedicated engine/API thread performs `bgfx::init`, owns
   `ApplicationContext` and scenes, submits bgfx API work, and calls
   `bgfx::frame()`.
4. A display link on the UIKit main thread calls nonblocking
   `bgfx::renderFrame(0)`.
5. `SDL_main` returns after setup so UIKit can deliver native events normally.

The UIKit thread never runs the engine loop or sleeps for engine frame pacing.
An engine slowdown can make visual snapshots stale, but cannot prevent UIKit
callbacks, worker judgement, or audio-command submission.

Shutdown closes native input gates, stops the gameplay worker, stops and joins
the engine/API thread, performs bgfx shutdown in its required ownership order,
and only then destroys main-thread SDL/UIKit resources. Callbacks cannot retain
engine, scene, or renderer objects.

## iOS native gameplay sources

### Touch

The existing SDL UIKit raw-touch integration gains a registered realtime sink.
UIKit publishes began, moved, ended, and cancelled samples with normalized
coordinates and native timestamps. All authoritative coalesced samples are
included in order. Predicted touches are excluded from judgement.

### External keyboard

`GCKeyboard` change handlers map native key codes to the existing SDL-scancode
binding namespace. The callback captures host time immediately when the API
does not provide an event timestamp.

### Controller

`GCController` connection and value handlers publish standard elements through
canonical SDL-compatible controller indices. Extended elements receive stable,
deterministic indices. Handler threading is treated as concurrent and therefore
uses the shared MPSC ingress ring.

### MIDI

CoreMIDI read callbacks copy packets into fixed-size continuation records with a
numeric source ID and CoreMIDI host timestamp. Running-status and message parsing
occurs on the gameplay worker. No callback-side `std::string`, `std::vector`,
mutex, or heap allocation is permitted.

### Gyroscope

Gameplay uses direct Core Motion delivery rather than frame-polling SDL sensor
state. Fixed motion samples enter the same worker, where the existing
platform-neutral turntable interpretation can be adapted to worker ownership.
Settings monitoring may continue through the ordinary registry path when no
attempt is active.

## Queues and wakeup

The initial iOS capacities are fixed before session activation:

| Path | Topology | Capacity |
| --- | --- | ---: |
| Native gameplay ingress | bounded MPSC, 64-byte records | 4,096 |
| Engine lifecycle control | SPSC | 64 |
| Worker-to-audio commands | SPSC | 1,024 |
| Worker-to-engine critical results | SPSC | 64 |
| Worker-to-engine read model | nonblocking triple buffer | 3 snapshots |

Every authoritative iOS sample is queued in the first version. Normal-operation
coalescing is disabled. A later platform may coalesce redundant continuous data
only after proving that all resolver threshold crossings remain explicit.

Ingress producers and the sole lifecycle-control producer share a precreated
dispatch semaphore and an atomic wake latch. The first idle-to-pending
transition signals the semaphore. The worker drains visible work, clears the
latch, rechecks queues to prevent a lost wakeup, and then waits until another
signal or the next automatic deadline. Producers never wait for the worker.

## Time and ordering

`mach_absolute_time` is the iOS host clock:

- CoreMIDI timestamps already use host ticks.
- UIKit and Core Motion timestamps use a calibrated affine conversion to host
  ticks.
- Keyboard and controller handlers without native timestamps capture host time
  on entry.

The audio callback publishes a generation-tagged clock anchor containing host
ticks, the corresponding audio frame, and playback rate. The worker reads the
anchor through a lock-free consistency protocol and maps input time directly to
song time. Pause, seek, restart, or audio-device generation changes require a
new anchor and session transition; an unusable clock is an integrity fault.

The MPSC reservation position is the cross-source arrival sequence. The worker
processes records in that sequence and never waits for another producer to
reorder them. Source time determines judgement; arrival sequence breaks
contention when multiple inputs target the same state.

For each available record, the simulation advances through automatic deadlines
up to that record's usable song time and then applies the input. After draining
published input it advances to current audio time and sleeps until the next
deadline. An unexpectedly late record may use its timestamp for an otherwise
eligible judgement, but it cannot rewind already committed gameplay state. Late
delivery is measured and must remain within the platform acceptance envelope.

## Render publication

Snapshot publication occurs only after gameplay commit and audio enqueue. The
publisher uses three preallocated buffers and dirty-page copying:

- The engine acquires an immutable latest generation and releases it after use.
- The worker writes only to a free buffer.
- If the engine temporarily holds all readable buffers, the worker skips that
  publication rather than blocking.
- Dirty pages remain pending so a later publication catches the view up.

One-shot visual effects carry sequence numbers and source/song times. A slow
renderer can show the latest meaningful effect without becoming part of
authoritative gameplay. Visual loss or staleness cannot affect judgement,
keysound, score, or replay.

Completion, failure, and integrity faults use the separate critical result
queue. An out-of-band atomic fault latch remains readable if that queue itself
is exhausted.

## Session and device lifecycle

### Start

Session activation creates a fresh epoch, resets queue state and held inputs,
installs definitions/bindings/handles, starts the worker, validates required
backends and the audio clock, then opens input gates.

### Pause, background, retry, and stop

Gate closure atomically prevents new callback acquisition and tracks callbacks
already in flight. After admitted callbacks publish and release their leases,
the serial control coordinator establishes the lifecycle boundary and publishes
the worker command. This completion is asynchronous; UIKit does not wait for
callback drain. Samples before the boundary are processed; later or stale-epoch
samples are rejected.

### Resume

Resume creates a new epoch and audio-clock anchor. A control already held when
the session resumes is suppressed until it returns to neutral. No held key,
button, touch, axis, or MIDI note becomes a synthetic press.

### Disconnect

A physical device disconnect creates worker-side releases for controls owned by
that device, preventing stuck lanes and long notes. A normal disconnect is not a
backend software failure and the attempt may continue with remaining inputs.

## SDL authority and fallback

SDL continues to deliver menus, settings, text entry, window events, and
lifecycle events. During gameplay, one explicit authority selection controls
each natively captured device class. When native authority is active, matching
SDL physical events cannot enter the gameplay resolver.

There is no silent gameplay fallback. An explicit developer diagnostic mode may
route SDL gameplay events through the worker, but the attempt is marked
latency-degraded and cannot save a competitive score or valid replay.

## Fault policy

| Condition | Required behavior |
| --- | --- |
| Required native backend unavailable at start | Refuse to start the attempt and explain the unavailable source. |
| Required native backend fails during play | Close gates and invalidate the attempt. |
| Digital ingress overflow | Close gates, reset held state, invalidate the attempt, and latch diagnostics. |
| Audio-command capacity unavailable | Do not mutate the input transaction; invalidate the attempt. |
| Invalid or mismatched audio clock | Stop judgement and input-triggered sound; invalidate the attempt. |
| Worker failure | Close gates, stop authoritative mutation, and invalidate the attempt. |
| Critical result queue exhaustion | Latch an out-of-band integrity fault. |
| Render snapshot buffers occupied | Skip publication; gameplay continues. |
| Stale epoch callback | Reject it and increment telemetry; do not fault the current attempt. |
| Physical device disconnect | Release its held controls and continue when otherwise viable. |

An integrity-faulted attempt cannot save a competitive score or valid replay.
Fault handling performs no speculative recovery that could create extra sound or
change prior note ownership.

## Migration

1. Extract `GameplaySimulation` and establish parity with existing chart and
   replay fixtures while SDL remains the only producer.
2. Add the worker, fixed queues, immutable snapshots, and realtime audio handles.
   SDL may feed the worker temporarily, but cannot mutate gameplay directly.
3. Split the iOS UIKit/render and engine/bgfx API threads.
4. Add native iOS touch, keyboard, controller, MIDI, and gyroscope sources.
5. In development builds, compare a side-effect-free shadow simulation with the
   authoritative result. The shadow cannot play sound, change score, or produce
   replay output.
6. Enable native iOS authority behind one explicit switch and suppress matching
   SDL gameplay events.
7. Remove obsolete scene-owned gameplay mutation after parity and device gates
   pass.

No migration phase permits two authoritative simulations or two keysound paths.

## Later platform phases

Windows is next. Android, macOS, and Linux follow. Each platform implements the
same adapter contract:

- deliver input independently of the render frame, using callbacks, window
  messages, or a dedicated high-rate platform sampling thread where the OS has
  no value-change callback;
- convert timestamps into a monotonic platform host clock;
- publish fixed records through a closable epoch gate;
- assign stable devices to precompiled numeric IDs;
- provide disconnect and backend-failure signals; and
- pass the same latency, integrity, lifecycle, and deduplication gates.

The exact native APIs, thread-affinity rules, queue topology adjustments, and
device coverage for each later platform require a focused platform design
review before implementation. Those reviews may adapt source-side queue details
but may not weaken the shared worker, transaction, sound/state, or fault
invariants.

## Verification and instrumentation

### Deterministic tests

- Existing behavior parity across representative charts and replay fixtures.
- Press/release windows, early/late boundaries, misses, long notes, landmines,
  score, gauge, combo, and outcome.
- Same-lane rapid retrigger and alternating-lane input.
- Simultaneous inputs from multiple source classes with deterministic arrival
  ordering.
- Exactly one audio command for each committed sound-triggering transition and
  none for rejected, duplicate, or faulted transitions.
- Equivalent results across different worker wake and scheduling patterns.

### Queue and lifecycle stress tests

- Synthetic input at ten times expected device rate without lost digital edges.
- Deliberate ingress, audio, and result exhaustion with the specified fail-closed
  outcome.
- Empty-to-nonempty wake races and worker clear/recheck races.
- Pause/resume, background/foreground, retry, shutdown, callback-in-flight, and
  stale-epoch sequences.
- Disconnect while controls or long notes are held.
- ThreadSanitizer runs proving no gameplay-state writer outside the worker.

### Low-frame-rate tests

- Cap engine/render updates at 15 FPS.
- Inject repeated 250 ms engine and rendering stalls.
- Continue native input and audio callbacks during those stalls.
- Confirm judgement/audio-command latency and deterministic state remain within
  the same gates as normal rendering.

### Realtime telemetry

Preallocated counters/rings record:

- callback entry and native source time;
- ingress enqueue and queue watermark;
- worker wake;
- transaction start and commit;
- audio-command enqueue and audio-callback pickup;
- snapshot generation and skips;
- late delivery, stale epoch, overflow, and integrity-fault codes.

Telemetry is exported or logged only outside the hot path. Debug instrumentation
also asserts zero allocation and zero prohibited-lock acquisition in native
callbacks, worker transactions, and realtime audio-command handling.

### Build verification

- Run the focused deterministic, queue, lifecycle, audio, and snapshot tests
  through CTest.
- Run the complete existing desktop test suite and
  `cmake --build cmake-build-debug --target main -j 6`.
- Run `scripts/ios_firebase_deploy.sh --build-only` for an iOS compile check;
  this does not authorize a Firebase upload.
- Complete latency acceptance with an instrumented build on physical iPhone and
  iPad hardware; Simulator results are useful for correctness but not latency
  acceptance.

## Acceptance criteria

- Callback entry through completed gameplay commit and audio-command enqueue is
  below 1 ms at p99 on supported physical iOS devices.
- The same target passes at 15 FPS and during repeated 250 ms engine/render
  stalls.
- Audio pickup occurs at the next available callback and is reported separately
  from the software input metric.
- All supported iOS gameplay classes—touch, external keyboard, controller,
  MIDI, and gyroscope—use native frame-independent capture.
- Digital edges are lossless under the ten-times-rate test; deliberate
  exhaustion fails closed.
- Rapid presses cannot produce a dead-note sound, duplicate sound, dangling
  claimed note, or sound/state disagreement.
- Hot-path allocation and prohibited-lock counters remain zero.
- Replay and result outcomes are deterministic across scheduling variations.
- Native authority never duplicates SDL gameplay delivery.
- Backend failure and explicit diagnostic SDL mode cannot save a competitive
  score or valid replay.
- Rendering may become stale under load, but never delays or changes gameplay or
  keysound submission.
