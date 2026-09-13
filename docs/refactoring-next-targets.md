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

## 4. Settings library-job ownership — completed

`SettingsLibraryTask` owns the exclusive table/folder worker, running state,
and typed table status, folder status, import progress, and accumulated reload
requests. Its publisher is borrowed only during synchronous work. The five
scene operations capture the externally owned repository and request values;
workers no longer capture the scene. The scene consumes updates outside the
owner mutex and retains colors, modal presentation, and URL completion.

Admission remains exclusive until work returns and joins previous completed
work before reuse. Completed updates survive a new admission until consumed,
matching the original handoff; stop joins uncancellable operations and clears
queued/late updates. Normal cleanup and direct destruction explicitly stop the
owner before scene members die. Its own destructor provides a final join.
Scanner cancellation, iOS folder-access lifetime, the delegated background
rebuild path, and table/folder repository operations are preserved.

Direct owner tests cover exclusive admission, coalesced channels, accumulated
reload, completion retention, stop/restart, and destruction. Compiled production
scene fixtures exercise import progress, success/failure and URL edits,
update/delete confirmation, absent views, and application-thread delivery.
The earlier destructor regression now uses the real owner.

## 5. Main Menu Find BMS job ownership — completed

`FindBmsTask` owns lookup/download/artifact worker lifetime, the service's
cancellation flag, bounded progress events, and result handoff. Scene worker
lambdas capture request values only. Nonblocking cancellation retains the
service outcome, including a pending artifact; shutdown/replacement joins
uncancellable artifact work and discards queued data. The latest 160 progress
events remain ordered. Selection generation and indexing stay in Main Menu.

A result awaiting application keeps the task busy and prevents new admission.
This closes a fast-completion race discovered during review: refreshing after
launch could otherwise re-enable old Keep/Delete actions before consuming the
completed transaction. A deterministic test observes publication through
worker-capture destruction, then checks real dialog policy, the complete
production artifact-launch method, and exactly-once indexing. Direct owner and
compiled scene tests also cover request forwarding, cancellation, progress,
candidate bounds, artifact decisions, and destructor joining.

## 6. IR upload preparation ownership — completed

`ir_uploads::PreparationTask` groups worker lifetime, `DurableEnqueueGate`, and
progress/completion data. It reuses `prepareSelectedCandidates`; the scene
captures the external application context for driver/submission dependencies
and retains selection, presentation, and controller updates. Consuming a
completion joins before returning it to the scene.

Stop requests gate cancellation before signaling the thread and joining. A
batch already inside durable enqueue keeps its outcome. Shutdown preserves
completion for consumption; initialization/cleanup reset discards explicitly.
Admission rejects owned work and unconsumed completion. Direct tests cover
partial failure, progress, cancellation on both sides of enqueue, retained
outcomes, restart, and destruction. Complete production scene methods run with
the real task, controller, and batch mapper against controlled effects.

## 7. Music Select direct-destruction lifecycle — completed

The derived destructor now calls the existing guarded `Scene::cleanup()` while
its callback dependencies remain alive. This joins launch/preload, Records,
skin preparation, and export work and unregisters input before member teardown.
It also covers the owning-pointer failure path when `SceneManager::changeScene`
catches a failed `init()`. Normal scene cleanup remains the same path; its guard
prevents repeated resource release during later destruction.

A compiled fixture executes the complete production destructor and cleanup,
selected cancellation helpers, and base cleanup/view-destruction methods.
It uses the real Records/export owners and controlled other resources to check
active direct destruction, exception unwinding, already-cleaned destruction,
never-initialized state, deferred capture disposal, and resource ordering. Lua
preparation and iOS scoped-access branches run against doubles; this is a local
lifecycle test, not physical-device validation.

## 8. Settings profile archive worker ownership — completed

`ProfileArchiveWorker` owns execution of the controller's existing one-shot
`ProfileArchiveTask`, its typed completion, and joining. The scene's supplied
post-execution callback retains temporary-import cleanup and warning policy.
The worker waits through both operation and callback on stop and discards the
completion. Taking a result joins before returning it; owned work, including
unconsumed completion, prevents a second admission.

Settings retains generation/controller decisions, picker state, launch-failure
recovery, and export staging ownership. Direct tests cover admission, exception
mapping, completion/capture lifetime, stop, restart, and destruction. Compiled
production launch/apply/stop methods use the real controller and owner with
controlled document effects to verify cleanup success/failure, both launch
exception paths, picker rejection, controller pipeline release, and native
export source retention. The existing controller test target now groups these
checks in `cmake/ProfileSettingsTests.cmake`.

## 9. Chart Viewer direct-destruction audio lifetime — completed

The viewer's explicit destructor now calls the existing guarded cleanup before
members disappear. Active, loaded, or retained listening state stops the shared
jukebox before chart release. An unused/inactive viewer does not stop audio;
normal cleanup followed by destruction does not repeat the stop.

A compiled fixture runs the complete production destructor/cleanup and actual
base cleanup/view disposal across every listening-flag combination, including
direct destruction, normal cleanup, exception unwinding, and unused state.
It checks exactly-once audio/chart/view ordering and deferred capture disposal.
The former default-destructor negative control fails the expected lifetime
assertion. Geometry and lifecycle targets are grouped in
`cmake/ChartViewerTests.cmake` with existing geometry registration preserved.

## 10. Music Player direct-destruction video lifetime

Music Player's fullscreen video owns jukebox visuals and temporary BGA policy
overrides. Its implicit destructor skips their explicit scene cleanup. Unlike
Chart Viewer, normal Music Player cleanup also resets global BGA state even
without acquired video resources. Direct destruction must therefore enter that
cleanup only when fullscreen, loaded-video, or visual-restoration state belongs
to this scene. An unused/already-exited scene must leave another playback
owner's state alone.

Characterize acquisition, active and partially acquired overrides, prior
normal cleanup, fullscreen exit, and unused destruction. Reuse the existing
cleanup path and preserve the previous visuals setting; avoid introducing
another copy of the release sequence or invoking UI refresh during destruction.

## What the review does not justify

The skin document loader, resource upload plans, and session activation graph
already provide meaningful decoding/rendering boundaries. No replacement
architecture is proposed. Likewise, file length alone does not justify moving
remaining archive adapters or gameplay methods into arbitrary files. Prefer
the ownership and state-transition improvements above, with subsystem-local
CMake changes and the existing full-suite baseline.
