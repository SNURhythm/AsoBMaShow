# Mobile BMS browser and shared background downloads

Status: proposed design, awaiting approval before implementation.

## Intent and agreed scope

Let players browse GingerRush and Manbow inside AsoBMaShow on iOS and Android,
then import a selected table or download a selected archive. Direct catalog API
integration and self-hosting are postponed. Downloads must continue when the
app is backgrounded or the screen is locked, subject to OS scheduling.

The user explicitly requested that the existing download path benefit from
the background-processing work too. Migrating Find BMS is part of this feature,
not a follow-up. Desktop browsing is outside the first version.

## Approaches

The discussion selected native embedded browsers with small site adapters and
shared import handling. External-browser-only handoff cannot provide the chosen
in-app experience. A separate API-driven catalog would duplicate the sites and
conflict with the decision to postpone direct integration.

For background work, this design recommends persistent OS-backed transfers plus
optional continued processing. Merely wrapping the current scene worker in
`BGContinuedProcessingTask` would benefit only iOS 26+ and would not reconstruct
downloads after process death. A browser-only transfer service would leave Find
BMS with different lifecycle behavior. Both entry points therefore submit to
the same application-owned download service.

## Browser behavior

Add a mobile Browse BMS entry with GingerRush and Manbow bookmarks. Provide
back, forward, reload, close, and open externally. Retain the current page while
the app lives; persist the last navigable HTTP(S) URL for reopening after launch.
Closing the browser does not cancel accepted downloads. A shared Downloads view
shows active work, progress, retry/cancel actions, and completed import results.

GingerRush table pages expose Import table using the published table header URL.
Pass it through the existing table importer and duplicate handling. Do not fetch
the private frontend catalog automatically or infer chart hashes from Manbow.
Manbow browsing follows the author's selected package link, including navigation
to cloud hosts. Preserve the user's choice among package variants. Visiting a
page never starts a package download. Table import does not download its charts.

Use WKWebView on iOS and WebView on Android. Recognize explicit user download
actions and attachment responses, including new-window navigation. Treat URL
extensions and suggested filenames as hints; verify the completed archive using
existing content checks. Native bridges are narrow and origin-scoped. Arbitrary
pages cannot request local paths, run native code, or silently import content.

A browser download handoff contains the request URL/method and the applicable
session context, including cookies, referrer and user agent. Preserve cookie
scope and redirect rules; never forward unrelated credentials to another host.
Private request/resume state is excluded from logs, public Documents and backups.
Site scripts are independently written adapters, not copied frontend code.

Only hand replayable HTTP(S) requests to the OS transfer service. Blob downloads,
one-shot POSTs and WebKit-only sessions require an explicit foreground download
fallback or opening externally; do not silently repeat a stateful request or
claim that WKDownload provides persistent background transfer. Once a foreground
fallback yields a file, enqueue it in the same import service. Explain that such
a fallback needs the app open before starting it.

## Shared ownership and migration

Today FindBmsTask owns a worker, cancellation and progress until scene result
delivery. The iOS file helper uses an ephemeral NSURLSession, local resume data,
and cleanup tied to the synchronous call's lifetime. Android uses the C++ curl
download path. Those lifetimes cannot own a durable mobile transfer.

Introduce a persistent job record and application-owned coordinator. Both Find
BMS selections and accepted browser downloads create jobs with stable IDs,
source information, destination identity, validation expectations and state.
Retain provider lookup logic and existing archive storage/verification policies;
adapt resolved package requests to the shared transfer interface. Scene closures
and stack pointers must not be retained by platform callbacks.

FindBmsTask remains suitable for bounded lookups. Download UI subscribes to job
snapshots and detaches on scene destruction instead of cancelling the transfer.
Explicit Cancel remains cancellation. Existing retry decisions and keep/delete
artifact decisions become durable needs-attention states; no background worker
blocks waiting for a dialog. Desktop may keep its existing transport behind the
same admission/result contract without acquiring mobile scheduling behavior.

Use one active archive job initially, with further selections durably queued.
No automatic retries after explicit cancellation. Reconstruct the queue before
accepting new work on launch. Publish results by job ID exactly once, including
after callbacks are redelivered or a scene is recreated. Library refresh occurs
on the application owner thread, independently of the original scene.

## iOS execution

Use a stable background NSURLSession and delegate-based download tasks for
replayable package transfers, including Find BMS. Reconnect persisted jobs to
system tasks at launch. Integrate background-session completion delivery with
the SDL/UIKit application delegate, retaining completion handlers until durable
file handoff finishes. Move downloaded temporary files into private durable
staging before returning from the download callback. No scene or render loop is
required for transfer completion bookkeeping.

Keep supported older iOS versions. On iOS 26+, submit a continued-processing
request from the user's foreground download/import action to cover active work
and, while granted, validation/extraction. Report real phase progress, respond
to cancellation/expiration, and complete the system task exactly once. Request
immediate admission with a failure fallback rather than creating a second queue
that can later start obsolete work. Use CPU/network resources only.

Continued processing is an enhancement, not the transfer's persistence mechanism.
If admission fails, the background URLSession transfer still proceeds and import
finishes in the foreground. Do not keep a continued task alive while waiting for
user input or indefinitely stalled transfer progress. If the download completes
after that runtime has ended, retain the archive for foreground import; do not
assume a new continued-processing task can be launched from a background callback.

Treat continued-task expiration conservatively as an interruption: stop the
associated work, preserve recoverable state, and require foreground continuation.
The system's cancel control must not leave a hidden transfer running. App Cancel
also cancels the system transfer and scheduled continued request. Do not promise
continued execution after force-quit. Configure private staging protection so
accepted downloads can finish while locked after first unlock; credentials keep
appropriate protected storage and are never copied into user-visible Documents.

## Android execution

Use user-initiated data-transfer jobs on Android 14+ and a dataSync foreground
service on older supported versions, launched from the visible user action.
Expose download progress and cancellation through a notification. Use the same
persisted job identity and recovery rules for browser and Find BMS downloads.
Do not borrow the music playback service or its background permission.

The transfer host must initialize independently of SDL Activity/render startup.
Retain resumable transfer behavior with HTTP validator checks, whether the worker
uses the existing curl implementation or a small platform transport adapter.
Stop/checkpoint when the OS withdraws runtime. Pending queue entries that cannot
legally start from background remain waiting until foreground admission. Archive
import may finish on foreground return; the first version promises background
file transfer, not unrestricted background CPU processing on every Android release.

## Durable state, safety and scale

Persist transitions for queued, transferring, downloaded, importing,
needs-attention, complete, cancelled and failed jobs. Reconcile OS tasks and disk
artifacts at startup rather than assuming either side committed atomically with
the other. Completion, cancel and publication races must have one durable outcome.

Keep completed downloads and job manifests in private durable storage, not an
evictable cache. Retain the existing space checks, TLS/redirect policy, archive
validation, staging containment, cancellation and atomic publication guarantees.
Check background-session redirect behavior explicitly: a platform switch must
not silently weaken the existing HTTPS downgrade protection. Incompatible
transfers use the disclosed foreground fallback.

Checkpoint between import stages. Interrupted extraction may restart in private
staging from the retained archive; do not promise arbitrary decompressor resume.
Publish a finished archive/folder atomically, then record idempotent library
indexing. Existing artifacts awaiting keep/delete remain available after relaunch.
Use bounded records and progress snapshots. Do not copy, hash or reread entire
large archives simply to show confirmation or reconcile completed jobs. Reuse
archive indexes and retain the existing selected-content verification behavior.

## Validation and delivery

Extend useful Find BMS task/download tests for scene detachment, cancellation
races, retry decisions, durable recovery and exactly-once publication. Use small
synthetic fixtures and sparse large-file tests for bounded metadata behavior.
Verify browser request handoff, cookie scope, redirects, new-window downloads,
unsupported foreground fallback, and GingerRush duplicate table imports.

Device checks cover home/lock during both download and extraction, connectivity
loss, low disk, OS runtime expiration, system cancellation, process termination
and relaunch. Test iOS 26+ and an older supported iOS version; Android 14+ and the
older service fallback. Distinguish OS termination recovery from user force-stop.
Run affected subsystem tests and unsigned mobile builds; no deployment is included.

Implementation order: establish the shared job lifecycle and migrate Find BMS;
add native background transports and recovery; connect browser handoffs; validate
the complete user experience. Precise class layout, private schema format and
visual styling remain implementation decisions within these contracts.

## Platform references

- [Apple background downloads](https://developer.apple.com/documentation/foundation/downloading-files-in-the-background)
- [Apple background URLSession lifecycle and force-quit behavior](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/background(withidentifier:))
- [Apple continued processing and user-visible progress](https://developer.apple.com/videos/play/wwdc2025/227/)
- [Android user-initiated data-transfer jobs](https://developer.android.com/develop/background-work/background-tasks/uidt)

Platform API behavior must be checked against the installed SDK during implementation;
browser-session compatibility and genuine locked-device execution require device evidence.
