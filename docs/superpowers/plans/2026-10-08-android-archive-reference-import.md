# Android Archive Reference Import Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Offer direct SAF references or extraction for non-solid archives, and explain mandatory extraction for solid archives.

**Architecture:** Preserve logical archive identities while resolving native reader access through owned descriptors. Android owns URI registration and permissions; existing archive readers, indexing, and extraction consume the resolved source.

**Tech Stack:** C++23, JNI, Android Java/SAF, existing miniz/7-Zip/libarchive/unarr readers.

**Spec:** `docs/superpowers/specs/2026-10-08-android-archive-reference-import-design.md` (approved).

## Global Constraints

- Direct import copies neither the archive nor its assets; small reference metadata and index caches are allowed.
- Never delete an external source; never persist a descriptor number as chart identity.
- Keep local/iOS archive access behavior and existing extraction safety checks.
- No worktrees or deployment; work in the current checkout and push verified changes to its upstream.

## Review Focus

- Independent offsets during concurrent reads and cache eviction (task 1).
- Missing/revoked sources must not purge chart records during refresh (task 2).
- Restart restores durable references before playback (task 2).
- Transient grants and non-seekable providers never offer an unusable direct import (task 3).
- Activity destruction releases dialog workers and provisional resources (task 3).

### Task 1: Archive source access

**Files:** Create `src/archive/ArchiveSourceAccess.h`, `src/archive/ArchiveReferenceRegistry.h`, and `tests/archive_source_access_tests.cpp`; modify `ArchiveFile.cpp`, `ArchiveSourceIdentity.h`, and native test wiring.

**Interfaces:** `archive_source::Access resolve(const std::filesystem::path&)`, with readable `path`, retained `owner`, and `error`; `Registry::registerSource(path, uri)`, `Registry::acquire(path)`, `Registry::remove(path)`, using an injected URI opener.

- [x] Test local passthrough, unavailable reference failure, one open for repeated/concurrent acquisition, independent file cursors, and safe eviction with active leases.
- [x] Run the new test and confirm failure before implementation.
- [x] Implement a bounded descriptor cache and route all archive open/stat operations through the adapter. Keep logical names for format selection and cache identity.
- [x] Add real ZIP/RAR/7z source-resolution cases to archive tests; run source and archive concurrency tests.

### Task 2: Android reference registration and library tasks

**Files:** Modify `AndroidNatives.{h,cpp}`, `ChartLibraryScanner.cpp`, `library/ChartLibraryPlatform.cpp`, `library/ChartLibraryTask{Types,Service}.{h,cpp}`, and `library/ChartLibraryOperations.cpp`; add focused task/scanner cases.

**Interfaces:** JNI archive inspection registers a logical source and returns non-solid/solid/error/stream-only status; finishing an import carries a retained URI only for direct use. Stored chart entries restore registered references before reads.

- [x] Test source state comes from the archive, not a reference; direct import survives metadata reload; unavailable roots preserve indexed records; cleanup never removes originals.
- [x] Run new cases to confirm failure.
- [x] Implement reference registration, descriptor validation/reopening, scanner source state, explicit keep/extract task routing, and ownership-aware cleanup.
- [x] Run scanner, task-service, archive, and storage-identity tests.

### Task 3: Import choice and permission lifecycle

**Files:** Add focused Android archive import helper(s) and Java tests; modify `AsoBMaShowActivity.java`, Android strings, and relevant workflow fixtures.

**Interfaces:** Choice values are use archive, extract, and cancel. Inspection and import run on the worker; dialog presentation and dismissal run on the UI thread.

- [x] Test non-solid offers both actions; solid offers extraction only; stream-only/transient sources cannot be retained; cancellation/destruction completes pending choice and cleans temporary ownership.
- [x] Run failing Java behavioral tests.
- [x] Implement inspection before copying, permission persistence, user choice, source reuse for extraction, stream staging fallback, and lifecycle cancellation.
- [x] Run Java behavioral/workflow tests and native import integration cases.

### Task 4: Verification and delivery

- [x] Build desktop `main` and relevant targets in one build invocation; run relevant CTest cases with `-j 6`.
- [x] Run `scripts/android_firebase_deploy.sh --build-only` and exercise direct/extracted imports on Android, including restart and storage usage.
- [x] Run `IOS_RELEASE_BUILD_JOBS=6 scripts/ios_release_verify.sh` for shared-reader changes.
- [x] Review the diff and source ownership paths; fix demonstrated failures, then commit and push task changes.
