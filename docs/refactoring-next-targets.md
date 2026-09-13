# Next workflow refactoring targets

These are structural findings from implementation review after the initial
roadmap's ownership slices. Completed follow-ups are marked below; remaining
candidates describe ownership improvements rather than confirmed correctness
defects. Preserve product behavior and choose one workflow at a time.

## 1. Settings archive-cache maintenance jobs — completed

`SettingsCacheMaintenance` now owns both jobs, admission, generation handling,
typed completion publication, and shutdown. `SettingsScene` supplies the real
archive-cache operations and keeps message/color/layout presentation. Its
cleanup and destructor explicitly join the controller; fallback controller
destruction also joins before its callback dependencies or mailbox disappear.

The existing asymmetry is preserved: cleanup resolves the jukebox's active
materialized paths on the worker and may finish its filesystem operation after
stop is requested; measurement accepts a stop token. Measurement cannot start
while cleanup is running, but cleanup may overlap an existing measurement.
Shutdown stops/joins cleanup before stopping/joining measurement.

Generation admission and result publication now share a mutex, and a new
request discards an older queued result. This closes the previous gap between
the generation check and status publication and prevents stale status from
replacing new progress. Tests cover both cases, real protected cleanup and
measurement in private directories, failures, repeated requests, cancellation,
restart, and destruction with work in flight. A compiled scene fixture checks
the complete production UI methods with the real controller and cache.

Pacemaker best-replay loading and archive index-build coordination are also
completed below. Settings library-job ownership is the next recommended slice.

## 2. Pacemaker best-replay loading — completed

`GamePlayScene` now reuses `ReplayRecordTask` to own its best-replay worker,
cancellation, completion, and joining. `src/scene/play/BestReplayLoad` supplies
the small asynchronous loading function; `BestReplayResolver` retains exact
attempt resolution and the shared replay consumer boundary. No second worker
class was needed.

The loading function explicitly cancels/joins before replacement because the
shared task's `start` alone joins without cancelling. Resolver construction
remains on the worker, including runtime profile-root resolution. Successful
loads queue a scene callback; consuming it joins the worker before updating
the personal-best ghost on the application thread. Missing/unreadable results
consume the shared task's no-op completion and retain the fallback target.
The selected pacemaker target keeps its proportional score behavior.

Tests exercise exact attempt/path forwarding, blocked replacement, late results
after cancellation, missing/mismatched/unreadable replays, callback/resource
release on destruction, and application-thread handoff. A compiled fixture
executes complete production scene methods with the real task, resolver, and
pacemaker policy, including absent chart/best state and chart replacement after
stop. Existing resolver and shared-task tests remain intact.

## 3. Archive index-build coordination — completed

`archive_file::IndexBuildCoordinator` replaces four parallel state maps and
`IndexBuildScope` with one active record per key and a movable builder lease.
Abandoning the builder completes its flight with failure and wakes waiters.
Each waiter retains the flight it joined, so a later retry cannot overwrite
that waiter's outcome. Cancellation and checkpoint exceptions affect only
the waiting caller; checkpoints run outside the coordinator mutex.

`ArchiveFile` retains source identity validation, memory/disk cache lookup,
backend selection, live-manifest promotion, and retry policy. It rechecks the
cache after admission and after waiting, including independently published
usable data following a failed flight. Completed flights leave the admission
map immediately; the existing index-cache retention policy is unchanged.

Eight direct tests cover per-key admission, old-flight failure across a newer
success, waiter cancellation, unlocked/reentrant checkpoints, exception
completion, retry competition, and moved builder ownership. Existing archive
concurrency regressions retain backend-level cancellation and promotion checks.

## 4. Settings library-job ownership

`SettingsSceneTables.cpp` runs table import/update/delete, fallback library
rebuild, and folder removal through one scene-owned thread. Worker lambdas
capture the scene and publish through a mutex plus parallel pending flags,
strings, colors, and import progress fields. Both normal cleanup and direct destruction now explicitly stop/join this
thread before its callback dependencies are destroyed. A compiled production
destructor fixture reproduced the previous gap and checks stop signaling,
joining an operation that finishes after cancellation, and idle/prior cleanup.

Then group admission, worker lifetime, and data publication in one owner while
keeping repository/import/scanner operations and application-thread UI
presentation distinct. Preserve exclusive job admission, uncancellable table
operations with suppressed late results, scanner stop tokens, import URL
completion, reload requests, and the existing delegated background rebuild
path. Keep iOS security-scoped folder handles alive throughout fallback scans.
The global chart-library task service has different queue/history semantics;
do not route these jobs through it solely to reuse a worker.

## What the review does not justify

The skin document loader, resource upload plans, and session activation graph
already provide meaningful decoding/rendering boundaries. No replacement
architecture is proposed. Likewise, file length alone does not justify moving
remaining archive adapters or gameplay methods into arbitrary files. Prefer
the ownership and state-transition improvements above, with subsystem-local
CMake changes and the existing full-suite baseline.
