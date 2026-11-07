# Find BMS and downloads

## Intent and user flow

Find BMS searches supported external sources, downloads and validates a matching
archive, and hands the resulting content to the library. The flow avoids exposing
partially written or unverified content as a chart folder.

Automatic lookup uses GingerRush, Konmai, Wriggle, and BMS Search.

## Mobile browsing and downloads

On iOS and Android, **Browse Sites** opens an embedded browser with Manbow,
GingerRush, and BMS Search bookmarks, navigation, an address field, and an external-browser action.
Manbow appears first and is the default on each app launch. Reopening the browser
within the same app session resumes the last page; the URL is not saved to disk. Native URL-entry controls provide clear/edit/go
behavior. HTTP(S) URLs and hostnames navigate directly; other input opens an
encoded Google search. Navigation icons reflect available history, and loading shows progress
and a Stop action. Sites, table import and Downloads remain available.
The standard selector's **Add BMS** menu (plus icon) contains **Add Folder** and
**Browse Sites**. Android also keeps **Manage Files** in that menu, with direct folder
linking only in all-files builds. The skin selector exposes browsing in its More menu. **Import table** is enabled for the active page when it declares a
`bmstable` header link or displays difficulty-table header JSON (`name`, `symbol`,
and `data_url`). GingerRush table URLs also retain their direct recognition.
While the native browser is in the foreground, both selectors block pointer,
scrolling, keyboard, and controller input behind it. Input resumes after closing
the browser; queued custom-skin actions are discarded while it owns focus.
Navigation and popup changes clear the previous page's detection. Import uses the
existing table importer and duplicate handling. Importing a table does not fetch
its chart packages. Event tables may have an empty symbol.

Accepted browser archives and Find BMS selections enter the same **Downloads**
queue in **Tasks → Downloads**. The Tasks modal has **Library** and **Downloads**
tabs. Its notification badge adds active library tasks to outstanding downloads,
including pending indexing, errors and keep/delete decisions. Completed, indexed
and cancelled downloads do not contribute. Each tab shows its own count; the skin
selector carries the combined badge on More (or Expand when collapsed).

Closing a dialog, closing the browser, or changing scenes leaves accepted
work running. Downloads shows transfer/import/indexing phases, explicit Cancel
and Retry, and keep/delete decisions for unmatched content. Once library indexing
finishes, **Locate** closes Downloads and opens the imported folder or packed
archive in the current selector, clearing filters that could hide its charts.
Missing files leave Downloads open with a refresh-library message. Remove discards the
job's private retained files and history after completion; it does not remove an
installed chart from the library. **Clear completed** removes all completed, indexed
entries together, preserving installed content and every unfinished or unacknowledged job.

Find BMS stays open after queuing a mobile download and follows that shared job's
progress, cancellation, retry and keep/delete decisions. **View Downloads** opens
the Downloads tab explicitly; starting or finishing the job does not navigate there.
Browser and notification requests open Tasks directly on the Downloads tab.

While Find BMS is working, **Hide (keep running)** dismisses its dialog without
cancelling the task. **Cancel** stops it. Hidden lookup completion does not reopen
Downloads automatically. On desktop, the task continues in its owning selector;
on mobile, accepted downloads belong to the durable queue described above.

Android uses user-initiated transfer jobs on Android 14+ and a separate data-sync
foreground service on older releases. Transfers can continue while backgrounded
or locked, with notification cancellation. Import/indexing resumes while the app
is active. Pending transfers wait for foreground admission when the OS cannot
legally start them in the background.

iOS preserves legacy HTTP support and HTTPS downgrade protection. Anonymous Find
BMS HTTP requests can use a persistent background URLSession. HTTPS and
session-bound Find BMS requests use a redirect-controlled foreground session.
Browser downloads retain the actual WebKit transfer, including service-worker
responses that cannot be reproduced by requesting their URL again. The shared
queue owns the browser engine during transfer, independently of the browser UI.
Keep the app open unless iOS 26+ grants continued processing. When that runtime ends, retry from Downloads;
the retained archive or resumable transfer is reused. The notice appears before
admission. Force-quit is not a promise of continued execution on either platform.

Attachment downloads from embedded frames (including OneDrive) use the download
request itself rather than the main page's method. iOS obtains this from WebKit;
Android requires matching navigation evidence and rejects ambiguous requests.
iOS stores an opaque browser-transfer identity and bounded private resume data.
Retry uses WebKit resume data when available; an expired/unresumable browser
session requires choosing the download again. It never silently replays that URL
through another HTTP client. Cross-origin redirects after WebKit download admission
require a new browser action because its delegate cannot strip request headers.
Only replayable HTTP(S) GET downloads enter the service. POST, blob, and uncertain
browser requests offer an explicit external-browser fallback. Cookies and referrers
are scoped to their origin and stripped on cross-origin redirects. Private request
state, downloaded files, and extraction staging are outside public Documents and
excluded from backup. No private catalog API is polled by the browser integration.

## Code map

- `src/bms_search/` contains source drivers, candidate normalization, download
  storage identity, staging, archive classification, and workflow helpers.
- `src/scene/FindBmsTask.*` owns bounded lookups on mobile, and the synchronous
  download/artifact workflow on desktop.
- `src/downloads/` owns durable jobs, native transports, publication recovery,
  browser request policy, and application-level library delivery.
- `src/scene/TasksModal.*` observes jobs without owning their lifetime.
- Native browser and transfer hosts live beside the downloads adapters on iOS
  and in Android's `com.snurhythm.asobmashow` Java package.
- `src/scene/FindBmsDialogPolicy.*` and progress presentation code connect
  search/download state to the main-menu UI.
- Archive support and repository/library services validate the downloaded result
  before library discovery consumes it.

## Boundaries and invariants

Download and extraction work use staging locations and a stable storage identity
before publishing a visible folder. Archive classification distinguishes solid
archives and unsupported entry behavior instead of assuming random access.
Cancellation and replacement handoff must leave no stale UI request or unsafe
partial artifact behind.

For the synchronous desktop flow, the task remains busy through completed-result handoff, so a fast artifact
resolution cannot re-enable actions against the previous pending artifact.
Cancelling a request is nonblocking and still delivers the service result,
including artifacts requiring a keep/delete decision. Shutdown and replacement
join all work, including uncancellable artifact resolution, and discard queued
updates. Pending progress retains the latest 160 events in order. Selection
generation remains scene policy. Mobile downloaded-path indexing is application-owned:
job acknowledgement happens only after the library task succeeds. A failed index
can be retried without another transfer or import. Restart may repeat idempotent
indexing if it stopped between database commit and job acknowledgement.

Mobile manifests are atomically replaced, bounded in size and count, and identify
each transfer attempt. Cancellation and late callbacks cannot overwrite newer
attempts. Completed transfers are retained before import. A bounded publication
journal reconciles interrupted file swaps by identity; recovery does not hash or
scan an entire archive. Interrupted extraction can restart from the retained file.
Google Drive confirmation produces another durable transfer attempt. PixelDrain
API rejection can advance to a resolved alternative package source; unrelated
errors stay with the selected provider.

Selected downloads do not impose fixed archive, chart-member, total expanded-byte,
or entry-count quotas. Chart verification reads one member at a time, and extraction
streams data to disk. Hash validation, cancellation, path containment, available
staging space, and atomic publication remain enforced. Callers can still supply
explicit verification/extraction limits for bounded work. Network metadata response
limits are separate and remain in place.

## Verification

Use `find_bms_download_tests`, archive-file concurrency tests, chart scanning
tests, and `scripts/check_find_bms_archive_flow.py` when changing the complete
workflow.
`find_bms_task_tests` covers task admission, progress bounds, cancellation,
replacement, destruction, and complete production scene launch/delivery methods.
It includes deterministic immediate-completion artifact gating and exactly-once
indexing. `main_menu_records_lifecycle` checks the production destructor with
the real task to ensure joining before scene dependencies are destroyed.

The download group also includes `download_jobs_tests`, `download_coordinator_tests`,
`download_publication_tests`, and Java loopback transport tests. The latter exercise
resume validators, cancellation, overlapping retry writers, and redirect credentials.
Native builds and physical-device checks remain necessary for OS scheduling behavior.

## Related pages

- [Library and chart scanning](library-and-chart-scanning.md)
- [Mobile and platform integration](mobile-and-platform-integration.md)

Publication recovery records obsolete variant paths before deletion, so a restart
can remove their old library entries even when the files are already gone.
Copied directory contents and directory entries are flushed before the install
journal is recorded. This flush walks metadata and does not reread payloads.
Completed archives remain available through mismatch and failed Keep Files paths;
attachments without a recognized archive suffix use extraction even when Skip
Unarchiving is enabled. Persistence-blocked imports expose Retry in both Tasks
and Find BMS after storage is freed.

Storage maintenance requires download publication to finish or recover first.
Admission refuses an active importer or unfinished publication; once admitted,
it holds a separate reservation against imports until maintenance releases its
guard. Network transfers can continue into private download storage.
