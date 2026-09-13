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
4. **Archive handling — first ownership slice completed.** `TemporaryCache`
   owns materialized-media writes, protected cleanup, measurement, and mutation
   serialization. The facade retains backend/source identity and cache naming.
   Backend mechanics versus extraction policy remain a subsequent slice;
   preserve cancellation, resource budgets, staging/output ownership, and
   recovery when choosing that boundary.
5. **Gameplay and skin boundaries — initial review completed.** Existing
   simulation, worker, document-loader, and resource-plan interfaces already
   separate substantial responsibilities. The next candidate is gameplay input
   session lifetime, whose platform detach/drain/join sequence still lives in
   the scene. See the evidence and constraints below before implementing it.

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
  `tests/gameplay_terminal_scene_extract.py`, the worker-stop fixture takes
  only the tail beginning at `session.worker->stop()`. Its passing replay and
  terminal tests therefore do not establish platform detachment or the earlier
  drain/cancellation sequence. A new compiled lifecycle boundary should cover
  failed startup, repeated stop, callbacks in flight, touch-cancellation
  failure invalidating replay transfer, and destruction. Keep the existing
  scene fixture until equivalent terminal/navigation coverage is retained.
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
