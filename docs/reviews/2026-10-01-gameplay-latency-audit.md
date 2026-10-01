# Gameplay audio, display, and input latency audit — 2026-10-01

Scope: iOS, macOS, Windows, and Linux. Android is excluded. Findings 1–6
describe the original baseline investigation. The implementation follow-up below
records the fixes, verification, and remaining physical measurement limits.

Baseline: `5750d646bda5d1c1d9814793712caed530809371`.
Branch: `investigate/gameplay-audio-display-input-latency`.

The highest-priority findings are the audio clock's callback-time anchor, the
deferred Metal presentation in iOS single-threaded mode, and frame-dependent
manual input delivery on macOS/Linux. These affect timing independently of
average FPS. Small mixer or worker optimizations alone will not remove them.

## Evidence and limits

- Traced production input registration, native/SDL event delivery, gameplay
  worker, audio command queues, mixer, backend, frame pacing, and the vendored
  SDL/bgfx/miniaudio implementations.
- Ran a silent PortAudio output probe on this Apple M1 Pro's MacBook Pro
  Speakers, at 48 kHz, using the same stream parameters as `AudioBackend.cpp`.
  This measures backend reports and callback timestamps, not acoustic latency.
- Compiled the production mixer and gameplay worker dependencies with `-O2`
  for synthetic probes. Three retained runs were sequential after compilation
  completed. Tables below use the median of the three per-run statistics;
  p99 columns are medians of per-run p99 values, not pooled p99 estimates.
  Ordinary desktop scheduling remained active. No real-time scheduling was
  requested by these probes.
- Executed 11 existing CTest binaries covering audio, gameplay worker,
  realtime input routing, timestamps, Windows mapping, and frame pacing:
  all passed in 1.35 seconds. These are functional checks, not latency tests;
  the existing test binaries were not rebuilt in this investigation.
- No live gameplay GPU capture, electrical/acoustic loopback, photodiode test,
  or physical iOS/Windows/Linux measurement was performed. CPU microbenchmarks
  do not establish end-to-end delay or real-device underrun rates.
- [Probe sources, build instructions, and retained output](evidence/2026-10-01-gameplay-latency/README.md).

## 1. High: the gameplay clock describes audio being generated, not audio reaching the output

[AudioWrapper.cpp:381](../../src/audio/AudioWrapper.cpp) anchors the first sample
of each mixed buffer to `nowMicros()` at callback execution. Both
`getTimeMicros()` and `songTimeMicrosAtSteadyMicros()` interpolate that anchor.
The latter drives input judgement through
`RealtimeGameplaySession::mapSteadyToSong` in `GamePlayScene.cpp:1173`.

[AudioBackend.cpp:473](../../src/audio/AudioBackend.cpp) discards both
`PaStreamCallbackTimeInfo` and callback status flags. Its generic render callback
cannot carry an output timestamp. The miniaudio path likewise exposes no
presentation timestamp to the mixer; the vendored CoreAudio output callback
discards `pTimeStamp` (`include/miniaudio.h:33390–33465`).

Consequently, audible chart time can trail gameplay time by the output pipeline's
delay. Callback jitter or buffer adaptation also moves the wall-clock anchor.
The clamp at the current buffer end prevents unlimited extrapolation during a
late callback but can temporarily freeze the published clock. Manual audio and
visual offsets can compensate a stable error, but do not remove pipeline delay
or variable error across devices/routes.

PortAudio explicitly defines `outputBufferDacTime` as the first output sample's
DAC time, separate from callback `currentTime`:
[PortAudio timing documentation](https://github.com/PortAudio/portaudio/wiki/BufferingLatencyAndTimingImplementationGuidelines).

The silent Mac probe demonstrates the distinction:

| Requested frames | Observed callback frames | Callback period | Reported stream output latency | Median DAC timestamp minus callback timestamp |
| ---: | ---: | ---: | ---: | ---: |
| Automatic | 64 | 1.33 ms | 18.708 ms | 4.305 ms |
| 64 | 64 | 1.33 ms | 18.708 ms | 4.309 ms |
| 128 | 128 | 2.67 ms | 20.042 ms | 5.633 ms |
| 256 | 256 | 5.33 ms | 22.708 ms | 8.287 ms |
| 512 | 512 | 10.67 ms | 28.042 ms | 13.567 ms |

No callback status flags occurred during these approximately one-second silent
streams. The difference between the backend's latency report and timestamp lead
requires route-specific validation; neither should be substituted uncritically
for measured acoustic latency. The application currently uses neither for its
clock anchor.

**Recommended change:** carry backend output timing and status into the audio
clock. Keep the generated-sample cursor for scheduling future PCM, while exposing
a separately defined audible/presentation clock for gameplay synchronization.
Calibrate the native-to-steady clock mapping, and verify startup, seek, pause,
playback-rate changes, underruns, and route changes. Simply subtracting the
settings panel's latency value is not a sufficient fix.

## 2. High: iOS delays presenting a newly encoded Metal frame until the next frame call

[main.cpp:181](../../src/main.cpp) enables iOS VSync but does not enable
`BGFX_RESET_FLIP_AFTER_RENDER`. At line 714, it explicitly selects bgfx
single-threaded rendering to keep UIKit layer access on the main thread.

In the pinned bgfx implementation:

1. `Context::swap()` calls `renderFrame()` synchronously in single-threaded mode
   (`bgfx/bgfx/src/bgfx.cpp:2512`).
2. `renderFrame()` calls `flip()` **before** rendering when the flag is absent
   (`bgfx.cpp:2589`).
3. Metal's `flip()` schedules `presentDrawable` and commits the pending command
   buffer (`renderer_mtl.mm:1475`).

Thus a normal frame encoded by call N is presented/committed at call N+1.
The main loop can perform its frame-cap wait, event processing, scene update,
and next-frame preparation between those calls. At a steady application cadence
of 60/120 FPS, the inter-call interval is approximately 16.7/8.3 ms. This is a
source-derived interval, not a measured input-to-photon result.

On multithreaded desktop bgfx, the render thread can loop back to `flip()` without
waiting for the next application frame call. The same flag therefore does not
justify claiming a fixed extra application-frame delay on desktop.

**Recommended change:** test flip-after-render specifically on iOS, preserving
the main-thread requirement, and compare presentation timestamps and visible
latency. Separately evaluate desktop frames in flight: the app leaves
`maxFrameLatency` at zero/default; pinned macOS Metal selects up to three
drawables (`renderer_mtl.mm:3618`, `config.h:428`). Buffer capacity alone does
not prove that three frames are always queued.

## 3. High: macOS/Linux manual input remains dependent on the main loop and loses source timing

[GamePlayScene.cpp:2121](../../src/scene/play/GamePlayScene.cpp) enables realtime
registry classes for iOS and Windows, with no equivalent macOS/Linux branch.
Its SDL event watch is installed only on iOS. Other manual paths use the
legacy logical adapter and enqueue to the gameplay worker after main-thread
dispatch.

[LogicalGameplayInputAdapter.cpp:186](../../src/input/LogicalGameplayInputAdapter.cpp)
calls `control_.pressLane(lane)` without the event timestamp; releases explicitly
use zero input delay. `prepareLegacyInput()` records `nowMicros()` at dispatch
(`GamePlayScene.cpp:1271`). This affects macOS MIDI too: CoreMIDI captures native
timestamps, but the macOS gameplay registration does not enable its direct
realtime path.

Keyboard events have an earlier loss of timing fidelity: Cocoa pumps native
events from the main loop, and SDL's `SDL_PushEvent()` replaces their timestamps
with `SDL_GetTicks()` (`SDL/src/events/SDL_events.c:1158`). A separate gameplay
worker cannot reconstruct hardware event time after that information is lost.

The main loop pumps events at `main.cpp:1222`, performs rendering, and sleeps for
the frame cap at line 1517 on desktop. An event just after a pump can wait until
the next pump. With evenly spaced input arrivals and one pump per frame, this
stage alone would average roughly half a frame: 8.3 ms at 60 FPS or 4.2 ms at
120 FPS, before audio callback/output or display delay. These are timing-model
examples; uncapped frame cadence, device behavior, and stalls change the result.

| Platform/path | Current behavior |
| --- | --- |
| macOS keyboard | Main-loop SDL delivery, followed by legacy adapter timestamping. |
| macOS MIDI | Native timestamps exist upstream, but gameplay uses main-thread legacy delivery. |
| Linux keyboard/controller | SDL/legacy delivery; no dedicated native gameplay input backend here. |
| Windows keyboard | Dedicated low-level hook thread; stamps callback receipt using QPC. Hook scheduling jitter still matters. |
| Windows XInput | Dedicated active polling at 1,000 Hz; fallback devices require separate testing. |
| iOS touch | `UITouch.timestamp` is preserved and sent to the worker from the UIKit callback. Main-thread rendering can still delay callback delivery and sound response. |
| iOS MIDI/gyro | Direct realtime registration is enabled. |

iOS's `WaitIOSMainRunLoopForMicros()` already services default/tracking run-loop
modes in approximately 1 ms slices. It should not be replaced with an ordinary
sleep. Accurate touch timestamps help judgement, but cannot make a keysound
audible before the callback has actually been delivered.

**Recommended change:** enable direct macOS MIDI routing and preserve timestamps
through every legacy transition. Add native timestamped keyboard ingress for
macOS/Linux where supported. Measure source-event-to-ingress separately from
ingress-to-worker, including deliberate render stalls and non-XInput devices.

## 4. High on iOS: roughly 10 ms buffer preference adds keysound waiting; latency UI mixes different quantities

The iOS miniaudio configuration leaves period size at its default
(`AudioBackend.cpp:111`). The pinned default low-latency profile requests
10 ms (`include/miniaudio.h:12100`); its iOS implementation calls
`setPreferredIOBufferDuration` and reads back the accepted duration at line
34371. The actual route may accept a different duration. iOS settings currently
reject non-default buffer requests (`AudioBackend.cpp:256`).

Realtime `PlayNow` commands are drained once at callback entry
(`AudioWrapper.cpp:415`). A command arriving after that drain waits for another
callback. A 10 ms callback cadence would add 0–10 ms of phase-dependent waiting,
about 5 ms on average under uniform arrivals, before output pipeline delay.
Scheduled BGM already uses within-buffer sample offsets and should retain that
behavior.

On desktop, `Pa_OpenStream` always receives `defaultLowOutputLatency` as its
suggestion, independent of selected callback size (`AudioBackend.cpp:365`).
Callback size is not an output-latency guarantee; the Mac probe above confirms
that explicitly choosing 64 frames did not improve the automatic setting.

The settings model then recomputes “latency” as frames/sample rate and ignores
the backend's `effectiveLatencyMs`
([SettingsAudioVideoModel.cpp:200](../../src/scene/SettingsAudioVideoModel.cpp)).
For automatic PortAudio sizing, the backend instead manufactures
`effectiveBufferFrames` from reported output latency (`AudioBackend.cpp:395`).
On the probe's default route that gives approximately **898 displayed frames**,
despite **64 actual callback frames**. Selecting 64 makes the UI show approximately
**1.33 ms**, although the backend timestamp lead was approximately **4.3 ms**.
These values describe different stages.

**Recommended change:** report actual callback sizes, callback period, and backend
output-latency estimates separately. Offer/test lower iOS period preferences with
accepted-duration readback and underrun monitoring, preserving existing audio
session behavior. Tune desktop requested output latency separately from callback
size. Apple documents preferences as requests and requires checking the accepted
duration: [Apple buffer-duration documentation](https://developer.apple.com/documentation/avfaudio/avaudiosession/setpreferrediobufferduration(_:)).

## 5. Medium: the audio callback shifts the entire remaining chart schedule

[Jukebox.cpp:3468](../../src/audio/Jukebox.cpp) stages all remaining scheduled
audio before playback. Each callback that activates a due event then moves all
remaining entries to the front of the array
([AudioMix.cpp:883](../../src/audio/AudioMix.cpp)). This is O(remaining events)
work inside a deadline-sensitive callback, even when only one event is due.

Optimized production-code probe, activating one event:

| Remaining schedule size before activation | Median time | Per-run p99 median |
| ---: | ---: | ---: |
| 1,000 | 2.5 µs | 2.6 µs |
| 10,000 | 33.9 µs | 36.2 µs |
| 65,536 | 224.9 µs | 266.3 µs |
| 200,000 | 686.1 µs | 786.4 µs |

The 200,000-event case is a stress case, not a claimed typical chart. At 48 kHz,
a 64-frame callback has a 1,333 µs period: schedule compaction alone can consume
a meaningful share before mixing and effects. Mixing 64 stereo voices in that
same 64-frame block took a median 33.1 µs; 512 voices took 264.9 µs. These probes
exclude effects, conversion, command drain, backend overhead, and render load;
they do not demonstrate actual callback deadline misses.

There is also a first-use/growth allocation in the real callback's
`mixBuffer->resize` (`AudioWrapper.cpp:450`). It is not a steady-state allocation
on every callback, but should be removed before pursuing very small periods.

**Recommended change:** use a schedule read cursor or equivalent structure so
activation costs depend on due entries, and preallocate callback scratch storage.
Preserve sorting, seek/reset, capacity, and retirement semantics. Optimize the
normal-rate mixer only after callback profiling establishes a need.

## 6. Medium: snapshot publication and consumption copy all note state

[RealtimeGameplayWorker.cpp:699](../../src/scene/play/RealtimeGameplayWorker.cpp)
copies every chart note on publication, plus graph state and transaction history.
The second ingress-drain loop can publish after every input. The render side
iterates every note again and performs a `dynamic_cast` per non-null note when
applying a new snapshot (`GamePlayScene.cpp:2589`).

Synthetic optimized worker probe, with pre-activation press/release events and
a fake audio sink:

| Chart notes | Enqueue → published snapshot median | Per-run p99 median | Enqueue → fake sound commit median |
| ---: | ---: | ---: | ---: |
| 1,000 | 13 µs | 21 µs | 2 µs |
| 10,000 | 73 µs | 138 µs | 3 µs |
| 50,000 | 415 µs | 561 µs | 4 µs |
| 100,000 | 683 µs | 1,366 µs | 8 µs |

This supports the scaling concern while showing that a basic sound-command
commit is much cheaper than full-state publication in this workload. It does
not include live judgement workload, audio mixing, render-side snapshot
application, OS input delivery, or screen presentation. The reader actively
checks snapshots; this is not representative of a normally sleeping UI thread.

**Recommended change:** publish changed notes or dirty ranges and bound
presentation-copy work. Keep authoritative input/sound processing independent
from full chart presentation copies. The worker's signaled semaphore already
wakes on input; its 1 ms timeout is not an unconditional 1 ms input delay.

## Measurement gaps and implementation order

The existing “Audio … µs (… Hz)” log at `main.cpp:1455` uses
`Jukebox::getAvgDeltaTime()`, whose samples come from the **visual scheduler**
loop. It is not callback execution time or audible latency. The local CMake
configuration also has performance telemetry disabled. Skin CPU telemetry is
useful but does not measure the entire input-to-sound/display chain.

1. Add bounded, allocation-free trace records across native event time,
   ingress receipt, worker judgement, sound command commit, callback drain,
   expected DAC time, snapshot selection, frame submission, and presentation.
   Expose callback size/jitter/underruns and stage p50/p95/p99/max.
2. Address iOS deferred presentation and macOS/Linux input delivery. Preserve
   UIKit thread ownership and native timestamps.
3. Introduce the output-timestamp-based audio clock with explicit scheduling
   versus audible-time semantics, then validate offsets and route changes.
4. Remove schedule compaction/callback allocation, then evaluate smaller iOS
   periods and desktop latency requests against glitch rates.
5. Reduce snapshot copying if chart-size and on-device measurements justify it.

Validate iOS at both available 60/120 Hz modes, macOS Metal with VSync/caps on
and off, Windows with the selected audio host API and keyboard/XInput/fallback
controllers, and Linux with its actual audio/display stack. Use representative
dense charts, BGA and selected skins, with real release builds. Audio loopback
and high-speed-camera/photodiode measurements are needed before claiming an
end-to-end latency number or improvement.

## Implementation follow-up — 2026-10-01

The fixes preserve the generated sample cursor for scheduled audio and use
native output timestamps for presentation/input mapping. No reported latency,
callback-period guess, configurable compensation offset, debounce, or artificial
input/audio/display alignment wait is added.

- **Output timing:** PortAudio DAC timestamps use a stream-clock/steady-clock
  epoch calibration taken outside callbacks. iOS carries AudioUnit host time
  through miniaudio's native render boundary and exact conversion-chunk offsets.
  Unknown native timing retains receipt-time fallback and is reported as unknown.
  A fixed history of actual output intervals preserves older input mapping and
  holds the preceding interval's end during a real output gap. Seek, rate, and
  stream changes invalidate previous intervals. Overwritten history is unknown.
- **Presentation:** iOS flips after rendering while keeping main-thread ownership.
  Desktop requests two queued frames where the renderer supports that setting;
  it services events during the existing frame-cap wait at intervals no longer
  than 1 ms. This does not add a new frame cap or delay input to match audio.
- **Input:** macOS MIDI routes directly to gameplay. macOS uses a per-process
  native keyboard tap only with existing permission; Linux uses timestamped
  evdev input when devices are readable and X11 focus is reliable. Wayland,
  missing permission, and unsupported configurations use SDL fallback. Runtime
  source loss pauses gameplay before handing control to SDL and requires an
  explicit resume. No elevated input access or permission prompt is requested.
  Known source timestamps survive legacy logical transitions. Ownership tracks
  focus, stale claim epochs, disconnects, and native-to-SDL backlog handover.
- **Audio work and reporting:** the pending schedule uses circular storage;
  activation no longer moves every future event. Mixer scratch is preallocated,
  and oversized callbacks render completely in chunks with one consistent sample
  origin. iOS requests 128 frames by default and offers alternate preferences;
  accepted native callback frames/rate remain observations, not guarantees.
  Desktop explicit frame preferences also set the requested output latency.
  Settings separate observed callback period from reported output latency and
  refresh when the backend supplies its first callback or a changed format.
- **Snapshots:** a bounded mutation journal updates rotating snapshots and scene
  state by changed note IDs; graph copies are skipped when unchanged. New readers
  and journal overruns fully resynchronize. A pending publication retries when
  leased buffers become available, without blocking authoritative input work.
  Realtime selected-skin frames also apply dirty note IDs directly, reuse note
  storage after releasing the previous frame, and derive HCN activity from lane
  indexes/per-projected-note state. Legacy replay, initial synchronization,
  journal overrun, and explicitly retained immutable snapshots can still require
  full conversion/copy work.
- **Telemetry:** the optional performance build records fixed-storage histograms
  for known-source delivery, ingress/worker/sound-command stages, callback drain,
  callback duration/interval, native output lead/lateness, snapshot age, and CPU
  frame submission. Percentiles are bucket upper bounds, with an overflow bucket
  reporting the observed maximum. The former `Audio` scheduler label is corrected.
  PortAudio reports underruns; iOS underrun reporting and physical display
  presentation remain explicitly unknown. Stage histograms are not an end-to-end
  trace or a physical latency measurement.

The production macOS backend probe observed **64 frames at 48 kHz** for the
automatic setting, rather than the old inferred 898 frames. It retained the
separate 18.708 ms backend estimate, approximately 4.34 ms native output lead,
and zero reported underruns across two one-second silent start/stop cycles.
This validates the clock mapping/reporting on this route, not an acoustic gain.

The repeatable worker comparison reduced 100,000-note preparation-publication
p50/p99 from **293/424 µs to 2/3 µs**. The retained measurements and limitations
are in [worker snapshot evidence](evidence/2026-10-01-gameplay-latency/worker_snapshot_fix.txt)
and [backend timing evidence](evidence/2026-10-01-gameplay-latency/backend_timing_fix.txt).
Physical iOS, Windows, and Linux latency, iOS 60/120 Hz behavior, and dense-chart
playback under real output routes still require device measurement; no acoustic
or input-to-photon improvement number is claimed here.

Final verification, repeated after the branch review fixes below:

- `cmake --build cmake-build-debug -j 6`: passed, including the desktop app
  and all test targets.
- `ctest --test-dir cmake-build-debug --output-on-failure -j 6`: **404/404
  passed**, 131.98 seconds, against the rebuilt binaries.
- `IOS_RELEASE_BUILD_JOBS=6 scripts/ios_release_verify.sh`: passed **66/66
  native checks**, **85 Python contract checks**, the unsigned arm64 iOS Release
  build, and the resulting app's artifact audit. No distribution was performed.
- Optional telemetry paths compiled on macOS; the iOS CoreAudio backend also
  passed an arm64 syntax check. Native keyboard startup/teardown passed on macOS.
  The Linux keyboard backend
  compiled against Linux headers both with and without X11, and its ownership
  tests passed. A complete Windows/Linux app build was not performed here.
- Regression coverage includes native timestamp history, output gaps, immediate
  restart callbacks, zero/oversized callbacks, fractional sample boundaries,
  native/SDL handover, stale input ownership, journal overrun, leased snapshot
  recovery, selected-skin note storage reuse, and HCN activity transitions.

## Branch review follow-up

The full branch review found a pause/resume regression in the new native output
clock: rebasing the generated cursor to the audible position could misalign PCM
that was already generated with future scheduled sounds. Pause now freezes the
visible clock while preserving the generated cursor, PCM positions, and output
timestamp history. Resume continues that timeline. Regression tests cover
queued and completed output, repeated pauses, BGM and manual keysound tails,
scheduled onsets, concurrent callbacks, paused seek, and rate changes.

The input review also found that Linux focus-loss releases used the old claim
timestamp and that delayed macOS events could reopen a previous focus epoch.
Claim ownership and focus now have separate transitions, with releases stamped
at the observed focus boundary and stale events rejected. Linux's native worker
owns focus transitions so delayed SDL focus notices cannot clear newer input.

The native-to-SDL handoff previously flushed all buffered keyboard events while
gameplay continued. Since SDL does not retain the native event identity, this
could drop a fresh press along with duplicates. Runtime source loss now gates
input, freezes the audio clock, and requests worker suspension before held-key
releases. The main thread completes fallback with gameplay paused; the player
explicitly resumes the same attempt. Startup configurations that cannot use
native input continue to select SDL directly. Linux conservatively treats
changes in `/dev/input` as a source change.

Follow-up review covered failure during initial claim, ordinary pause-menu
resume, and ingress reopening. Claim handoff completion and a narrow session
interlock prevent those races from erasing an interruption or leaving a held
lane unreconciled. Forced course pauses resume correctly and use the existing
assisted/Modified eligibility rules when they occur during play. New tests
exercise interruption ordering, callback teardown, a stalled main thread,
held-note preservation and reconciliation, resume concurrency, and course
provenance.

The follow-up source reviews found no remaining actionable issues. The full
desktop build and 404 tests, 66 release-critical native checks, 85 Python
contract checks, unsigned iOS Release build, and artifact audit all passed
again after these fixes. The final Linux keyboard source also passed syntax
checks with and without X11 support. Hardware timing limitations above remain.
