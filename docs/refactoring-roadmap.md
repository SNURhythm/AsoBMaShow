# Workflow refactoring roadmap

Improve the repository in independently reviewable workflow slices. The standard
is code whose resource owner, thread boundaries, state transitions, completion,
and failure handling are easy to identify. File length is a signal, not a goal.

## Sequence and progress

1. **Trustworthy baseline — completed.** Build the app and affected native test
   targets before interpreting CTest results. The Records integration baseline
   and final validation are recorded in
   [its implementation plan](superpowers/plans/2026-09-13-replay-export-ownership.md).
   The Records integration checkpoint is `138a385e`: desktop app/all-target builds and
   all 367 CTest entries passed.
2. **Replay export ownership — completed.** `ReplayExportJob` owns execution,
   cancellation, queued progress, and completion. Both selection scenes retain
   their presentation and navigation responsibilities. Explicitly selected
   behavior changes are documented in the
   [Records design](superpowers/specs/2026-09-13-replay-export-ownership-design.md).
3. **Selection-scene responsibilities — initial slices completed.** Replay
   preparation and result recall use shared owned tasks and record services.
   `MainMenuPreviewController` now owns preview scheduling and deferred release,
   preserving nonblocking selection, supersession, media reuse, and
   gameplay/export handoff. The selected chart intentionally remains in the
   scene because it is also used by Start and result/replay preparation.
4. **Archive handling — cache and extraction-output boundaries completed.** `TemporaryCache`
   owns materialized-media writes, protected cleanup, measurement, and mutation
   serialization. The facade retains backend/source identity and cache naming.
   `UnzipWriteGuard` and `UnzipOutputPipeline` now separate shared extraction
   budgets, bounded output buffering, cancellation checkpoints, and writer
   lifetime from backend decoding. Existing path reservations, markers, and
   recovery remain in the archive workflow.
5. **Gameplay and skin boundaries — initial review completed.** Existing
   simulation, worker, document-loader, and resource-plan interfaces already
   separate substantial responsibilities. A subsequent gameplay slice now gives
   native input registration an explicit owner; the scene retains raw-touch
   ingress, final drain, worker handoff, and replay/result policy.

## Supporting work

- Improve tests alongside each subsystem: prefer compiled production components
  with stable interfaces while preserving behavioral coverage of existing
  extracted-method fixtures. Exact-source assertions should not substitute for
  runtime behavior checks.
- Organize CMake tests by subsystem and share targets only where dependencies
  match. Keep build changes tied to the slice they enable.
- Preserve existing behavior by default. Investigate divergences separately and
  ask the user to choose when they represent product policy.
- Verify each slice with relevant tests, build the affected targets, and run the
  full parallel suite before completion. Commit cohesive verified changes to the
  current branch and push its upstream. Deployment is a separate action.

## Preview ownership slice

The controller replaces duplicated scene-side worker/deferred-release
coordination. It preserves the difference between cancel, deferred release, and
join-for-handoff. Cancellation does not start an unstarted/stopped worker or
release media on the calling thread. Parse/load behavior remains in the existing
scene processor; this slice does not change preview playback policy.

Main Menu now holds the controller with `unique_ptr`. Its out-of-line destructor
and ordinary cleanup share the same shutdown ordering: replay preparation,
export preparation, then preview. This joins callbacks before their scene
members are destroyed, including destruction without completed initialization.

Compiled controller tests exercise blocked loading, replacement and same-path
requests, repeated idle cancellation, callback joining, retained-chart handoff,
restart, and destruction. Existing selection fixtures retain UI coverage while
calling the production controller instead of extracting the idle cleanup logic.
The Records lifecycle fixture executes the production destructor and shutdown
helper with real preparation/export workers.

The desktop app and all-target builds passed. A new teardown test initially
assumed callbacks had begun before cancellation; an explicit entry barrier now
makes its in-flight-work premise deterministic, and five consecutive runs pass.
The final `ctest --test-dir cmake-build-debug --output-on-failure -j 6` run
passed all 368 tests in 89.22 seconds. The source audit's removed cleanup-flag
checks were replaced by result-recall callback assertions in the lifecycle
fixture. Independent review found no remaining blocker, and `git diff --check`
passed. No deployment or physical-device verification was performed.

## Temporary archive cache ownership slice

`src/archive/TemporaryCache` owns materialization and cleanup under one mutex;
measurement remains an unlocked, best-effort observation. `ArchiveFile` supplies
the current root and source-derived output identity on every call. Output
identity resolution still occurs under the mutation lock after root creation.
Cleanup evaluates the live path normalizer for both protected paths and cache
entries, preserving platform aliases and active-media protection.

The public result types remain available through `ArchiveFile.h`. Existing
same-size reuse, write/cancellation errors, partial-output behavior, and exact
top-level protection semantics are preserved. Backend reads, extraction
budgets, staging/recovery, and the separate index/cache registries remain in
their existing owners.

The compiled cache tests use private temporary directories and cover reuse,
replacement, empty writes, early and post-identity cancellation, filesystem
failures, nested usage/counts, protected cleanup, live aliases, and mutation
serialization with independent measurement. A public-facade integration test
first verifies its private temporary root, then exercises naming, live
normalizer replacement, cancellation forwarding, and cleanup protection.

The desktop app and all-target builds passed. The first full suite passed
368/369 tests but exposed a video shutdown timeout; an isolated run reproduced
it. A thread sample showed `stopPredecoding` joining a decoder asleep in
`freeSpace.wait`. The stop flag had been published without either condition
variable's mutex. A separate fix publishes it under both wait mutexes and
releases them before notifying and joining. Independent lock-order review
found no cycle. The existing regression then passed five consecutive runs.

After rebuilding all affected targets, the full parallel suite passed all 369
tests in 96.98 seconds. Independent cache review found no remaining blocker,
including the Records support target's new cache dependency. `git diff --check`
passed. No deployment or physical-device verification was performed.

## Gameplay and skin boundary review

This is a structural review of the listed paths, not a complete correctness or
platform audit.

- `src/scene/play/GameplaySimulation.{h,cpp}` already owns note runtime state,
  judging, score/gauge transitions, bounded event histories, and terminal
  snapshots through time/input methods. `RealtimeGameplayWorker` owns its
  simulation, ingress queue, thread, snapshot publication, fault latch, and
  post-stop replay transfer. Keep these production interfaces and their direct
  simulation/worker tests intact.
- `GamePlayScene.cpp` still embeds `RealtimeGameplaySession`, initializes
  platform callbacks and input subscriptions, and performs shutdown in
  `stopRealtimeGameplayAuthority`. The order is meaningful: disable ingress,
  detach subscriptions, restore device claims, drain/cancel accepted touches,
  close delayed callback lifetime, stop the worker, synchronize the final
  snapshot, then transfer valid replay data. A focused session-lifetime owner
  could make this contract explicit while leaving navigation, presentation,
  and result policy in the scene.
- That gameplay extraction needs characterization first. In
  the original `tests/gameplay_terminal_scene_extract.py`, the worker-stop
  fixture took only the tail beginning at `session.worker->stop()`. The native
  registration slice below now exercises the full production method with a
  compiled registration owner. Platform raw-touch cancellation and delayed
  callback shutdown retain their existing implementations and tests; do not
  claim desktop fixture coverage verifies those iOS-only branches.
- `GameplaySkinDocumentLoader` already separates Lua/JSON/LR2 decoding and
  configuration admission from resource preparation. `PlaySkinSession::create`
  consumes its admitted document, then a `SkinResourcePreparationService`
  upload plan, movie preparation, and `SkinResourceCatalog::upload` before
  publishing a session. `OwnedActivation` holds the revision lease, Lua
  runtime, resource/movie catalogs, renderer, and bridge in deliberate
  destruction order. The resource catalog explicitly requires destruction on
  its upload owner thread. These are useful existing boundaries; moving the
  factory into another file alone would not improve ownership.
- Preserve the skin boundaries for now. If a later measured loading or
  testability issue justifies a prepared-session component, retain the exact
  revision/configuration identity, configured-Lua frame authority, cancellation
  checkpoints, upload-thread destruction, and publication-after-success
  contract. Existing resource-catalog and play-skin-session tests should anchor
  that work; do not prescribe a new decoding/rendering architecture upfront.

## Gameplay native input registration slice

`RealtimeGameplayInputRegistration` owns both native registry subscriptions,
the optional SDL event watch, and the selected device-class routing claims.
Construction registers callbacks and disables selected legacy classes while
native delivery remains gated. The scene enables raw ingress, then activates
the registration; acceptance opens before backend claims can emit input.
Autoplay still skips native registration, and each platform retains its
existing claimed and registry-routed device classes.

Close gates new delivery, removes the SDL watch, waits for registry callbacks
to return during unsubscribe, and restores backend/legacy routing. Repeated
activation and close are idempotent, a closed owner cannot reactivate, and
partial construction/activation rolls back before propagating an exception.
The owner is the session's final declared member, so fallback destruction
detaches callbacks while all their dependencies remain alive. Ordinary scene
shutdown still closes raw ingress before this owner and drains/cancels touches
before joining the worker and transferring final replay evidence.

Tests compile the production owner with the real registry and SDL event system.
They cover staged/active/closed routing, destruction, partial startup and backend
activation failure, and input/device/SDL callbacks in flight. The existing
terminal fixture now invokes the full production shutdown method and asserts
ingress closure, native detachment, and touch draining before real worker stop,
while retaining its accepted-replay transfer and terminal/navigation checks.
The two registry test targets share only their matching backend dependencies
through `cmake/RealtimeInputTests.cmake`; the terminal fixture uses that support
target for its real registration integration.

The desktop app/all-target build and all 370 parallel CTest entries passed
(104.12 seconds). Review also tightened a fixture callback's captured-state
lifetime; the rebuilt terminal fixture and registration tests passed afterward.
No deployment or physical-device verification is included in this slice.

## Archive extraction-output slice

`src/archive/UnzipOutput` contains the production byte/entry/free-space guard,
shared cancellation checkpoint, and bounded writer pipeline previously embedded
in `ArchiveFile.cpp`. Miniz, libarchive, unarr, and 7-Zip selection and decoding
continue through the existing facade; extraction output policy no longer needs
to be read or tested inside those adapters. Public limits, execution-plan, and
shared-budget types remain available through `ArchiveFile.h` via `UnzipTypes.h`.

One guard belongs to each archive and shares batch admission through the budget
mutex. Stream writes are serialized per guard. One pipeline owns its staged
chunk, queued stream references, capacity accounting, and optional writer
thread. Backend producers finish before flush/destruction; destruction drains
the staged tail and joins before the guard or cancellation dependencies die.
Inline writes retain caller-owned checkpoint behavior. Worker cancellation,
pause rejection, checkpoint exceptions, stream errors, first budget failure,
and pending disk reservations retain their existing distinctions.

Direct compiled tests cover exact/shared byte and entry limits, pending
reservation release, missing destinations, reserved free-space rejection,
stream failure accounting, checkpoint order, inline thresholds, file switching,
output retention, sticky cancellation/failure, backpressure, and destructor
joining. Existing backend regression fixtures retain coverage of extraction,
active/reserved output protection, and recovery. Lightweight cache/output tests
are grouped in `cmake/ArchiveStorageTests.cmake` without backend or media links.

The desktop app and all-target builds passed. The output-policy and archive
concurrency tests passed together, followed by all 371 parallel CTest entries
in 124.91 seconds. Independent review found no remaining behavior, lifetime,
or build-wiring blocker, and `git diff --check` passed. No deployment or
physical-device verification was performed.

## Roadmap completion and further work

The initial sequence has delivered a verified baseline, shared replay-export
and Records preparation ownership, explicit preview scheduling, independent
archive cache/output policy, and the gameplay/skin boundary review with native
input registration ownership. Tests and CMake organization changed alongside
the workflows they support. The selected skin decoding/rendering boundaries
were retained based on implementation evidence.

Further candidates are documented in
[Next workflow refactoring targets](refactoring-next-targets.md), with concrete
ownership problems, behavior constraints, and characterization requirements.
Settings cache-maintenance jobs and pacemaker best-replay loading are completed
as follow-ups below, along with archive index-build coordination. Settings
library-job ownership is the next slice.

## Follow-up: Settings cache-maintenance ownership

`SettingsCacheMaintenance` owns cleanup/measurement jobs, admission, running
state, generation checks, typed completion storage, and cleanup-first joining.
The scene supplies cache facade operations; cleanup captures the externally
owned jukebox rather than the scene and reads its active materialized paths
on the worker. Both scene cleanup and destruction stop/join the owner. Workers
no longer format or publish UI strings through scene members.

Cleanup still allows its filesystem operation to finish after cancellation;
measurement still receives a stop token. Cleanup may overlap a prior
measurement, measurement remains blocked during cleanup, and completed workers
are joined before reuse. A targeted correctness improvement makes generation
validation/publication atomic with admission and discards previously queued
results when a newer request starts. An old measurement can no longer replace
the newer cleanup status or progress.

The compiled tests exercise real cache measurement/protected cleanup in private
directories, duplicate admission, deterministically late measurement,
previously queued completion, filesystem errors, stop/restart, and destruction
while cleanup is blocked. Complete production scene methods run against this
owner and small view doubles to check application-thread delivery, button
restoration, errors, and layout refresh. Temporary mutations removing the
generation guard or admission's completion reset each failed the corresponding
regression test. Review found no remaining blocker.

The desktop app and all-target builds passed, followed by all 372 parallel
CTest entries in 101.71 seconds. `git diff --check` passed. Verification was
local to the desktop build; no deployment or physical-device checks were run.

## Follow-up: Pacemaker best-replay task reuse

`GamePlayScene` replaces its best-replay thread, shared cancellation flag,
mutex, and pending replay with the existing `ReplayRecordTask`. A small
`BestReplayLoad` function performs cancellation-before-replacement, constructs
the resolver on the worker, and publishes only a successful uncancelled load.
The resolver's runtime profile-root access remains on that worker. The
scene callback is deferred until the application consumes it and the worker
has been joined; it only updates the personal-best ghost. The selected target
continues to use its existing proportional progression.

Reuse was checked against the shared task's actual semantics: `start` joins
without cancelling, so the loading function explicitly cancels first;
completion remains active until consumed; failed/no-result work produces a
no-op completion; cancellation discards queued and late callbacks. Existing
configuration, cleanup, and destructor stop boundaries remain in place. No
additional task class, mailbox, or cancellation authority was introduced.

Direct tests use the production resolver and shared task to check exact
attempt/path forwarding, worker construction, caller-thread completion,
replacement during a blocked load, cancellation, missing/mismatched/unreadable
results, and destruction. The scene fixture compiles complete production
start/apply/stop methods with real pacemaker policy; it distinguishes the
personal-best ghost's first-note score of 2 from the selected target's
proportional score of 1 and verifies stopped loads cannot update a replacement
chart. A temporary mutation removing cancellation-before-replacement failed
the blocked-consumer regression. Independent review found no blocker.

The first full suite passed 372/373 entries but exposed an intermittent
renderer characterization mismatch, also reproduced in isolated repeats.
Added JSON differences identified swapped long-note collection groups at
distinct depths. The renderer's pointer-keyed lookahead map has no stable CPU
iteration order; bgfx's main view orders their draws by depth. The fixture now
stably orders contiguous main-view texture-note runs by depth for both saved
and current trace comparison. Equal-depth order, nontexture pass boundaries,
geometry, missing/duplicated primitives, and PNG checks remain covered.
The golden files were not regenerated. The new ordering probe failed before
the correction; afterward the renderer test passed 20 consecutive runs.

After rebuilding all targets with that fixture correction, the full parallel
suite passed all 373 tests in 103.40 seconds. `git diff --check` passed.
Verification was local to the desktop build; no deployment or physical-device
checks were run.

## Follow-up: Archive index-build coordination

`IndexBuildCoordinator` now owns one active build record per archive key and
movable builder/waiter leases. Builder destruction publishes failure unless
explicitly completed; waiters retain their original flight through later
retries. This removes the active/done/failed/waiter map protocol from the
archive facade and prevents a newer build from replacing an older waiter's
outcome. Cancellation and checkpoint exceptions remain local to the waiter.

The facade still validates sources and cached indexes, handles persistent
storage and backend decoding, and decides when a successful but mismatched
result needs another build. It rechecks usable cache data after waiting,
including after a failed flight, and after builder admission. Live-manifest
promotion and the current cache-retention policy are unchanged. No cache lock
is acquired while holding the coordinator's mutex.

Eight direct cases cover admission, independent keys, retained failure across
new success, cancellation, unlocked checkpoints, builder/waiter exceptions,
retry competition, and move/abandonment ownership. Focused coordinator and
archive concurrency tests passed together. Independent review found no
remaining behavior, lifetime, or build-wiring blocker.

The desktop app and all-target builds passed, followed by all 374 parallel
CTest entries in 111.83 seconds. `git diff --check` passed. No deployment or
physical-device verification was performed.

## Follow-up: Settings library-worker destructor safety

Inspection of the next ownership candidate found a concrete teardown gap:
`difficultyTableJobThread` was declared before its callback status fields, so
implicit thread destruction joined only after those fields had died. Normal
scene cleanup joined explicitly, but direct scene destruction did not.
The destructor now requests stop and joins while all members remain alive.

A fixture compiles the complete production destructor with a real thread and
a dependency-lifetime sentinel in the same relative declaration order. It
failed before the fix and passes afterward, checking both stop signaling and
joining an operation that finishes after cancellation. Idle and previously
joined destruction also pass. The desktop build and both Settings worker
fixtures passed; independent review found no blocker. The preceding full-suite
baseline was 374/374; this bounded fix adds the focused lifecycle regression.

## Follow-up: Settings table/folder task ownership

`SettingsLibraryTask` now owns the single table/folder worker, running state,
and typed optional status/progress updates with accumulated reload requests.
The scene's five operations capture repository and request data instead of the
scene. UI messages retain their wording; colors, URL completion, modal state,
and reload/layout work remain on the application thread after consuming data
outside the owner mutex.

The owner holds admission until work returns, joins prior completed work before
reuse, and preserves queued completion across admission. Stop suppresses late
updates, joins operations that cannot be interrupted, and clears queued data.
Both explicit scene shutdown paths and owner destruction join before callback
dependencies disappear. Importer progress callbacks are synchronous on the
worker, so their borrowed publisher stays valid. Scanner stop tokens, iOS
security-scoped folder handles, and delegated background rebuild are unchanged.

Direct tests exercise admission, channel coalescing, reload accumulation,
completion retention, cancellation, restart, and capture lifetime. Generated
fixtures compile complete production scene methods with the real task and URL
policy to check progress, success/failure, edited URLs, table update/delete
confirmation, absent views, and application-thread delivery. The production
destructor fixture now uses the real task. Focused tests passed, and independent
review found no remaining behavior, lifetime, or build-wiring blocker.

The desktop app and all-target builds passed, followed by all 376 parallel
CTest entries in 102.45 seconds. `git diff --check` passed. No deployment or
physical-device verification was performed.

## Follow-up: Main Menu Find BMS destructor safety

The next teardown audit found the same declaration-order gap in Main Menu:
normal cleanup cancelled/joined Find BMS, but the destructor stopped only
replay/preview work. Find BMS status fields would die before the thread's
implicit destructor joined, while its callback could still publish results.
The destructor now sets the search API's atomic cancellation flag, requests
thread stop, and joins before member destruction, preserving cleanup order.

The existing fixture compiles the production destructor with a real Find BMS
thread and a dependency sentinel. The added regression failed before the fix;
afterward it verifies both cancellation signals and waits for an operation
that finishes after cancellation. The desktop build and Main Menu lifecycle
fixture passed, and independent review found no blocker. The preceding full
suite passed 376/376; this bounded fix extends an existing test entry.

## Follow-up: Find BMS task ownership and completed-result admission

`FindBmsTask` owns the search/download/artifact worker, service cancellation,
latest 160 progress events, and result handoff. Scene operations capture request
values only; their service calls, selected-chart generation, indexing requests,
progress formatting, and dialog policy retain their existing boundaries.
Nonblocking cancellation still delivers the service outcome, including a pending
artifact. Stop joins uncancellable transactions and discards queued updates.

Review found a race in the initial extraction: refreshing after launch could
observe a completed worker while the scene still held the previous pending
artifact. Keep/Delete could be re-enabled before the first transaction's result
was consumed. The owner now remains busy and rejects new admission while a
result awaits delivery. Explicit replacement still stops/joins and discards.

A deterministic regression waits for thread-callable capture destruction after
publication, verifies the real dialog policy and complete production artifact
launch reject a duplicate action, and checks exactly-once indexing. A temporary
mutation restoring early-idle reporting fails that policy assertion. Other
owner and compiled scene tests cover progress bounds/order, request values,
cancellation with pending artifacts, replacement, candidate validation,
keep/delete outcomes, UI handoff, and destruction. The archive-flow source audit
now references the owner while retaining the tested dialog-policy requirement.

The first full suite also exposed an unrelated archive worker-budget fixture
assumption. Diagnostics reproduced successful extraction of two archives with
two workers each but only three globally distinct thread IDs: an inner worker
ID was reused after its first lifetime ended. The fixture now checks each
archive's two-worker allocation directly. This preserves the shared-budget
assertion without assuming thread IDs remain globally unique after exit.

The corrected archive fixture passed 20 consecutive runs. After rebuilding all
targets, the full parallel suite passed all 377 tests in 103.95 seconds. Review
found no remaining blocker and `git diff --check` passed. Verification stayed
local to the desktop build; no deployment or physical-device checks were run.

## Follow-up: IR upload preparation task ownership

`ir_uploads::PreparationTask` now owns preparation worker lifetime, the existing
durable-enqueue gate, and typed progress/completion storage. The scene captures
the external application context for verification/draft/enqueue dependencies;
workers no longer capture the scene. `prepareSelectedCandidates`, durable
batch mapping, and controller selection/failure policy are unchanged.

Stop enters gate cancellation before requesting thread stop and joining. This
preserves cancellation before enqueue and the outcome of a batch already
started. Stopped completion remains available; scene initialization and cleanup
explicitly reset it. Consuming completion joins before returning updates, and
admission rejects existing work or an unconsumed outcome.

Tests exercise progress/partial failure, cancellation before enqueue with zero
batch calls, cancellation after enqueue begins with a retained queued outcome,
stop/consume/restart, explicit reset, and destruction while verification runs.
Generated fixtures compile complete production launch/apply/stop methods with
the real owner, controller, and batch mapper, checking provider/selection gates,
one batch, application-thread progress/completion, and retained cancelled
selection. Existing domain tests remain intact, grouped with this fixture in
`cmake/IrUploadTests.cmake`. Focused tests and desktop compilation passed;
independent review found no remaining blocker.

The flow audit now follows the scene's stop delegation into the owner and checks
cancellation-before-stop-before-join ordering there. After rebuilding all
targets, the full parallel suite passed all 377 tests in 97.20 seconds.
`git diff --check` passed. Verification stayed local to the desktop build.

## Follow-up: Music Select destruction uses guarded cleanup

`MusicSelectScene` now invokes `Scene::cleanup()` in its derived destructor, so
scene-capturing workers and input subscriptions are stopped while their member
dependencies remain alive. This covers direct destruction and owning-pointer
unwinding after failed initialization. Normal SceneManager cleanup still uses
the same implementation and its existing once guard.

The regression compiles complete production destruction/cleanup and selected
cancellation helpers, plus actual base cleanup and view disposal. Real Records
and export owners run blocked work; other controlled resources check launch,
preload, Lua preparation, input, preview, view, and scoped-access ordering.
It exercises direct, already-cleaned, uninitialized, and exception-unwind cases,
including deferred callback disposal. Focused tests and desktop compilation
passed; independent review found no blocker.

After rebuilding all targets, all 377 tests passed in the parallel suite in
107.87 seconds. `git diff --check` passed. No deployment was performed.

## Follow-up: Profile archive worker ownership

`ProfileArchiveWorker` owns the existing one-shot controller task's execution,
completion mailbox, and thread. Settings passes an after-execution callback
that retains import temporary-document cleanup and warning policy. Stop still
waits for the non-interruptible operation and cleanup, then discards completion.
Consuming completion joins outside the mailbox mutex before scene application;
completed but unconsumed work remains unavailable for another admission.

Generation/controller decisions, picker state, export staging retention, and
launch-failure recovery remain in Settings. Direct tests use real controller
tasks to cover admission, result/capture lifetime, stop/destruction through
blocked cleanup, restart, and existing exception mapping. Compiled complete
production scene methods cover cleanup warnings, both launch-failure catches,
rejected admission/pickers, exactly-once application, controller release after
joining, and native export source retention. Existing controller tests remain
in the same registered target, grouped in `cmake/ProfileSettingsTests.cmake`.
Focused tests, desktop compilation, and all-target compilation passed;
independent review found no blocker.

The full parallel suite passed all 377 tests in 106.63 seconds after rebuilding
all targets. `git diff --check` passed. Verification remained local; archive and
document effects in the new scene fixture use controlled test dependencies.

## Follow-up: Chart Viewer destruction releases listening audio

The explicit Chart Viewer destructor now calls the base cleanup-once entry
point while derived members remain alive. Existing cleanup still stops active,
loaded, or retained listening state before chart release. Inactive/unused
viewers leave the shared jukebox alone; practice handoff and normal cleanup
policy are unchanged.

The generated regression compiles complete production derived/base cleanup and
checks all eight listening-flag combinations through direct destruction, prior
cleanup, and exception unwinding, plus unused state and deferred capture
release. A negative control using the former default destructor fails the
stop-before-chart-release assertion. Existing geometry checks and the new
lifecycle target are grouped in `cmake/ChartViewerTests.cmake`. Focused tests,
desktop compilation, and all-target compilation passed; review found no blocker.

After all-target compilation, the full parallel suite passed all 378 tests in
104.72 seconds. `git diff --check` passed. Audio/view effects in the new fixture
are controlled doubles; no platform deployment or physical-device test ran.

## Follow-up: Music Player releases only its owned video state

The explicit Music Player destructor calls guarded cleanup only when fullscreen,
loaded-video, or visual-restoration state remains owned by the scene. Existing
cleanup unloads visuals and restores the previous setting before chart/view
release. Unused and already-exited scenes leave shared BGA state untouched;
destruction does not refresh UI or stop native music playback.

The generated fixture compiles complete production acquisition, fullscreen
exit, cleanup, destructor, and base cleanup/view disposal. It exercises BGA and
artwork fallback, both previous visuals values, partial acquisition exceptions,
normal cleanup, unused/no-selection destruction, and shared-state changes after
exit. Negative controls for default and unconditional-cleanup destructors both
fail the expected ownership assertions. Focused tests, desktop compilation,
and all-target compilation passed; review found no blocker.

The full parallel suite passed all 379 tests in 106.15 seconds after all-target
compilation. `git diff --check` passed. Verification used local desktop tests
with controlled native effects; no deployment was performed.

## Follow-up: Parallel work ownership and count width

`ParallelWork.h` groups the active sizing/indexed-execution helpers behind the
existing `Utils.h` include facade. The unused integer-range parallel API and
thread wrapper were removed after a repository-wide caller search. Indexed
execution retains callable borrowing, dynamic assignment, and sequential
behavior; joining thread owners now outlive their work but die before the
borrowed state during return or launch-failure unwinding.

Worker sizing retains hardware fallback and render/audio headroom. It compares
at `size_t` width before narrowing the bounded result, fixing the former wrap
to zero for counts above `unsigned int`. Standalone tests cover deterministic
policy boundaries, exactly-once execution, borrowed move-only work, and joining
before return. The old-narrowing negative control fails its intended boundary
assertion. Desktop compilation and focused tests passed; review found no blocker.

After rebuilding all affected targets and checking the final incremental build,
the full parallel suite passed all 380 tests in 132.28 seconds.
`git diff --check` passed. Verification remained local to desktop builds/tests.

## Follow-up: Sleep-timer shutdown cannot lose its wake

Music Player shutdown now requests timer stop under the same mutex used to
check the worker's wait predicate. The final notification and join still occur
outside both timer and thread mutexes. This prevents the worker from missing
the stop notification between predicate evaluation and wait registration.

The generated fixture executes the complete production timer methods with a
controlled scheduling pause in that gap. The old implementation failed the
expected early-notification assertion; the fixed version passed. Additional
cases cover timer replacement, clear, remaining time, repeated stop/restart,
expiry messages, and shutdown joining an in-progress expiry callback while
the timer mutex stays available. Desktop compilation and focused tests passed;
independent review confirmed the lock order and improved one positive test
deadline for loaded hosts. Native playback/status effects use controlled doubles.

After all-target compilation, the full parallel suite passed all 381 tests in
108.74 seconds. The review-only positive deadline adjustment was rebuilt and
its focused test passed again. `git diff --check` passed.

## Follow-up: Local archive and scanner batches own joining workers

Four remaining local worker vectors now use `std::jthread`: direct ZIP reads,
random-access RAR reads, parallel RAR5 reads, and individual-chart parsing.
Launch loops and explicit joins are unchanged. On partial launch failure, owned
threads now join before local captures disappear and the exception propagates.

Independent review confirmed declaration order, the absence of a full-pool
barrier, and the no-stop-token invocation under both standard and Android
compatibility thread owners. Work assignment, cancellation, memory admission,
and exceptions escaping worker bodies retain their existing behavior.

Desktop and all-target compilation passed, followed by all 381 parallel CTest
entries in 116.54 seconds. `git diff --check` passed. Partial-launch cleanup for
these local batches was checked by ownership review; backend concurrency and
scanner behavior were exercised by the existing suite.

## Follow-up: Persistent worker pools roll back partial construction

The chart-scan scheduler and image-decode coordinator now call existing shutdown
logic before propagating a constructor-body reserve or launch exception. Idle
workers are stopped, notified, and joined while all captured members remain
alive. Ordinary enqueue, prioritization, cancellation, and decode behavior is
unchanged.

The direct fault-injection target links the real implementations and replaces
scalar allocation only in that isolated test executable. It measures successful
construction and fails each allocation on the constructing thread in turn,
leaving worker and cleanup allocations alone. Both old implementations terminated
on the sweep; both fixed implementations propagate all eleven allocation failures
observed per pool on this runtime. Fresh pools construct and destroy after every
failure. These checks cover allocation failure, not an injected OS thread error.

The existing normal tests and two failure modes are grouped in
`cmake/WorkerPoolTests.cmake`, preserving original normal-test definitions and
registration. Failure modes have explicit timeouts. Desktop compilation and
independent review passed.

All four focused tests passed after rebuilding the affected desktop/test
targets. The all-target build and full parallel suite then passed all 383 tests
in 115.13 seconds. `git diff --check` passed.

## Follow-up: Intro destruction detaches its input listeners

The Intro scene now invokes existing guarded cleanup from its explicit
destructor. Both callbacks capturing the scene are removed before its adapter
and views disappear, including unwinding after partial input registration.
Normal SceneManager cleanup and navigation behavior remain unchanged.

The generated fixture executes complete production start/stop, cleanup, and
destructor methods with actual base cleanup/view disposal. The real input
registry handles delivery and cancellation; a controlled backend and observed
adapter/views verify ordering. Cases cover direct destruction, normal repeated
cleanup, unused state, failure of the second registration, queued input/device
events, unrelated listener retention, and deferred capture disposal. The old
implicit destructor fails the expected live-subscription assertion.

Desktop compilation, both focused tests, and independent review passed. After
the all-target build, all 384 tests passed in 109.85 seconds in the parallel
suite. `git diff --check` passed. The fixture controls adapter/view effects and
uses application-thread registry delivery; it does not claim native callback
concurrency coverage.

## Follow-up: Input capture rolls back a partial subscription pair

The capture controller removes its input subscription if device registration
fails during construction, then rethrows the original exception. That closes
the callback lifetime gap left when the constructor fails and its destructor
cannot run. Normal registry delivery and teardown remain on the application
thread.

The failure test uses the actual controller, resolver, profile, and configuration
sources with a controlled registry boundary. It checks first/second registration
failure, retained callbacks without invoking dangling references, saved-callback
capture disposal, unrelated listener survival, and successful resolver delivery.
The old implementation failed the intended retained-callback assertion. Existing
normal tests and the failure target share controller source definitions in
`cmake/InputCaptureTests.cmake`; original native-backend setup is preserved.
Desktop compilation and independent review passed.

Both focused tests passed, followed by the all-target build and all 385 tests
in 111.57 seconds in the parallel suite. `git diff --check` passed. Registry
failure behavior is controlled in this fixture; the existing normal target
continues to exercise the real registry and native backend stack.

## Follow-up: Remove the obsolete application thread list

`ApplicationContext` no longer declares an unused generic thread list or logs
and iterates over it during shutdown. No code registers a worker there; the
real workers already belong to explicit subsystem owners. Repository-wide
searches and independent review confirmed the dead boundary. The surrounding
service shutdown order is unchanged, and other application diagnostics remain.

Desktop and all-target builds passed. The first full suite passed 384/385 tests;
the ledger's unchanged Lua runtime runner failed a callback visibility assertion
without reporting its underlying diagnostic. That ledger passed in isolation,
then the unchanged full suite passed all 385 tests in 100.59 seconds. Its tight
callback wall-time budget is a possible cause, not a confirmed diagnosis. No Lua
policy or test limits were changed. `git diff --check` passed.

## Follow-up: Picker launch rollback and pending-result admission

The library and sound-set picker paths clear active ownership before rethrowing
a join/thread-launch exception. Sound-set requests now claim active ownership
before checking the previous worker's published result. This prevents a racing
request from admitting a new native pick over an unconsumed result.

Two generated executables compile the full production picker methods under iOS
and Android branches. Native dialog and database effects are controlled, while
worker creation and publication use real threads/atomics. One-shot allocation
failure checks launch rollback/retry; a pause before the active exchange checks
publication during admission. All six old-code modes failed the expected active
leak or second-pick assertions. The fixed modes verify retry, pending-result
retention, one-time consumption, later admission, and Android direct/import
enqueue policy. Each has an explicit timeout, and fixture generation is shared.

Desktop compilation, six focused cases, and independent review passed. Native
picker calls, bookmarks, cancellation checks, and platform routes are unchanged.
Validation is local simulation of those branches, with no deployment or native
dialog operation.

The all-target build passed. An initial full run exhausted disk space because
old play-skin test snapshots were not removed from the system temporary folder.
After removing only stale generated fixtures, the unchanged full suite passed
all 391 tests in 102.00 seconds. `git diff --check` passed. Fixture cleanup is a
separate follow-up.

## Follow-up: Remove read-only play-skin test snapshots

The full-suite disk failure exposed a fixture lifecycle defect: the play-skin
session runner ignored `remove_all` errors beneath immutable snapshot roots.
Its temporary owner now grants owner permissions to directories before descent,
adds file write permission on Windows, and reports traversal/removal failures.
It uses non-following status and traversal so external symlink targets retain
their permissions and data.

A regression in the existing runner creates nested read-only directories and
a read-only file, verifies destruction removes the owned root, and on POSIX
checks an external symlink target before its separate owner cleans it up.
The actual old destructor failed the root-removal assertion; the fixed runner
passed. Independent review found no issues. Windows file handling follows the
snapshot writer's attribute policy but was not executed on this macOS host.

The all-target build and all 391 tests passed (97.34 seconds). Comparing the
runner's temporary-directory prefix before and after the full suite found zero
new leftover fixtures. `git diff --check` passed.

## Follow-up: Roll back replay task thread startup

The shared replay/Records preparation task previously retained active ownership
and accepted completion publication after worker construction failed. Its
launch catch now invokes the existing cancellation cleanup before rethrowing.
Normal worker execution and the join/token-allocation order before admission
are unchanged.

The existing direct runner arms an allocation failure on its requesting thread
only after the real task reports active. This allows cancellation-token setup
and fails actual `jthread` construction. The old source failed both idle-state
and late-publication assertions. The fixed regression checks callback capture
release, no failed-work execution, late-publication rejection, and a successful
retry with a fresh token and one-time completion. Desktop and focused checks
passed.

Independent review, the all-target build, and all 391 tests passed (99.01
seconds). `git diff --check` passed.

## Follow-up: Explain private-arity visibility assertion failures

The Lua runtime regression now identifies the safety policy and prints any
runtime failure code/message before asserting private debug-library visibility.
It also checks the returned boolean without throwing a variant-access error.
This makes a future callback-budget failure distinguishable from the callback
returning false; it does not establish the cause of the earlier intermittent
ledger failure. Production policy, time budgets, and fixtures are unchanged.

The runtime target rebuilt, and both its direct CTest case and the music-select
ledger evidence contract passed (5.48 seconds). Independent review and
`git diff --check` passed. Validation was scoped to these affected runners.

## Follow-up: Allow identical preload retries after launch failure

Preload admission stored its pending chart before starting a worker. If thread
creation threw, the next request for that chart was discarded as a duplicate
even though no worker existed. The request now invokes existing cancellation
cleanup before rethrowing an `ensureWorker` exception. Successful request and
debounce behavior is unchanged; pending idle notification follows the existing
cancel/restart path.

The direct runner warms and measures allocations on the requesting thread,
then fails its final allocation in actual thread startup. The old source failed
pending-state, identical-retry, and one-time-processing assertions. The fixed
test checks exception propagation, no failed-work execution, cleared admission,
and bounded successful processing of an identical retry. Desktop compilation,
the focused runner, and independent review passed.

The all-target build and all 391 tests passed (101.87 seconds).
`git diff --check` passed.

## Follow-up: Remove the obsolete music-select catalog rebuild

Repository-wide searches found no callers of `refreshResources`; its
`requiresResourceRefresh` gate always returned false. Removed that unreachable
catalog rebuild and the private device/counter owners and stop token used only
by it. Initial upload/finalization still receives the same context, and the
resource and movie catalogs independently retain their lifetime dependencies.
The public compatibility query and active image/font patch paths are unchanged.

Existing session tests cover incremental images, font patches, cancellation,
resource ownership, and the compatibility query. No new test was added for
deleting unreachable code.

Desktop and all-target builds passed, independent review found no issues,
and all 391 tests passed (93.71 seconds). `git diff --check` passed.

## Follow-up: Deliver replay export startup failures to the scene

Export admission occurs before scene preview/UI handoff. Thread-construction
exceptions previously escaped without a result, leaving those scene flags
busy. The job now publishes startup failure through the same result mailbox
as export failures. Admission remains owned until the application thread
consumes that terminal result; `takeResult` supports completion without a
worker. A private publisher shares the mailbox lock with normal completion.

A scoped one-shot allocation hook is linked only into the affected tests.
Work/options are constructed before arming it, so the tests fail actual thread
startup. Old source failed the direct result-channel assertion and both scene
consumer assertions. The fixed direct case verifies capture release, no worker
execution, retained admission, one-time diagnostic delivery, and retry. Existing
compiled scene fixtures run complete production entry/result methods to verify
Main Menu busy/status/preview recovery and selector Records recovery with or
without a modal. Desktop compilation and all three focused cases passed.

Independent review, the all-target build, and all 391 tests passed (101.92
seconds). `git diff --check` passed.

## Follow-up: Share read-only cleanup with the loading benchmark

An isolated loading-benchmark test passed but left two temporary snapshot roots
behind. Its owner now uses the same non-following permission restoration as the
session fixture through a small test-only helper. Cleanup still attempts removal
after a permission/traversal failure and reports errors to the owning runner.

Review also found that benchmark fixture destruction occurred after `main`
evaluated its exit code. Benchmark work now runs in an inner owning function;
the final status and success message are decided after cleanup. Existing work
failure and invalid-argument statuses are preserved, including JSON report modes.

Both focused runners passed with zero new directories under either fixture
prefix. Default, cold/warm JSON, invalid-argument, and missing-input CLI checks
also preserved their expected results without leaving fixtures. Independent
review found no issues. The existing nested read-only/external-link regression
tests the shared cleanup through its real temporary owner.

The all-target build and all 391 tests passed (101.40 seconds). The full run
left zero new directories under either tracked fixture prefix.
`git diff --check` passed.

## Follow-up: Clean immutable lifecycle and settings test roots

Lifecycle, settings, commit-coordination, and package-operation tests passed
while leaving 30, 13, 7, and 1 temporary roots behind respectively. Their
temporary owners now share the established read-only cleanup and report errors
through their existing runner mechanisms. Dependency teardown precedes cleanup,
including parent-owned fixtures in the commit coordinator's fork/death tests.
All cleanup completes before each runner evaluates its final status.

The four rebuilt focused runners passed with zero new directories under their
tracked prefixes. Review confirmed ownership/reporting order. Explicit namespace
qualification distinguishes the general helper from skin-specific test fakes.

The all-target build and all 391 tests passed (102.11 seconds). The full run
left zero new directories under all four tracked prefixes.
`git diff --check` passed.

## Follow-up: Consolidate remaining snapshot cleanup loops

Archive importer, tree snapshotter, package store, and Lua filesystem temporary
owners now use the established read-only tree cleanup. This removes four
permission loops that could follow symlinks when changing permissions and
silently discarded cleanup errors. Existing runner failure counters now receive
cleanup errors before final status; fixture construction and teardown order are
preserved. All four rebuilt focused cases passed with zero new fixture roots.
Independent review found no issues.

The all-target build and all 391 tests passed (102.51 seconds). The full run
left zero new directories under all four tracked prefixes.
`git diff --check` passed.

## Follow-up: Claim snapshot test roots exclusively

The snapshotter's deterministic temporary path could reuse another run's
existing directory and delete it during teardown. A private temporary-parent
check reproduced this: the old runner passed while removing a pre-existing
sentinel. Construction now requires `create_directory` to claim a new path,
matching the session and Lua fixture convention.

The rebuilt runner preserved the sentinel and passed three paired concurrent
runs with no leftover roots. These checks use a short owned temporary parent
to stay within the existing Unix-socket fixture's path-length limit. Focused
CTest and independent review passed. `git diff --check` passed. The preceding
cleanup consolidation established the unchanged 391-test application baseline.

## Follow-up: Restore gameplay worker state after launch failure

The gameplay worker set its admission flag before constructing its thread. A
launch exception left `running()` true and prevented immediate retry. Startup
now releases admission before rethrowing the original exception; normal thread
execution and gameplay fault reporting are unchanged.

A one-shot allocation failure in the existing runner reproduced the old false
running state. The fixed regression checks exception propagation, stopped state,
no audio or gameplay fault, immediate retry, duplicate-start rejection, one
actual input/audio transaction, and shutdown. The hook is linked only into the
test executable. Focused CTest and independent review passed.

Desktop main and all-target builds passed, followed by all 391 tests (90.98
seconds). `git diff --check` passed.

## Follow-up: Contain image probe path preparation failures

The BMS image-availability probe declared `start` as `noexcept` but constructed
its resource path before entering the startup exception handler. Moving that
construction inside the handler preserves the existing completed/unavailable
failure result when path allocation fails.

A regression in the image decoder runner preconstructs all arguments before
arming the shared allocation hook. The old implementation terminated with
uncaught `bad_alloc`; the fix completes without calling the decoder and allows
a successful retry. Focused CTest and independent review passed. Caller-side
argument construction remains outside the method's exception boundary.

Desktop main and all-target builds passed, followed by all 391 tests (102.93
seconds). `git diff --check` passed.

## Follow-up: Bound the Unix socket fixture path

The snapshotter test copied a temporary path into `sockaddr_un::sun_path`
without checking its size. It now requires room for the path and terminating
zero before copying or creating a socket descriptor. An overlong path records
a clear test failure and still cleans the owning temporary directory.

The rebuilt normal focused test passed. A deliberately long private temporary
parent produced exactly one expected fixture diagnostic and exit status 1,
without leaving roots behind. Independent review and `git diff --check` passed.
The preceding image-probe fix established the unchanged 391-test application
baseline.

## Follow-up: Prepare folder-status ownership before admission

The folder-status loader committed requested rows before allocating processor
ownership and creating its worker. Failure could strand deduplication state or
consume the delayed-retry state. Resource preparation now follows the duplicate
fast return but precedes those state changes. The prepared callback is declared
before the lock so exception unwinding unlocks before destroying its captures.

The shared test-only allocation hook now supports an allocation countdown while
preserving existing next-allocation callers. A new regression walks actual
caller allocations, checking capture release, no failed work/results, identical
retry, and one result/call. The existing delayed-retry test similarly verifies
readiness survives failure and the retained row is eventually delivered once.
The old implementation failed fresh admission and delayed readiness assertions.
This change addresses startup preparation; it does not claim a strong exception
guarantee for every later priority or completed-row mutation.

Focused CTest, independent review, and desktop main/all-target builds passed.
All 391 tests passed (102.78 seconds), including the existing priority/scene
contracts and other allocation-hook consumers. `git diff --check` passed.

## Follow-up: Prepare directory workers before request ownership

The directory loader previously stored its pending callback before creating
the worker, retaining captures when startup failed. Worker preparation now
precedes request-state changes under the same lock. The worker cannot consume
an incomplete request, and exception unwinding unlocks before capture release.

The direct runner walks caller allocation points until successful admission,
checking failed-request capture release, no work/results, and an immediate
retry with one matching identity/generation and one callback call. The old
implementation failed the capture-release assertion. Focused CTest and
independent review passed.

Desktop main/all-target builds and all 391 tests passed (101.55 seconds).
`git diff --check` passed.

## Follow-up: Claim catalog test directories exclusively

Resource-catalog and movie-catalog fixtures now claim deterministic serial roots
with `create_directory`, retrying occupied names. Both old runners passed but
deleted a preseeded sentinel in a private temporary parent. Both rebuilt runners
preserve that sentinel in single and paired concurrent runs without leaving
owned roots. Focused CTest, independent review, and `git diff --check` passed.
The preceding directory-loader change established the 391-test baseline;
this change only adjusts the two fixture constructors.

Separately, all 19 audited skin fixture runners passed isolated cleanup checks
with zero leftover roots, so no broad read-only cleanup migration was made.

## Follow-up: Deliver cache-operation exceptions as failures

Cache cleanup and measurement exceptions now use the owner's existing typed
failure result instead of escaping the worker. Named exceptions retain their
message, while empty/unknown exceptions receive a fallback. Running-state reset,
generation filtering, and stop suppression remain shared with ordinary outcomes.

The old runner aborted on an operation exception. The direct regression covers
both operations, three exception forms, same-operation retry, and late failure
suppression after supersession or stop. A compiled scene regression checks
failure text/colors, cleanup-button reset, and successful recovery on the
application thread. Focused CTest and independent review passed.

Desktop main/all-target builds and all 391 tests passed (102.81 seconds).
`git diff --check` passed.

## Follow-up: Exclude linked targets from cache byte totals

Cache byte accounting now ignores symbolic-link targets when visiting entries
or sizing a top-level cleanup candidate. A private-root probe containing 7 owned
bytes previously reported 35 bytes used and 49 bytes removed while its linked
14-byte file remained intact. Link entries still count, and requested-root
resolution, protection identity, and filesystem deletion remain unchanged.

The POSIX regression covers top-level and nested file/directory links, a dangling
link, protected-link removal in two passes, preserved outside contents, and a
linked requested root. The old runner failed its byte-total assertion. Focused
cache/maintenance CTest and independent review passed. This keeps the existing
best-effort observation model rather than promising atomic filesystem accounting.

Desktop main/all-target builds passed. The first full run passed 390/391 tests;
`image_view_fade_tests` missed its 10-second thumbnail-load deadline while the
load log recorded 14.6 seconds. That unchanged target passed in isolation, and
the full 391-test recheck passed (103.65 seconds). `git diff --check` passed.

The image-view runner subsequently split thumbnail readiness from dimension
validation, so a load deadline reports its actual phase instead of a cache-key
collision. Its focused rebuild/test and independent review passed; timing and
cache behavior are unchanged.

## Follow-up: Release IR service admission after startup failure

IR submission startup now clears its started flag and pauses its profile when
preparation or worker construction throws. The exception still propagates, and
retry reloads profile state. Previously, the early started flag caused every
retry to return without launching a worker, leaving pending attempts dormant.

The direct regression injects caller allocation failure during preparation and
uses the existing wake hook to fail actual thread construction after preparation.
Both old cases preserved the queued attempt but failed retry, delivery, and
single-call checks. The fixed cases preserve that attempt and deliver it once
after same-instance retry. Focused CTest passed.

Independent review, desktop main/all-target builds, and all 391 tests passed
(109.88 seconds). `git diff --check` passed. The recovery restores admission;
it does not roll back completed repository maintenance or partial preparation.

## Follow-up: Commit library admission with its worker and metadata

Library admission now serializes worker preparation and state publication with
shutdown, always acquiring lifecycle before state. Task metadata is prepared
before queue insertion. A provisional new row is removed if queue/token insertion
fails; this preserves vector growth behavior. Reserved rows move into place only
after queue insertion, and Android tokens survive failed queue/error publication.
Android completion revalidates the reservation ID alongside token/type after
acquiring the lifecycle lock. Invalid/error-only calls retain their state-only
paths, and a trimmed task still consumes its token without starting a worker.

The old allocation runner reported 47 failures across new/reserved admission,
Android copy begin/finish, and copy-error publication. The fixed regressions
keep workers paused during caller allocation injection, check unchanged visible
admission state, and verify one matching request after retry and shutdown. Further
cases cover rejected calls from a worker during shutdown and trimmed task rows.
Focused CTest and independent review passed. An idle worker may remain prepared
after admission fails; no queued work or reservation mutation is published.

Desktop main/all-target builds and all 391 tests passed (109.90 seconds).
`git diff --check` passed.

## Follow-up: Centralize Jukebox lifecycle state bindings

Jukebox now builds its borrowed `SessionState` through one private helper instead
of ten repeated aggregate initializers. Every call remains at its original
construction point, including the scheduler lambda and already-locked stop
paths. The helper binds references without retaining state, locking, or allocating.

Independent review verified all ten bindings and unchanged lifetimes. Existing
lifecycle, restore, scheduler, and BGA tests cover the surrounding behavior.

Desktop main/all-target builds and all 391 tests passed (119.73 seconds).
`git diff --check` passed.

## Follow-up: Reuse shared profile and picker scope cleanup

Profile settings' three mutation barriers and the library/sound-folder pickers'
four active resets now use the existing `ScopeExit` utility. Five duplicate local
guard implementations were removed without moving callback execution or changing
atomic ordering. The picker fixture includes the production guard directly.

Desktop and focused fixture builds, independent review, and all eight relevant
profile/picker tests passed (1.94 seconds). Both mobile branches are compiled
against controlled native effects. `git diff --check` passed. The preceding
Jukebox commit supplies the full 391-test baseline; this structural cleanup was
verified with its affected workflows.

## Follow-up: Share gameplay and result timing statistics

Result Scene delegates to the existing Beatoraja score-metrics function used by
gameplay. Its duplicate result type and calculation were removed after a
normalized-body comparison confirmed equivalence. The same inputs and course
suppression remain, and the sign-convention comment moved to the shared owner.

The existing scene runner now links the production calculation. Two assertions
about formula source text were replaced by numeric checks for histogram sign,
truncation and filtering, judge penalties, and classic/charge long-note result
counting. Those cases passed before and after consolidation. Independent review,
focused CTest, and desktop main/all-target builds passed.

All 391 tests passed (104.69 seconds). `git diff --check` passed.

## Follow-up: Share Music Select mode conversion and filtering

Four Music Select components now use one chart-mode conversion, and both eager
filtering paths share the matching predicate. Normalized-body comparison and
independent review confirmed unchanged conversions, wildcard behavior, and call
order. Clear-lamp mappings with distinct fallback rules remain separate.

The desktop and focused builds and all four affected CTest runners passed
(2.36 seconds), including the eager/index matrix for every mode, difficulty, and
sort. `git diff --check` passed. The preceding timing commit supplies the full
391-test baseline.

## Follow-up: Share song clear-lamp conversion across selection and queries

The selector repository and three Music Select projections/indexes use one
song-lamp conversion. All four removed bodies matched, preserving the no-play
sentinel and threshold buckets. The ranking and Result Skin adapters retain
their intentionally different fallback rules.

Independent review, desktop/focused builds, and all four affected CTest runners
passed (7.07 seconds), covering projections, indexing, and repository queries.
`git diff --check` passed. The shared-timing commit supplies the latest full
391-test baseline; the subsequent mode/lamp refactors used affected workflows.

## Follow-up: Share export and audio-cache filename sanitization

Three filename helpers delegate their common character handling to one sanitizer
while retaining local fallback names and length limits. Normalized-body comparison
and independent review confirmed the same unsigned-byte rules, punctuation
replacement, trim/truncate order, and owning fallback strings. Call sites and
path construction are unchanged.

Desktop/focused builds and all four affected workflow tests passed (8.19 seconds).
The all-target build passed. Exporter render-access helpers keep their distinct
restoration behavior.

The full run passed 390/391 tests (107.10 seconds). The unchanged Jukebox runner
exceeded its 150 ms paused-stop deadline by waiting roughly its 250 ms scheduler
idle interval; that runner does not link these filename implementations. It
passed alone (1.45 seconds). Scheduler wake synchronization is being investigated
as the next change. `git diff --check` passed.

## Follow-up: Preserve scheduler wakeups across state and deadline work

Jukebox now records notifications with a mutex-protected generation captured
before each scheduling iteration. Both wait paths detect notifications received
before sleeping, and their atomic readiness predicates also handle a stop that
preceded capture. Existing idle and responsiveness limits remain unchanged.

Independent review, new notification-boundary tests, desktop main/all-target
builds, and all 392 parallel tests passed (94.70 seconds). An earlier unrelated
archive assertion passed on isolated and full rechecks; separate failure-only
diagnostics improve its next failure report. Observed transient disk space fell
below the unzip reserve, but the earlier assertion did not expose its cause.
`git diff --check` passed.

## Follow-up: Share stable ID and cache-name byte conversion

Six FNV-1a implementations and six hexadecimal formatters now use one shared
owner. Caller-specific key construction and the chart-music cache's delimited
append remain unchanged. Compiled comparison against every removed implementation
matched 1,025 binary inputs and hexadecimal boundaries; permanent golden vectors
also passed.

Independent review, desktop/focused builds, and all three affected hash/audio
workflow runners passed (1.69 seconds). The preceding scheduler change provides
the full 392-test baseline. Free disk space fell to about 200 MiB, so this change
used focused tests. Android/Windows native builds were not performed; their pure
helper bodies were included in compatibility testing. `git diff --check` passed.

## Follow-up: Prepare the scheduler before committing audio startup

A real-method allocation probe and debugger trace reproduced a thread-construction
failure after audio had started, leaving playback active. Jukebox now prepares
that thread first and releases its startup gate on every scope exit. The worker
waits for committed state or exits after a failed startup. Its scheduling body is
unchanged apart from moving to a private entry method.

Both allocation probes, retry/restore cases, and all six related audio/BGA/visual
and feature-off runners passed (4.84 seconds). Desktop/focused builds, independent
review, body comparison, and `git diff --check` passed. With about 166 MiB free,
verification used affected workflows; the notification fix provides the full
392-test baseline.

## Follow-up: Commit chart-analysis state after worker admission

Music Select now publishes its selected-chart analysis mailbox and started flag
after the detached worker is admitted. A rejected start preserves retryable state.
The extracted real-method regression failed before the change and now passes,
including exactly one retried worker and publication of its generated graph.

Desktop/fixture builds, all graph-selector cases, independent review, and
`git diff --check` passed. Existing debounce, cancellation, generation checks, and
exception propagation are unchanged. Validation used the affected workflow, with
the preceding notification fix supplying the full 392-test baseline.

## Follow-up: Align Android fallback stop requests and cleanup

The pinned NDK uses the local thread fallback. It now reports only the first
successful stop request and requests stop during destruction/replacement only
for joinable threads. Shared native/fallback tests reproduced both previous
mismatches and now pass, including concurrent requests and joined/detached/live
cleanup (0.69 seconds).

Independent review, both small host builds, pinned-NDK arm64/API 23 cross-compile,
and `git diff --check` passed. Android execution and a full application rebuild
were not performed; disk space was about 149 MiB. The tested subset does not
establish full fallback conformance.
