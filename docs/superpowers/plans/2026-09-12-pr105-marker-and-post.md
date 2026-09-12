# PR 105 Marker and POST Review Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Address the two unresolved PR findings without hiding valid charts or leaving cancelled Android metadata POSTs blocked.

**Architecture:** Validate incomplete extraction ownership against both a bounded marker reader and recovery journal. Extend the Android checkpoint lifecycle to POST, repeatedly disconnecting the active connection until the cancelled worker finishes. These changes have independent write scopes.

**Tech Stack:** C++23, SQLite, JNI, Java HTTP connections, CMake/CTest, Gradle.

**Spec:** [Scanner ownership review](https://github.com/SNURhythm/AsoBMaShow/pull/105#discussion_r3996425363), [Android POST review](https://github.com/SNURhythm/AsoBMaShow/pull/105#discussion_r3996425365).

## Global Constraints

- Work on the current branch; no new worktree or deployment.
- Preserve unrelated changes, source archive safety, response limits and cancellation semantics.
- Use real behavior regressions, preserve surrounding formatting and leave the parser amalgamation untouched.
- Serialize shared CMake builds and CTest while parallel agents are active.

### Task 1: Validate Incomplete Extraction Ownership

**Files:** `src/ChartLibraryScanner.cpp`, `src/ArchiveFile.h`, `src/ArchiveFile.cpp`, `tests/chart_library_scanner_tests.cpp`, `tests/archive_unzip_operation_tests.cpp`, `tests/unzip_marker_io_tests.py`, its C++ fixtures and `CMakeLists.txt`.

**Interfaces:** Consume `Session::LoadUnzipRecovery()` and `archive_file::unzipFolderHasMatchingIncompleteMarker(folder, archivePath, archiveKey)`. Keep scanner public APIs unchanged.

- [x] Reproduce a successful scan omitting a valid chart beside an unowned marker; include absent, unrelated and mismatched recovery records and parent/folder/chart scan roots.
- [x] Replace filename-only checks with optional ownership results. Read the journal when a regular marker is encountered so records created during traversal are visible. Require folder identity and matching marker contents; propagate unavailable ownership information as an unhealthy traversal.
- [x] Retain real interrupted-extraction suppression and verify nested marker names in a successfully extracted archive do not prevent indexing after source deletion.
- [x] Run the focused suites:

```sh
cmake --build cmake-build-debug --target chart_library_scanner_tests archive_unzip_operation_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(chart_library_scanner_tests|archive_unzip_operation_tests|unzip_marker_io_tests|android_metadata_post_cancellation_tests)$' -j 6
```

### Task 2: Cancel Android Metadata POSTs

**Files:** `src/AndroidNatives.h`, `src/AndroidNatives.cpp`, `src/bms_search/DownloadSupport.cpp`, `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java`, related Android and transport tests.

**Interfaces:** Extend `PostURLTextAndroid` with the existing `AndroidDownloadCheckpoint` callback type, following `DownloadURLTextAndroid`; forward a checkpoint token to Java `postUrlText`.

- [x] Reproduce cancellation while a POST response is stalled, without waiting for the 180-second production timeout.
- [x] Register/unregister the native checkpoint bridge for the complete Java call lifetime, propagate the caller's cancellation predicate and disconnect the active POST connection on cancellation.
- [x] Verify success, cancellation and cleanup with focused native/Java behavior tests, preserving response and redirect constraints.

### Task 3: Integrate and Respond

- [x] Independently review both changes and resolve actionable findings.
- [x] Rebuild affected desktop targets and run all CTest tests in parallel; run the Android build-only check for the changed JNI/Java bridge.

```sh
cmake --build cmake-build-debug -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6
scripts/android_firebase_deploy.sh --build-only
IOS_RELEASE_BUILD_JOBS=6 scripts/ios_release_verify.sh
```

**Delivery:** Commit and push only the task changes, then reply in each review thread with the pushed commit and verified evidence before resolving it.

## Review Follow-ups

- The initial scanner regression reproduced a successful scan with zero indexed charts despite a valid BMS beside an unowned marker. Ownership tests cover missing, unrelated, wrong-key and wrong-source recovery records; parent, directory and file roots; live journal registration; strict scans and failed journal reads.
- Independent review found an unreadable owned marker was indistinguishable from readable invalid contents. A real permission-denied regression reproduced the failure. The shared bounded marker reader now optionally reports filesystem/open/read errors so the scanner cannot publish incomplete charts when ownership cannot be read.
- A real macOS `funopen`/DYLD-interposed read-error regression then reproduced three cases where libc++ `ifstream` treated `EIO` as EOF without setting `badbit`. The reader now opens once with stdio, checks `ferror`, preserves the two-line 64 KiB bound and rejects embedded NULs. All three read-error cases pass alongside readable/malformed marker and complete-marker compatibility cases.
- Android tests exercise the production transport, JNI and Java methods with controlled stalled connection boundaries. The monitor must keep disconnecting until the worker finishes: Android can ignore a disconnect issued before connection initialization. A deterministic regression models that early no-op rather than making cancellation sticky in the fake connection.
- Final focused CTest run passed all four suites: scanner ownership, real archive operations, marker I/O failures and Android metadata POST cancellation. The JNI fixture supports macOS/Linux JDK discovery and explicitly skips unsupported or missing toolchains; separate tests cover that selection logic.
- Final independent scanner and Android reviews reported no unresolved actionable findings within these two fixes. Marker tests were independently rerun successfully; broad build verification remains a separate integration step.

## Final Verification

- Full desktop build passed, including `main` and all test targets.
- Final parallel CTest run passed **359/359** in 83.59 seconds. The first run, concurrent with iOS compilation, failed `visual_catch_up`, `ir_submission_service_tests` and `image_view_fade_tests`; all three passed without code changes after compilation finished, followed by the clean full-suite rerun. No unrelated tests or timeouts were changed.
- Android `firebaseRelease` build-only passed, compiling the actual NDK JNI bridge and Java activity. The deterministic cancellation tests use controlled HTTP connections on a host JVM, not an Android device network test.
- `scripts/ios_release_verify.sh` passed: 66 native checks, 88 Python release-contract checks, the unsigned iOS build and artifact audit.
- No Firebase upload, TestFlight release or other deployment was performed.
