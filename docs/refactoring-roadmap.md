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
Start with Settings cache-maintenance jobs, then evaluate pacemaker best-replay
loading and archive index-build coordination as separate slices.
