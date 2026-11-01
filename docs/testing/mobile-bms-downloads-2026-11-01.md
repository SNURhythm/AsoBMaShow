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

Final mobile packaging, installation, and runtime observations are recorded as
verification proceeds. The branch is being pushed before Android device testing
at the user's request, to allow iOS testing in parallel.

Android restricted-file release APK packaging passed. It was installed with
`adb install -r` on the connected Samsung Android 13 device using its existing
version code 5 and matching signing certificate. Installation succeeded without
uninstalling or clearing data. Device interaction tests have not started at this
intermediate push.
