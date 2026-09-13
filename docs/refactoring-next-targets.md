# Next workflow refactoring targets

These are structural findings from implementation review after the initial
roadmap's ownership slices. They are candidates for subsequent work, not
confirmed correctness defects. Preserve product behavior and choose one
workflow at a time.

## 1. Settings archive-cache maintenance jobs

`SettingsScene::cleanupTemporaryArchiveCache` and
`SettingsScene::measureTemporaryArchiveCache` each own a `jthread`, running
flag, generation handling, completion formatting, and publication through the
scene's status mutex. `SettingsScene::cleanup` separately requests stop and
joins both. The filesystem work now has an explicit `TemporaryCache` owner,
but the asynchronous UI workflow still lives across those scene methods and
members.

Extract a cache-maintenance controller that owns jobs and completion delivery.
Keep message/color/layout presentation in the scene. Preserve the existing
asymmetry: cleanup protects the jukebox's active materialized paths and may
finish its filesystem operation after stop is requested; measurement accepts
a stop token. Measurement is not started while cleanup is marked running, and
workers check the shared generation before publishing their status.

Characterize overlapping measurement/cleanup, stale completion, repeated
requests, active-path protection, failed operations, and destruction with work
in flight. Reuse the real cache interfaces and private temporary directories;
do not test only status strings or thread-field names. This is the recommended
next slice because it directly builds on the newly separated cache owner.

## 2. Pacemaker best-replay loading

`GamePlayScene::startBestReplayLoad`, `applyPendingBestReplay`, and
`stopBestReplayLoad` coordinate a thread, a shared atomic cancellation flag, a
mutex, and a pending replay. `configurePacemakerTarget`, scene reset/cleanup,
and destruction depend on that lifecycle. The scene captures itself in the
worker even though `BestReplayResolver` already owns the replay-resolution
operation.

Use a narrow owned task to return the loaded replay to the application thread.
Evaluate reuse of `ReplayRecordTask` against its actual admission/completion
semantics before adding another task abstraction. Keep pacemaker selection
and target application in the scene. In particular, the persisted best replay
updates the personal-best ghost; the selected pacemaker target deliberately
keeps its current proportional score behavior.

Characterize target replacement during a blocked load, cancellation and join,
late completion after chart replacement, missing/unreadable replay, and scene
destruction. Preserve `BestReplayResolver` tests and add direct task tests plus
a small scene handoff check.

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
