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

## 10. Music Player direct-destruction video lifetime — completed

The destructor enters existing guarded cleanup only while fullscreen, loaded
video, or visual-restoration state belongs to the scene. That covers partial
acquisition while protecting shared BGA state after fullscreen exit or normal
cleanup, and when an unused scene is destroyed. Release restores the previous
visuals setting and unloads owned visuals without stopping native music or
refreshing UI during destruction.

The generated fixture executes complete production acquisition, fullscreen
exit, cleanup, and destructor methods with base cleanup/view disposal. It
covers prior visuals enabled/disabled, BGA and artwork fallback, partial
acquisition exceptions on both sides of entering fullscreen, deferred capture
release, and inactive/exited/cleaned ownership. Negative controls fail for both
the former implicit destructor and an unconditional cleanup destructor. Native
media and UI effects are controlled doubles.

## 11. Shared parallel-work helpers — completed

`src/utils/ParallelWork.h` now groups worker sizing and indexed execution;
`Utils.h` retains the existing include facade. Unused `parallel_for` and
`threadRAII` declarations/definitions were removed after a repository-wide
caller search. Indexed work retains dynamic assignment and borrowed callable
semantics, with joining thread owners whose lifetime ends before the index and
callable references on normal return or partial launch unwinding.

The sizing policy preserves hardware fallback/headroom and compares the wider
item count before narrowing the bounded result. Standalone tests cover policy
boundaries, counts above `unsigned int`, sequential execution, move-only work,
exactly-once indices, and waiting for completion. A negative control using the
old narrowing fails the large-count assertion. Callback exception policy is
unchanged; partial launch safety follows the joining ownership structure.

## 12. Music Player sleep-timer shutdown synchronization — completed

Shutdown now requests stop while holding `sleepTimerMutex`, serializing the
predicate change with entry into the worker's wait. Final notification and
joining remain outside both mutexes. This closes a lost-wake gap that could
leave destruction waiting indefinitely for an idle timer.

The compiled production-method fixture holds the worker between a false
predicate and wait registration. The old implementation fails its early-stop
notification assertion; the fixed implementation joins successfully. Further
cases cover replacement, clear, restart, expiry status, and joining a blocked
expiry callback while keeping the timer mutex available. No additional timer
abstraction was needed to fix the synchronization boundary.

## 13. Local archive and scanner thread-batch ownership — completed

The direct ZIP, random-access RAR, parallel RAR5, and individual-chart parsing
batches now store joining thread owners. Existing launch loops, work assignment,
cancellation checkpoints, memory budgets, and explicit joins remain intact.
If a later thread launch throws, already-started threads finish before the
borrowed local state is destroyed, instead of terminating during unwinding.

All four vectors follow their captured state in declaration order. Their
workers accept no stop token and need no full-pool barrier, so automatic joining
does not add a new cancellation protocol or require every launch to succeed.
Exceptions escaping worker callbacks retain their existing behavior.

## 14. Persistent worker-pool construction rollback — completed

`chart_scan::WorkScheduler` and `ImageDecodeCoordinator` now retire already
started workers if a later reserve/launch operation throws. Each constructor
calls its existing cancel/shutdown method from a body-local catch, then
rethrows. All members still exist during rollback, and the idle workers receive
their normal stop predicate and notification before joining.

A direct regression links both real implementations and sweeps the constructing
thread's allocation points, disabling fault injection before cleanup. Both old
implementations terminated while unwinding; both fixed implementations propagate
all eleven injected failures observed on this runtime and permit subsequent
construction/destruction. Normal pool tests and the two bounded failure modes
are grouped in `cmake/WorkerPoolTests.cmake`. No production test hook was added.

## 15. Intro scene input-subscription lifetime — completed

The explicit Intro destructor calls guarded cleanup while derived state remains
alive. Existing stop logic removes both registry subscriptions before resetting
the adapter; base cleanup then releases views and deferred captures. This also
handles initialization unwinding after only the input subscription succeeds.
Unused and already-cleaned scenes remain safe, with no repeated teardown.

The compiled fixture executes full production registration, stop, cleanup, and
destruction against the real registry and a controlled backend. Observed adapter
and view effects check exact teardown ordering, queued-event cancellation, and
retention of an unrelated listener. The former implicit destructor fails the
live-subscription assertion. Navigation and lifecycle targets are grouped in
`cmake/IntroSceneTests.cmake` under the original feature gate.

## 16. Input capture construction rollback — completed

`InputCaptureController` now removes a successfully acquired input listener if
device registration throws during construction. Its destructor cannot run in
that case, so rollback occurs before resolver/member destruction and preserves
the original exception. Ordinary startup, callback delivery, and teardown are
unchanged; the other post-construction subscription callers need no shared
replacement abstraction.

The regression links the real controller, resolver, and profile/configuration
implementations against a registry boundary that fails either registration.
It verifies no callback is retained, unrelated listeners survive, saved callback
captures are released, and a normal input reaches the actual resolver. The old
constructor fails the retained-callback assertion without invoking a dangling
reference. Normal and startup-failure tests share their controller sources in
`cmake/InputCaptureTests.cmake`.

## 17. Obsolete application thread registry — completed

The unused `ApplicationContext::threads` list, its empty shutdown join/log loop,
and the corresponding thread include were removed. Repository-wide searches
and independent review found no registrations or consumers. Actual background
workers remain owned and stopped by their subsystem services; their existing
shutdown order is unchanged.

## 18. Library picker admission and startup rollback — completed

All four mobile picker launch paths now release their active admission if
joining or creating a worker throws, then propagate the original exception.
The sound-set picker claims active ownership before checking result readiness,
so a completion published during admission cannot be replaced by another pick.
Native routing, bookmarks, stop checks, and direct/import behavior remain intact.

Generated fixtures compile complete picker methods in both iOS and Android
branches with controlled native/database effects. Real thread allocation fails
once on the requesting thread to verify rollback and retry. An observed atomic
exchange pauses admission while the previous worker publishes through real
atomics, verifying first-result retention, one-time consumption, and reopening
admission. All six old-code modes failed their intended assertions; the fixed
modes pass. These are local branch tests, not native dialog/device execution.

## 19. Play-skin snapshot fixture cleanup — completed

The session runner's temporary-directory owner now restores permissions before
removing immutable snapshots and reports cleanup failures. Traversal inspects
entries without following symlinks; Windows regular files also regain write
permission. A same-runner regression verifies nested read-only removal and,
on POSIX, preservation of an external symlink target's contents and permissions.
The old destructor failed the removal assertion. Production snapshot policy
and other test fixtures are unchanged.

## 20. Replay task launch rollback — completed

`ReplayRecordTask::start` reuses `cancelAndWait` if thread construction throws,
then propagates the exception. This releases active ownership and rejects late
completion publication. Joining prior work and allocating a replacement token
retain their existing order and failure behavior.

The direct test runner fails one real startup allocation only after the task
becomes active. The old implementation failed both idle-state and late-publish
assertions. The regression also checks capture release, no execution of failed
work, a fresh cancellation token on retry, and exactly-once completion.

## 21. Preload worker launch rollback — completed

`ChartPreloadWorker::request` cancels its pending request before propagating a
worker-launch exception. Previously, the queued chart survived without a worker
and an identical retry returned through deduplication without launching work.
Successful admission, debounce, and idle callbacks retain their existing order.

The direct runner measures a warmed request's caller-thread allocations and
fails the final startup allocation. The old implementation failed queued-state,
identical-retry, and processing-count assertions. The fixed regression verifies
no failed-work execution, pending-state release, and exactly-once processing
after the same chart is requested again.

## 22. Obsolete music-select catalog rebuild — completed

Removed the unused `MusicSelectSkinSession::refreshResources` method, whose
always-false guard made its full catalog rebuild unreachable. Its duplicate
device/counter owners and unused preparation stop token were removed from the
session and private constructor. The resource and movie catalogs retain their
own lifetime dependencies. The compatibility query `requiresResourceRefresh`
and active image/font patch paths remain unchanged.

## 23. Replay export startup failure delivery — completed

Worker-construction exceptions now use `ReplayExportJob`'s result channel,
matching export/preparation failures. A shared publication method retains
admission until `takeResult`, including when no worker exists. This lets both
scene consumers finish the already-started UI handoff and report the error.

One-shot allocation failure exercises real thread construction in the direct
job runner and both existing scene fixtures. The old job and scenes failed
their result-delivery assertions. Regressions cover released captures, no work,
retained admission, one-time diagnostic delivery, retry, Main Menu preview
restoration, and Records recovery with or without a modal. The allocation hook
is shared by those test executables and is never linked into the application.

## 24. Shared session/benchmark snapshot cleanup — completed

The loading benchmark also left immutable snapshots behind: its isolated test
passed while creating two leftover directories. `ReadOnlyTreeCleanup` now
shares the session fixture's permission restoration and non-following traversal
with that benchmark. Removal is attempted after permission errors, and callers
report the removal error or the earlier traversal/permission error.

Benchmark work now runs inside an owning helper so all fixture destructors
finish before `main` checks failures or announces success. The existing session
regression continues to cover nested read-only trees and external symlinks.
Both focused runners left zero new fixture directories, and default/cold/warm,
invalid-argument, and missing-input CLI checks preserved their output/status.

## 25. Skin lifecycle/settings/commit fixture cleanup — completed

Four passing runners still left 51 immutable fixture roots behind per isolated
group run: lifecycle (30), settings (13), commit coordination (7), and package
operations (1). Their temporary owners now use `ReadOnlyTreeCleanup` and their
existing failure-reporting mechanisms. Resource destruction and death-test
parent ownership remain intact; cleanup finishes before final test reporting.
The corrected focused group passed with zero new directories for all four
prefixes. Global and skin-specific test helper namespaces are explicit where
both occur in one runner.

## 26. Consolidate remaining snapshot cleanup loops — completed

The archive importer, tree snapshotter, package store, and Lua filesystem
fixtures now use `ReadOnlyTreeCleanup` instead of four duplicate permission
loops. Cleanup avoids changing permissions through symlinks, restores directory
access before traversal, and reports failures through the existing test runners.
Constructors and resource teardown order are unchanged. All four focused tests
passed with zero new fixture roots; independent review found no issues.

## 27. Claim snapshot test roots exclusively — completed

The tree snapshotter fixture now retries `create_directory` until it owns a
new root. Previously, each process started at the same serial path and
`create_directories` silently accepted an existing directory, which teardown
then deleted. An isolated executable check demonstrated deletion of a
pre-existing sentinel despite a passing runner. The fixed runner preserves that
sentinel; three pairs of concurrent runs also passed without leftover roots.
The rebuilt focused CTest passed, and independent review found no issues.

## 28. Gameplay worker launch rollback — completed

`RealtimeGameplayWorker::start` restores stopped state before propagating a
thread-construction exception. Previously, the admission flag remained set,
`running()` returned true without a thread, and an immediate retry was rejected.
The failure remains an exception rather than a gameplay simulation fault.

The existing runner now uses the shared test-only allocation hook to fail real
thread startup after constructing all fixtures. The old implementation failed
the stopped-state assertion. The regression also verifies no audio execution,
immediate retry, duplicate-start rejection, exactly one audio commit from real
gameplay input, and clean shutdown. Focused tests and independent review passed.

## 29. Image probe path failure containment — completed

The `noexcept` BMS image-availability probe now constructs its resource path
inside the existing startup exception handler. Previously, allocation failure
in `parent_path` or path joining could terminate the process before that handler.
Failure now uses the established completed/unavailable result.

The decoder runner preconstructs every argument, then fails the next body
allocation through the shared test-only hook. The old implementation aborted
with uncaught `bad_alloc`; the fixed regression checks completion without
decoding and a successful retry. Focused CTest and independent review passed.

## 30. Bound the Unix socket fixture path — completed

The snapshotter runner checks the socket path length before copying into
`sockaddr_un::sun_path`, reserving space for its terminating zero. An earlier
isolated run under a long temporary parent exposed the unchecked copy. An
unsupported path now reports a fixture failure before opening a descriptor or
writing beyond the buffer; it does not skip validation or truncate the path.

Normal focused CTest passed. An intentionally long private temporary parent
produced exactly the expected diagnostic and exit status 1, with no leftover
fixture roots. Independent review found no issues.

## 31. Prepare folder-status ownership before admission — completed

The folder-status loader now prepares its shared processor and worker before
committing deduplication state or consuming failed rows. Previously, allocation
failure could reject an identical fresh retry or clear delayed-retry readiness.
The duplicate-request fast path remains unchanged. Prepared callback ownership
outlives the request lock so startup failure destroys captures after unlocking.

The shared test allocation hook can now fail after a chosen number of ordinary
caller-thread allocations; existing next-allocation users retain their behavior.
The runner walks actual allocation points until successful admission, checking
capture release, no failed work/results, identical retry, and one result/call.
Its delayed-retry case also checks retained readiness and row delivery. Old-code
runs failed both admission and retry-readiness assertions; focused tests,
independent review, desktop builds, and all 391 tests passed.

## 32. Prepare directory workers before request ownership — completed

The directory loader creates its worker before committing the pending request,
generation, or result changes. Previously, failed thread startup retained the
rejected callback. The worker still waits on the request lock until admission
finishes; exception unwinding releases that lock before callback captures.

A regression walks actual caller allocation failures through successful
admission, checking capture release, no failed work/results, and an immediate
retry that delivers one matching identity/generation and one callback call.
The old implementation failed the capture-release assertion. Focused CTest,
independent review, desktop builds, and all 391 tests passed.

## 33. Claim catalog test directories exclusively — completed

The resource-catalog and movie-catalog runners now retry `create_directory`
until they own a fresh root. Their deterministic serial names previously used
`create_directories`, which reused existing directories and later deleted them.
Both old binaries passed while deleting a preseeded sentinel in a private
temporary parent. The fixed binaries preserve it, pass focused CTest and paired
concurrent runs, and leave no owned roots. Independent review passed.

An isolated audit of 19 other skin fixture runners found no cleanup leftovers;
that evidence did not justify extending read-only cleanup to those fixtures.

## 34. Deliver cache-operation exceptions as failures — completed

`SettingsCacheMaintenance` now catches operation exceptions and publishes the
existing failed completion. Previously, an exception from cleanup or measurement
escaped its worker and terminated the process. Named errors retain their message;
empty or unknown exceptions receive a fallback. The same running-state reset,
generation filtering, and stop suppression apply to all operation outcomes.

The old direct runner aborted on a thrown operation. New tests cover both jobs,
named/empty/unknown failures, same-operation recovery, and superseded/stopped
failure suppression. The compiled scene fixture checks failure presentation,
button reset, and recovery through complete production methods. Focused CTest,
independent review, desktop builds, and all 391 tests passed.

## 35. Exclude linked targets from cache byte totals — completed

Cache traversal now classifies entries without following symbolic links, and
cleanup's byte helper does not traverse a top-level link target. A private-root
probe with 7 owned bytes previously reported 35 bytes used and 49 bytes removed,
although the linked 14-byte file remained intact. Link entries still count;
root resolution, protected-entry identity, and removal behavior are unchanged.

The POSIX regression covers top-level/nested file and directory links, a dangling
link, protected-link cleanup, outside contents, and an explicitly linked cache
root. The old runner failed its byte-total assertion. Both focused cache runners
and independent review passed. Desktop builds and the full 391-test recheck
passed; the first run hit an unrelated artwork-load deadline. Measurement
remains a best-effort observation.

## 36. Release IR service admission after startup failure — completed

IR submission startup now restores its inactive admission state when profile
preparation or worker creation throws. Previously, the early `started` flag
remained set and every later `start` returned without creating a worker. Pending
attempts remain stored, and a retry reloads profile state before processing them.

The direct runner injects caller allocation failure before preparation and,
through its existing wake hook, immediately before real thread construction.
Both old-code cases preserved the pending attempt but failed all retry/delivery
checks. The fixed cases propagate the exception, preserve pending work, and
deliver once after same-instance retry. Focused CTest, independent review,
desktop builds, and all 391 tests passed. This restores admission without
rolling back repository maintenance or partial profile preparation.

## What the review does not justify

The skin document loader, resource upload plans, and session activation graph
already provide meaningful decoding/rendering boundaries. No replacement
architecture is proposed. Likewise, file length alone does not justify moving
remaining archive adapters or gameplay methods into arbitrary files. Prefer
the ownership and state-transition improvements above, with subsystem-local
CMake changes and the existing full-suite baseline.
