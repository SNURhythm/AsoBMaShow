# Android device fixes implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix the three failures reproduced on the connected Android 13 phone.

**Architecture:** Use a synchronized, app-container-scoped regular-file rename fallback on Android filesystems without exclusive rename. Preserve storage journal identities and replay publication/restore semantics. Launch the Files provider root, whose intent was verified on the phone, instead of explicitly launching a directory document.

**Tech Stack:** C++23, Android JNI/Java, CMake/CTest, ADB.

**Spec:** `docs/testing/android-device-2026-10-23.md` (confirmed failures).

## Global Constraints

- Work on `fix/android-device-storage-files` in the current checkout; no worktree.
- Preserve no-clobber publication, bounded memory, and existing crash recovery.
- Android fallback applies only within app-owned storage, with the DocumentsProvider mutation monitor held; external roots additionally require API 30+. Unsupported arbitrary external folders and older external roots fail closed.
- Use focused subsystem tests and a signed build-only Android build; no distribution.
- Restore device settings and remove only generated test fixtures.

## Review Focus

- An occupied destination, including a dangling symlink, must remain untouched.
- Publication outside app-owned roots must not use a check-then-rename fallback.
- Concurrent native publishers and provider mutations must serialize.
- Interrupted replay installation and quarantine restore must remain recoverable.
- Files navigation must use a system root-capable handler and preserve provider access.

### Task 1: Repair Files entry point

**Files:** `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java`, `tests/android_documents_activity_tests.py`, `tests/java/AndroidDocumentsActivityFixture.java`.

**Interfaces:** Keep `openDocumentsFolder(): String`; launch the existing root intent with the selected system component.

- [x] Extend the Activity test to require a root URI and root MIME type; run `python3 tests/android_documents_activity_tests.py` and observe failure.
- [x] Reuse the root intent for launch, retaining canonical BMS validation and system-handler selection.
- [x] Run the same test, expecting all scenarios to pass; commit.

### Task 2: Repair Android regular-file publication

**Files:** new `src/platform/AndroidFilePublication.h`, `src/AndroidNatives.cpp`, `src/storage/StoragePlatform.cpp`, `src/replay/ReplayFileStore.cpp`, new `tests/android_file_publication_tests.cpp`, `CMakeLists.txt`, `cmake/TestGroupCatalog.cmake`.

**Interfaces:** `android_file::renameNoReplace(from, to, std::error_code&)` is Android-only and acquires the provider monitor. A host-testable `renameUnderMutationLock(from, to, roots, error)` validates scope, regular source, missing destination and performs one rename using pinned parent descriptors. Callers retain responsibility for directory fsync.

- [x] Add behavioral coverage for successful publication preserving inode/bytes; existing regular file/dangling link/directory refusal; unsafe source; outside-root refusal; serialized competing publishers. Confirm missing implementation fails before adding it.
- [x] Implement the scoped fallback and JNI monitor wrapper. Use it after unsupported exclusive rename in storage, and instead of hard-link installation/restoration on Android replay paths. Keep desktop behavior.
- [x] Build/run the new test plus storage-file transaction and replay-file-store tests. Expected: all pass, including existing recovery/collision cases.
- [x] Run affected library/replay/results/Android groups and build the Android release APK with manual version code 4. Retest folder FLAC/H.264, replay save/playback, and Files navigation on the phone; commit changes and evidence.

### Task 3: Review and publish branch

- [x] Obtain a fresh whole-branch code review; address material findings with regression coverage.
- [x] Update the device report with attribution, fix results, and remaining limitations.
- [ ] Verify diff and clean working tree, then push the new branch. Do not merge or deploy.
