# Release blocker review — 2026-10-03

Base: `484a2d69` (`develop`, PR #115 merged).
Fix branch: `fix/release-blockers-2026-10-03`.

## Localization and PR review follow-up

Retained ranking and score-detail roots are refreshed against the current
language revision on open and while active, even if a root missed the scene's
language notification while closed. Native own-player labels now translate;
custom-skin Select, Gameplay, and Result projections continue to emit the exact
`YOU` marker expected by ModernChic and LITONE12.

Reviewed all eight open PR comments, including the older threads:

- Freeze gameplay IR state before account, provider, or foreground resets once
  ending begins. Tests now execute the real configure and update methods.
- Observe ranking-evidence revisions in gameplay so an older queued upload can
  refresh an idle request before capturing this play's previous rank.
- Handle selector ranking-evidence changes through ranking reload scheduling,
  preserving the current modal, preview, analysis, and selection. Unaffected
  requests reuse the service cache; modal ownership still defers skin loading.
- Preserve normalized result ranking-position writes until a positive population
  is known. The latest explicit write wins and suppresses automatic centering.
- Center results by the own row's index, including large competition-rank tie
  groups, while exposing the provider's rank unchanged.
- Accept equal-ranked nearby players while retaining identity, chart, population,
  size, and wrong-side validation. Preserve above/own/below order within ties.
- Confirm the earlier fixes retaining an observed rank after pagination failure
  and recentering after upload refresh remain present and tested.

Desktop build and 13 targeted suites passed: modal, ranking service, parser,
Tachi driver, gameplay IR scene, result IR scene/projection, skin state bridge,
selector ranking/lifecycle/export, and localization/catalog checks. This
follow-up did not run the full suite or a live authenticated/mobile UI smoke test.

## Native ranking tabs follow-up

The merged list still made scrolling above a distant nearby rank jump to top
rows. Tachi's chart endpoint accepts `startRanking` and returns up to 100 rows
in ascending rank order. Its nearby endpoint returns five rows strictly above
and below a player's rank. Neither exposes a backwards cursor or a tie-breaking
row cursor, so a rank-only previous-page calculation cannot guarantee adjacent
players across ties. These contracts were checked against the upstream
[chart route](https://github.com/zkldi/Tachi/blob/main/typescript/server/src/server/router/api/v1/games/router.ts)
and [ranking queries](https://github.com/zkldi/Tachi/blob/main/typescript/server/src/lib/db-formats/pb.ts).

Following the requested fallback, the native modal now has **Top rankings** and
**Near me** tabs. Top rankings contains only sequential top pages and retains
normal prefetch. Near me contains only the nearby window and never paginates.
A distant known own rank opens Near me initially; a chosen tab survives refresh,
and tab switches retain separate scroll offsets. When own rank is already in
the loaded top pages, Near me derives up to five neighboring rows on either side.
The tab is disabled when no own row is known. Labels and the nearby-window
explanation are localized in English, Korean, and Japanese. This supersedes the
merged-list viewport guard described below.

The desktop build and five targeted modal, service, result-scene, and
localization suites passed. Tests cover tab isolation, rank 5,000, own-row score
details, page completion while Near me is active, refresh and new-chart state,
known/unknown own ranks, and the bounded window derived from completed pages.
No live authenticated or mobile UI smoke test was performed for this follow-up.

## Native nearby-rank pagination follow-up

Opening native rankings at a distant own rank treated the supplementary nearby
rows as the end of the sequential leaderboard. Each fetched page preserved the
own-row viewport, triggering another page. The modal now retains the top-page
row count separately and prefetches only while the viewport stays within those
rows. Scrolling near the top-page boundary still fetches the next page; viewing
the nearby window does not. Result-skin background pagination also pauses while
the native modal owns the shared request.

Both regression tests failed before the guards were applied. The desktop app
build and four targeted modal, ranking-service, result-projection, and production
result-scene suites passed afterward. Coverage includes rank 5,000, tall
viewports, page insertion with retained scroll position, normal top-page
prefetch, and repeated result updates while the modal is open. This follow-up
was not tested in a live authenticated session or on a mobile device.

## Authored-skin IR follow-up

The subsequent iOS report identified a gap missed by the earlier bridge tests
and reviews: native rankings worked, but ModernChic and LITONE12 displayed zero
ranking values. Testing the supplied skins from `~/Downloads/Skins` through the
real Lua loader reproduced 46 failures before this correction.

`GameplaySkinBuiltinCatalog` admitted generated IR property names but omitted
their numeric selectors. Consequently, numeric `ref` bindings were dropped at
decode time and `main_state.number()` returned zero before reaching the live
ranking bridge. Register the exact numeric ranking and clear-statistic families
in their integer/image domains, preserving source skin-configuration precedence
for numeric image selectors 386–388 and leaving unrelated gaps unsupported.

Both supplied skins also identify the current player's row by comparing its
text with `YOU`. Selector and result projections now provide that label for
`currentUser` rows while preserving native provider entries and account names.
This follows the supplied skins' compatibility requirement.

ModernChic's rank-change callback is a separate path: it already reads selectors
179/182 and requires successful-upload timers 172/173. The authored regression
checks a change from rank 8 to rank 3 produces -5; the catalog fix alone does not
establish the cause of every reported zero rank change on a device.

The supplied ModernChic and LITONE12 Select/Result scripts now pass the authored
acceptance run with nonzero ranking fixtures:
`cmake-build-debug/play_skin_session_tests --authored-result-ir ~/Downloads/Skins`.
This checks retained decoded numeric references, real score/rank callbacks,
ModernChic's ranking graph, and its own-row/rank-change predicates. External skin
assets remain outside the repository; the regular suite includes a portable live
Lua numeric-selector regression and separate current-user projection tests.
Numeric SELECT image collisions failed three paired assertions before their
guard and pass afterward. Independent review of this follow-up found no further
actionable issues. The final full rebuild and parallel CTest run passed all
417 tests in 134.44 seconds. Device upload state and live iOS rendering remain
unverified.

## IR fetch latency follow-up

Music select imposed a five-second delay before starting an uncached ranking
request, then discarded received rows from the skin projection while later
pages loaded. The debounce is now 250 ms, removing 4.75 seconds of fixed waiting
while still coalescing fast song-wheel movement. Successful pages stay visible
during subsequent downloads or a continuation failure. Totals come from the
server; clear counts and percentages remain unavailable until the leaderboard
is complete. Partial snapshots do not enter the scene's ten-minute completed
cache, so returning to a song can resume the service's cached continuation.

The regression failed seven projection assertions before the fix, and the
production-method cache fixture caught partial data entering the completed
cache. Both pass after the change, along with the selector property tests.
Independent review checked both supplied skins' timer and unavailable-statistic
handling and found no blocking issue. Scrolling beyond downloaded rows still
waits for those pages. This change does not claim to reduce server latency or
to have measured live iOS network performance.

The first full run passed 416/417 tests; `lua_skin_coroutine_callback` rejected
its yield fixture with `skin_lua_wall_time_limit_exceeded` during parallel
execution. It passed immediately in isolation. Like the existing Lua host test,
this callback test now runs serially so sibling tests do not compete with its
production wall-clock budget. No Lua execution limit changed.
The final full build and parallel CTest run passed 417/417 tests in 301.22 seconds.

## Nearby-own-rank latency follow-up

The remaining result-skin delay came from sequential top-page pagination:
reaching rank 5,000 required about 50 ranking-page requests. Tachi provides a
public `leaderboard-adjacent` endpoint for a known authenticated numeric user ID.
Its current server implementation and a live public response were checked;
it returns the player's PB and up to five neighbors on either side, while its
profile list omits the player themself.

The initial fetch now adds one adjacent request when the player's row is absent
from an incomplete top page. This makes the nearby lookup independent of rank
distance (two ranking requests, plus uncached chart/identity preflight). Missing
PBs, unsupported endpoints, malformed responses and transient failures retain
the normal page fallback; the optional request has a five-second timeout.
Cancellation still cancels the complete fetch. Names are never fabricated from
missing profile data, and all skin projections retain the `YOU` marker.

Supplemental neighbors remain separate from the contiguous top-page prefix.
They cannot count toward completion or clear statistics. Result skins display
the nearby window immediately; music select receives own rank and available
nearby rows, gameplay captures the previous rank, and the native list includes
neighbors without duplicate players. Native pagination preserves its viewport
and selected player by identity. The result view follows an improved own rank
after the upload refresh unless the player has manually chosen a scroll position.

Regression coverage includes distant ranks 110/5,000 with constant request
counts, missing own profile, tied ranks, malformed/cross-chart/duplicate rows,
encoded chart IDs, fallback and cancellation, credential redaction, supplemental
row retirement, skin properties and the production result-update method while
page two is pending. Initial driver tests failed four assertions before the
fix; result/select projection checks failed before their integration. Independent
review identified and verified fixes for prefix-window overlap, post-upload
recentering and chart-ID encoding. No remaining actionable finding was reported.

The desktop app and all test targets built successfully, including the final
review corrections. The full parallel run completed in 249.44 seconds with
413 passes, three failures and one blocked dependent test. All nearby-ranking
regressions passed. The other failures were the coroutine yield fixture
(`skin_lua_wall_time_limit_exceeded`), numeric-text formatting (`<no text>` for
`-0.0`), and the sound-bundle decoder's 15-second CTest timeout. An immediate
isolated rerun of the failed tests and their dependencies passed all 24 checks
in 29.69 seconds without source changes; the decoder completed in 2.88 seconds.
The formatting failure did not record a detailed callback diagnostic, so its
precise cause is unconfirmed. No production or test deadline was relaxed for
this follow-up. This was not a clean first-pass full-suite result.

The live check verified the public endpoint's response shape, not authenticated
device latency.
Complete-only statistics and uncached arbitrary leaderboard positions still
require sequential pages. No mobile build or deployment was performed for this
follow-up.

## Confirmed runtime blockers

The follow-up program-code review found two release-blocking defects:

- A selected result skin's scene/fadeout timeout navigated synchronously from
  `ResultScene::renderScene()`. Scene replacement deleted the result scene,
  then `Scene::render()` continued iterating its freed overlay views. Ordinary
  chart results and course results could hit this use-after-free simply by
  waiting for the skin's timeout. Navigation now uses the scene's deferred
  queue, after rendering has unwound. A regression executes the complete
  production render/scene-management methods: it aborts on the old code and
  passes after the fix, including ordinary/course navigation, timer boundaries,
  repeated rendering, persistence/details gates, and the course-replay guard.
  The fixed fixture also passes AddressSanitizer.
- Android startup configured `SQLITE_TMPDIR` but not `TMPDIR`. The pinned NDK
  28.2.13676358 libc++ checks `TMPDIR`, `TMP`, `TEMP`, and `TEMPDIR`, then uses
  `/data/local/tmp`, outside the app's writable private storage. This behavior
  was confirmed in the installed aarch64 `libc++_shared.so`. Find BMS staging
  and archive caches use `std::filesystem::temp_directory_path()`, so ordinary
  app execution could fail to stage files; a thrown filesystem error could
  also escape the download worker. Application startup now points native and
  SQLite temporary storage to the app cache. Three Java regression tests
  compile the actual application class and verify initialization, inherited
  path replacement, and assignment failures; they failed before and pass after
  the fix. Existing Android instrumentation also checks the process `TMPDIR`
  and probes that directory, but was not run on a device during this review.

Separate Astra reviewers approved each runtime fix and its regression coverage.
The Android host tests substitute the Android environment API; they do not
directly execute libc++ in an Android app process.

## Continued program review

A subsequent course-transition review found a smaller gameplay defect:
continuing from a stage result defaulted Club Beat to off. Normal continuation
now carries the completed attempt's setting; saved same-pattern retry stages
retain their own recorded setting. The existing production-method fixture
checks live on/off and both opposing saved-stage settings. It failed before the
fix and passes afterward. The full desktop build and 413-test CTest suite also
passed after this change.

The next review found a separate lifetime blocker in Settings: its Back button
can synchronously destroy a dynamically owned SettingsScene, after which
`handleEvents()` reads `previewActive`. Consumed events now return immediately.
The new regression embeds the production event handler and scene ownership
methods: AddressSanitizer reports a heap-use-after-free before the fix and passes
afterward. It also checks unconsumed preview input, consumed preview input,
preview on/off, and display-preview restoration after focus loss.

The following full run exposed an intermittent IR credential-reactivation
failure. Initial profile activation could resume delivery before publishing its
initial status snapshot; Retry All could also replace a newly completed upload
with an older database snapshot. Activation now stays paused until publication,
and snapshot refreshes validate both profile generation and status revision.
Manual enqueue reloads through the same guard. Deterministic regressions force
the formerly failing ordering without increasing timeouts.

## Custom-skin IR integration

The user identified missing IR data across selection, gameplay, and result
skins. The account lookup expected a top-level username, but Tachi's documented
[user endpoint](https://docs.tachi.ac/api/routes/users/) returns an API envelope
with `success` and `body.username`.
The parser now validates that envelope and reads the actual account identity.
Tests cover successful, malformed, unsuccessful, and invalid-name responses.
The public endpoint shape was checked without using a user credential.

Selection now chooses an enabled provider that supports chart rankings and
invalidates its local ranking cache after a successful score submission.
Gameplay fetches rankings asynchronously, validates chart/profile/provider
identity, and projects the selected IR target independently of the native
pacemaker. Incomplete rankings remain unavailable for percentile/next-rank
targets. Normal play also retains the current user's pre-submission rank for
the result skin when the ranking arrives in time.

Result skins now receive ranking rows, current and previous rank, player totals,
complete-ranking clear counts/rates, ranking-position scrolling, and submission
timers. The scene owns request generations, continues pagination, and refreshes
after successful submission. Aggregate rates are exposed only with complete
ranking evidence. Offline rendering does not initiate network requests.
The mapping was checked against the pinned beatoraja reference revision
`c2ed5db1a46145ed10790c3872f717e95b59db9d`.

Focused tests cover provider selection, cache invalidation, target projection,
ranking completeness, scene request lifecycle, result property mappings,
scrolling, upload timers, and previous-rank handoff.

A subsequent review found a pagination liveness race in all three scenes. A
page could finish before the immediate snapshot re-read, leaving a further
page token on an already-consumed revision. A cancelled older request could
also temporarily reject page scheduling without later changing that revision.
The scenes now check continuation eligibility before skipping an unchanged
projection. Regressions force immediate three-page completion and a rejected
then successful request with no intervening revision, and verify completed or
blocked pagination does not repeat.

The next compatibility review found a smaller result-slider defect: accepting
position `1` made the row offset equal the population and displayed an empty
window. Both pointer and Lua ranking writers now follow the pinned source's
`[0, 1)` float range, including double values that round to `1` when narrowed.
Tests cover numeric/named writers in ordinary/course result sessions, ignored
endpoints preserving a queued valid position, and bridge range/rounding cases.
The regression failed before the fix; the complete skin-session executable
passes afterward.

## Confirmed release-gate blocker

The macOS workflow's required cross-platform release check fails before
packaging or publication. `test_download_redirects_cannot_downgrade_https_to_http`
expects exactly three curl redirect guards in `DownloadSupport.cpp`. The new
`probeDownloadUrl` HEAD request correctly adds a fourth, producing `4 != 3`.

The fix audits each curl request between URL setup and execution. Every request
must restrict redirects using the initial URL's protocol policy. Adding another
protected request no longer fails the release gate, and an extra guard in one
request cannot compensate for a missing guard in another. Runtime download
behavior is unchanged.

The original failure was reproduced with:

```sh
python3 -m unittest discover -s tests -p '*release*tests.py'
```

After the fix, all 68 release-contract tests pass. Independently replacing each
of the five policy calls in memory with unrestricted `http,https` makes the
updated check fail, including the new HEAD probe and difficulty-table request.

## Intermittent verification failures

Rebuilding all targets and repeating CTest exposed additional timing assumptions:

- `find_bms_ios_file_bridge_tests` successfully downloaded the exact expected
  archive using offsets `0,16384,16384`, but required `0,16384,32768`. Foundation
  can checkpoint fewer bytes than the server has sent. Resume assertions now
  require the expected attempt count, bounded resume offsets, and the
  existing exact final-byte comparison. The server-ignores-range case receives
  the same checkpoint tolerance. A subsequent run returned `0,0,16384` with the
  correct complete archive, confirming that a checkpoint may also be empty.
  Zero-byte restart remains valid. The user-approved recovery scenario still
  checks native resume availability and partial-file reuse. Restart,
  cancellation, and retry limits remain covered.
- `lua_skin_runtime_tests` ran alongside sibling tests despite retaining the
  production 4 ms callback deadline. It failed with
  `skin_lua_wall_time_limit_exceeded`; a focused rerun passed. It now uses
  `RUN_SERIAL`, as the existing Lua host and session tests already do. Runtime
  deadlines are unchanged.
- `play_skin_session_tests` used the production wall clock while verifying
  state, timers, input, and rendering. Diagnostics confirmed
  `skin_lua_wall_time_limit_exceeded` in otherwise valid custom-timer and
  input-snapshot callbacks; other runs failed different callbacks. The session
  suite now controls wall time through the existing test-only runtime hooks.
  Its instruction and allocation limits remain active. Production builds do
  not compile the clock override and retain their real monotonic clock and
  unchanged deadlines. The old session timing assertion would become vacuous
  under a controlled clock, so it is replaced by runtime tests for the exact
  4 ms callback and 6 ms cumulative frame boundaries, overrun rejection, and
  frame reset behavior. The existing real-clock host-call timeout test was
  defined but absent from the test runner; it is now explicitly invoked.
  Both Lua suites passed five consecutive focused runs after these changes.

A later full run also exposed two timing-sensitive semantic checks. The
oversized selected-artwork test imposed a two-second limit on asynchronous
processing of two images over 32 MiB; it now uses a test-only future wait between
the render that queues decoding and the render that publishes its result. The
bridge writer suite expected semantic callback errors while retaining the real
Lua frame deadline; it now uses the existing test clock and explicitly verifies
that instruction exhaustion remains active. Both failures passed on an isolated
rerun, but the original logs did not record enough detail to establish their
exact timing trigger. New failure diagnostics include upload/render counts and
actual callback status/codes. Both test executables have a 120-second CTest
process bound. Production loading behavior and callback deadlines are unchanged.

The failed Lua test also prevented its dependent
`beatoraja_music_select_skin_ledger_evidence_contract` from running. These
failures were observed during concurrent Android CI work on this machine.
Serial CTest scheduling cannot isolate tests from external processes, and no
production deadline was relaxed to accommodate machine contention.

## Consecutive final reviews

After the final result-slider correction, two sequential independent Astra
reviews checked the same production changes and found no actionable issues,
including minor compatibility defects:

1. Cumulative review of IR snapshot races, pagination, account invalidation,
   gameplay targets and result handoff, aggregate/timer mappings, float writer
   boundaries, scene lifetimes, and native temporary storage.
2. A different reviewer checked the complete production diff and earlier task
   commits, including ranking ownership, pre-submission rank capture, upload
   refresh, Settings and result lifetimes, Club Beat propagation, and
   profile/download/replay recovery paths.

The subsequent test-only timing stabilization was independently approved for
production isolation, preserved deadline/instruction coverage, bounded waits,
and unchanged rendering assertions. No production execution logic changed
between the two clean reviews and that test stabilization.

## Review scope and limits

Review covered first-launch/navigation and scene/view callback lifetimes,
gameplay/audio/input shutdown, course/replay transitions, profile and database
changes, download/import/recovery, IR delivery and custom-skin integration, and
release workflow checks. The desktop app and all test targets build successfully.
After the final program/test changes and full rebuild, the complete parallel CTest
run passed 417/417 tests in 134.44 seconds, including native iOS transfer,
production-method skin lifecycle regressions, and dependent evidence checks.

Additional verification:

- Release contracts: 68/68 passed.
- iOS build setup: 49/49 passed.
- iOS artifact-auditor tests: 16/16 passed.
- macOS artifact-auditor tests: 7/7 passed.
- Result timeout and Settings event-lifetime regressions: passed with AddressSanitizer.
- Android startup: 3/3 Java regression tests passed; the application class also
  compiles against the real Android 36 SDK with Java 17.
- Independent Astra review: two consecutive final program reviews found no
  actionable issues; the subsequent test-only stabilization was also approved.

This review does not establish signed release readiness. Local validation did
not include a live authenticated Tachi session, full iOS/Android build,
physical-device smoke test, Windows build,
signing/notarization, store validation, or deployment. Artifact-audit tests validate the
auditors; they do not substitute for auditing a signed release artifact.
