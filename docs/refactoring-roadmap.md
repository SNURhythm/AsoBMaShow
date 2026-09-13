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
4. **Archive handling — pending.** Separate backend mechanics from extraction
   policy and cache ownership. Preserve cancellation, resource budgets, active
   file protection, staging/output ownership, and recovery. Select one boundary
   from implementation evidence before editing; do not split files solely by
   size.
5. **Gameplay and skin boundaries — pending.** Inspect orchestration versus
   simulation, and decoding versus rendering. Choose individual extractions
   from evidence instead of prescribing a replacement architecture.

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
