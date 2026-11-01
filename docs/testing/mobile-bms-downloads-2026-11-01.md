# Mobile BMS browsing and durable downloads — verification

This change adds the mobile browser and moves both Find BMS entry points onto
application-owned jobs. The approved design and its iOS policy amendment are in
[the design document](../superpowers/specs/2026-11-01-mobile-bms-browser-design.md).

## Automated coverage

The download tests exercise durable manifest reload, stale callbacks, cancellation,
queue order, retained-file import, interrupted publication, library acknowledgement,
Google Drive confirmation, PixelDrain alternatives, and scoped browser headers.
Publication recovery uses file identity and bounded metadata. The Google Drive
confirmation test includes a sparse 64 GiB file, which is rejected from HTML
inspection based on its size without reading the payload.

A fresh whole-branch review identified four lifecycle defects. Each now has a
regression that failed before the fix and passed afterwards:

- Concurrent Android retries serialize access to the retained partial file through
  stream closure and checkpoint completion. The Java loopback test coordinates two
  writers with latches and verifies the final bytes.
- Android browser method tracking uses the current main-frame request. A prior GET
  cannot authorize replay of a POST redirect as GET.
- Failed completed-file/manifest handoff is visible and retryable without relaunch.
  The test forces atomic manifest replacement to fail, then recovers the retained
  archive without starting another transfer.
- Cancelling an unprepared keep/delete decision clears its artifact reference so
  the job and private files can subsequently be removed.

Android backup tests cover exclusion of browser storage from cloud backup and
phone-to-phone transfer. Download manifests and payload staging use no-backup
storage separately.

## Runtime boundaries

A successful native build does not prove OS scheduling or locked-device behavior.
Android 13 exercises the foreground-service path; Android 14+ UIDT needs separate
runtime validation. iOS continued processing requires an eligible physical device
and an OS grant. Older iOS and HTTPS/session-bound requests need the app open when
continued processing is unavailable, as disclosed before download admission.

Process termination and user force-stop/force-quit are distinct. Neither platform
promises continued execution after an explicit user stop. Interrupted extraction
restarts from retained data rather than resuming an arbitrary decompressor position.

## Recorded checks

- Desktop `main` and affected native test targets: built successfully.
- Downloads, library, selector, application, and UI groups: 149 CTest cases
  verified, including focused reruns for three integration failures. The failures
  were fixture declarations for the new modal and the catalog's three-segment key
  convention; the corrected cases pass.
- Android release/backup workflow checks: 21 passed.
- iOS build-setup checks: 55 passed.
- Final Android native compilation: passed with the pinned NDK and existing Ninja
  build directory, using six workers.

Android restricted-file release APK packaging passed. It was installed with
`adb install -r` on the connected Samsung Android 13 device using its existing
version code 5 and matching signing certificate. Installation succeeded without
uninstalling or clearing data. Implementation fixes were pushed after installation
and before runtime testing so iOS testing could proceed in parallel.

## Device-driven corrections

The connected Android 13 phone completed the 4 MiB loopback transfer while the
launcher was foreground. Initial import then exposed Android's hard-link
restriction. Import now reads the retained archive in place; the private file
keeps its format suffix. Existing extensionless retained files are renamed using
metadata only. The retained-file integration test verifies import without a
second transfer or hard link, and the manifest test covers the filename upgrade.
The platform restriction is documented in [AOSP's app sandbox policy](https://android.googlesource.com/platform/system/sepolicy/+/android-9.0.0_r42/private/app_neverallows.te).

The supplied iOS build 1627 crash matches archive dSYM UUID
`908F94A8-BD48-34C5-A523-11F058071D88`. Symbolication identifies
`DownloadPlatformIOS.mm:319`: an asynchronous block copied a `Job` through a
reference after the caller's object was gone, failing while copying request
headers. Runtime and progress callbacks now own their job snapshot; completion
captures its descriptor before dispatch. A Foundation queue regression fails
with reference capture and passes with owned capture. This affects both browser
and Find BMS downloads. No crash report or private request data is committed.

Also corrected both Manbow bookmarks to `/event/event.cgi`, made a browser table
import request a sidebar refresh, constrained Downloads text to its panel width,
and fixed Android address-field contrast and hardware Enter handling. iOS HTTP
link clicks now use programmatic WebKit navigation to avoid handing universal
links to installed apps; the physical-device Drive retest is pending.

After these corrections: desktop, unsigned iOS, and Android release builds pass;
all 13 download-group tests pass, as does the focused library-operation test.
The updated Android APK was installed successfully with version code 5 before
another push and runtime retry.

## Final connected Android observations

On the Samsung Android 13 device with the updated release APK:

- Retrying the retained archive completed extraction and library acknowledgement,
  with no additional server request. Downloads displayed Complete.
- The foreground-service download continued with the launcher foreground. A
  separate throttled transfer was cancelled from its notification; the server
  observed a closed connection after 720,896 of 4,194,649 bytes, and Downloads
  displayed Cancelled on return.
- A user-initiated new-window link displayed the native archive confirmation.
  Cancelling that prompt and pressing Back returned to the originating page.
- A POST attachment displayed the explicit external-browser fallback. The server
  saw only the WebView request, with no download-service replay.
- The address field was readable, the corrected Manbow bookmark loaded the event
  list, and the Downloads rows remained within the panel at landscape size.

The fixtures were synthetic and served over an adb loopback mapping. Test archives
and download history were removed after validation. No distribution upload was
performed. Physical iOS retesting of the reported crash, Drive navigation, and
modal wrapping remains pending; unsigned iOS compilation and the ownership
regression pass. Android 14+ UIDT, locked-device scheduling, and physical process
recovery were not exercised in this device session.

## Locate and non-cancelling dismissal

Completed, indexed jobs now expose Locate using their persisted import destination.
The connected Android check opened the synthetic package's folder from both Play
and Course; the latter waited for the Play transition. Removing the fixture's
files then pressing Locate kept Downloads open with the missing-file message.

Find BMS offers Hide (keep running) separately from Cancel while its worker runs.
Behavioral regressions verify hidden completion/index delivery, retry ownership,
no cancellation on hide, and suppression of the automatic Downloads popup when a
hidden lookup queues its transfer. The new hide assertion failed before the
policy change. Persisted Locate eligibility tests cover pre-index, completed,
packed, cancelled, attention, and missing-result states.

After both additions, desktop main, unsigned iOS, and Android release builds pass;
all 13 download tests and the localization catalog check pass. The Android APK
was installed with the existing version code and the implementation pushed before
device testing. Physical iOS interaction testing remains separate.

## Consolidated library actions and Tasks tabs

The standard selector now groups folder and browser actions under Add BMS with a
plus icon. Tasks contains Library and Downloads tabs, with their combined count
on the selector and separate counts on the tabs. The skin toolbar retains that
count when collapsed. Find BMS remains open after submitting a shared download;
View Downloads is the explicit navigation action.

The desktop application build and all 15 `area_downloads` tests pass. Coverage
includes real-view tab switching, combined counts, cancellation, compact portrait
and landscape layouts, toolbar rebuilds, and shared-job progress without automatic
navigation or duplicate indexing. Selector lifecycle, main-menu tab, localization,
and archive-flow contract checks also pass. The changed native sources pass Android
compile checks; TasksModal and FindBmsModal pass iOS compile checks. These latest
UI changes have not been installed or tested interactively on a physical device.

## Browser input isolation

Both selectors now honor native browser focus independently of SDL app/window
focus. Regressions reproduced the built-in selector forwarding browser events
and tab shortcuts, and the custom selector accepting input and queued skin
actions. They now verify blocking during browsing and fresh input after closing.
The custom-selector tests run with Lua skins enabled and disabled. Queued skin
actions are discarded rather than applied or replayed later.

The selector error/lifecycle, export/action, and built-in tab suites pass, as does
the desktop application build. iOS compile checks cover the browser lifecycle,
shared focus state, and both selectors. Android compile checks cover the shared
focus state and both selectors. Physical-device input retesting remains pending.

## PR review recovery and admission fixes

All six review findings were checked against the current implementation. Added
regressions reproduced loss of deleted-variant paths during completion recovery,
installation of an extracted tree whose nested payload could not be synced,
retained-archive deletion on extracted mismatch, unknown-extension direct-import
failure, and missing Retry after failed transition/completion manifest writes.
The maintenance test holds a publication in progress, checks that admission is
refused until it settles and recovery completes, then verifies that application
import toggles cannot override a held maintenance reservation.

The 15-test download group passes, including real-view Retry recovery without a
second transfer and centered tab text in compact portrait/landscape layouts.
Storage task and settings event/lifetime checks also pass. Desktop main builds;
iOS and Android compile checks cover publication, staging, workflow, coordinator,
import, both affected modals, and storage integration. Failure injection and
process-boundary reconstruction exercise recovery; physical power-loss and device
UI retests were not performed for this follow-up.


## Review loop, completed-history cleanup, and iframe downloads

The review loop identified and fixed publication ancestry durability, preservation
of rollback material after a successful rename followed by failed directory sync,
and iOS runtime admission for retained-file Retry/Keep. Failure-injection tests
cover both backup and install renames and recovery while sync still fails. A
coordinator regression checks runtime admission for initial import and retained
retry/keep. Runtime leases carry revisions so stale completion or expiration
callbacks cannot finish a newer attempt.

Clear completed is enabled only on the Downloads tab when an acknowledged,
completed job exists. The real-view regression covers mixed complete, unindexed,
queued, failed and cancelled jobs, retained files and persisted history.

An iOS simulator harness compiled from the actual browser source captured the
provided public OneDrive archive as a GET and displayed the native confirmation
with its correct filename. The generic WebKit download delegate now handles
attachments from hidden iframes. Synthetic iframe GET/POST cases were checked.
A separate Android WebView probe on the connected Android 13 device uses the
production request policy: the OneDrive attachment and synthetic iframe GET were
accepted; a POST preserved through a 307 redirect was rejected. Java regressions
also cover historical URL evidence, unrelated metadata POSTs and overlapping
GET/POST ambiguity. These probes stop before durable transfer/import, so they do
not establish a new end-to-end OneDrive import result.

The desktop app build, all 15 download tests, localization catalog, unsigned
iOS build, and Android debug-variant Java compilation pass. The final fresh
source review found no actionable findings.
The production Android app was not replaced during this probe session; physical
iOS continued-processing and lock behavior remain untested.


## Native browser controls and actual WebKit transfer ownership

Browser URLs now live only in process memory; Manbow is the launch default and
first bookmark. iOS uses a native search field configured for URL entry, SF Symbol
navigation items, disabled history controls and progress/Stop state. Android uses
SearchView/Toolbar controls, standard overflow menus, history icons and progress.
A separate probe built from the Android activity source verified the rendered
controls, Manbow-first menu, GingerRush after closing/reopening, and Manbow after
force-stopping/restarting the probe. The installed production app was not replaced.
iOS accessibility inspection found the Website address, clear, Sites and Reload
controls; its simulator frontend could not open for screenshot inspection.

The reported OneDrive failure was reproduced beyond admission: replaying its URL
through URLSession returned HTTP 200 with about 35 KB of HTML. Keeping the original
WKDownload instead produced a 57,124,223-byte ZIP with 1,077 entries and nine BMS
charts. The new application-owned WebKit adapter and real DownloadCoordinator
then retained that same full archive in Ready to import state after detaching the
browser UI. The harness stopped at durable handoff, before library import.

A synthetic 2 MiB HTTP archive tested interruption followed by immediate Retry.
The old writer stopped, WebKit resumed with Range starting at byte 1,048,576,
and attempt 1 completed with the full 2,097,423-byte archive. The adapter preserves
attempt identity, rejects non-2xx responses, persists bounded private resume tokens,
and never falls back to URLSession for a captured browser request. Cancellation
during asynchronous resume waits for the new writer before collecting its token.
Without usable resume data, the job explicitly asks to choose the source download
again. Existing jobs containing the old HTML response likewise need a fresh download.

All 15 download tests and the desktop application build pass. Persisted request
identity and validation have new coverage. The full unsigned iOS build, final iOS
browser source check and Android Java compilation pass. A fresh source review
found no actionable findings. Physical iOS background/lock behavior and importing
the OneDrive package into the installed player library remain separate checks.


## Address alignment and text search

Native simulator frame inspection reproduced an 8-point vertical offset between
the UISearchBar field and the navigation buttons (centers 56 vs. 48). A native
UISearchTextField directly in the title slot now centers at 48, matching the
button container. Both mobile address inputs share URL/search classification:
HTTP(S), domains, localhost and IP literals navigate; ordinary text opens Google
with encoded query bytes. Regression cases cover whitespace, Korean, punctuation,
email/search operators, unsupported schemes and non-BMP URL paths.

The focused download-jobs tests, iOS native browser compile/runtime layout check,
Android Java compilation and browser JNI source check pass. No app distribution
or production-device installation was performed for this follow-up.
