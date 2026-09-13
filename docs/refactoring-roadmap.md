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
