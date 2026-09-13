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

Pacemaker best-replay loading is also completed below. The next recommended
slice is archive index-build coordination.

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

## 3. Archive index-build coordination

`ArchiveFile.cpp` has four parallel maps for active/done/failed/waiter state,
their shared mutex/condition variable, and `IndexBuildScope` for exception
completion. `cachedIndexForArchive` interleaves that protocol with source
identity validation, memory/disk cache lookup, and backend selection. A reader
must track several booleans and lock transitions to understand whether a
caller builds, waits, cancels, consumes failure, or retries a mismatched result.

Extract an index-build coordinator with one per-key state record and an owned
builder completion guard. Keep cached index data, persistent storage, source
identity checks, and backend selection outside that coordinator. Preserve
cancelled-waiter isolation, one shared failure for current waiters, later
request retries, live-manifest promotion, and the recheck before claiming a
new build. The current intentionally unbounded index cache is a separate
product/performance policy and should not change as part of this refactor.

Retain the archive concurrency regressions for waiter cancellation and live
manifest promotion. Add compiled coordinator tests with blocked builders,
multiple waiters, builder exceptions, cancelled waiters, and mismatched-result
retry before removing the old maps and guard.

## What the review does not justify

The skin document loader, resource upload plans, and session activation graph
already provide meaningful decoding/rendering boundaries. No replacement
architecture is proposed. Likewise, file length alone does not justify moving
remaining archive adapters or gameplay methods into arbitrary files. Prefer
the ownership and state-transition improvements above, with subsystem-local
CMake changes and the existing full-suite baseline.
