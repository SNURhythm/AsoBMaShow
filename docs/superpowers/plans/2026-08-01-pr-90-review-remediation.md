# PR #90 Review Remediation Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Resolve every actionable, unresolved review on PR #90 before merging `develop` into `main`, with Android storage correctness and fatal lint enforcement while preserving the existing iOS release constraints and eager BGA loading.

**Architecture:** Keep the public application-data root unchanged for charts and profiles, but route transient SQLite preflight snapshots to Android's private cache and route IR secrets to Android's private files directory. Reuse the existing verified legacy-credential migration to move plaintext credentials out of external storage. Treat Android lint as a deployment gate, fix app-owned findings, port the narrow SDL 2.32 receiver compatibility fix into the pinned SDL fork, and suppress only permission diagnostics for optional upstream SDL capabilities the app does not enable.

**Tech Stack:** C++23, JNI/SDL Android storage bridges, Java/Android SDK 28-35, Gradle Android lint, Python `unittest`, CMake/CTest, GitHub Actions.

---

## Task 1: Add release-contract regression coverage

**Files:**

- Create: `tests/android_release_workflow_tests.py`

1. Add contract tests that require Android fatal lint, the lint configuration, a pre-deploy Firebase lint step, notification permission handling, explicit URI read grants, the SDL API-33 receiver compatibility wrapper, Android private-cache SQLite snapshots, and Android private-files credential storage.
2. Run `python3 -m unittest tests/android_release_workflow_tests.py -v` and confirm the new tests fail against the reviewed code.
3. Keep assertions scoped to durable release invariants rather than formatting details.

## Task 2: Route SQLite preflight snapshots to private Android cache

**Files:**

- Modify: `src/repositories/SqliteRAII.h`

1. Add a platform-specific temporary-root resolver in the SQLite preflight helper.
2. On Android, use `GetAndroidCacheDir()` and fail closed with a useful diagnostic if no private cache path is available.
3. On other platforms, preserve `std::filesystem::temp_directory_path(error)` behavior.
4. Run the Android release-contract test and the focused SQLite/profile tests.

## Task 3: Move Android IR credentials to private files and migrate existing secrets

**Files:**

- Modify: `src/AndroidNatives.cpp`
- Modify: `src/ir/IrCredentialBackend.h`
- Modify: `src/ir/IrCredentialBackend.cpp`
- Modify: `tests/ir_credential_migration_tests.cpp`

1. Add a failing C++ regression proving a migration-enabled file backend copies credentials from a legacy external profile path into a separate private root, verifies them, and removes the legacy plaintext file.
2. Make the reusable file backend configurable for whether legacy-file migration is required.
3. On Android, construct it from `GetAndroidInternalFilesDir()`, require legacy migration, and return no backend if protected storage is unavailable.
4. Remove the external-storage fallback from `GetAndroidInternalFilesDir()` so the credential path fails closed rather than silently remaining public.
5. Preserve desktop file storage and iOS Keychain behavior.
6. Run `ir_credential_migration_tests` and the Android release-contract test.

## Task 4: Make Android lint a blocking pre-deployment gate

**Files:**

- Modify: `android/app/build.gradle`
- Create: `android/app/lint.xml`
- Modify: `android/app/src/main/AndroidManifest.xml`
- Modify: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java`
- Modify: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowMusicService.java`
- Modify in SDL submodule: `SDL/android-project/app/src/main/java/org/libsdl/app/HIDDeviceManager.java`
- Modify: `.github/workflows/mobile-beta-deploy.yml`

1. Port SDL 2.32's API-33 `registerReceiverCompat` implementation using `Context.RECEIVER_NOT_EXPORTED` for the USB and Bluetooth dynamic receivers.
2. Declare `POST_NOTIFICATIONS`, request it when native music playback starts, and guard non-foreground `NotificationManager.notify` calls when permission is unavailable.
3. Pass an explicit read-grant constant to persisted URI permission calls, guarded by the actual grant flags.
4. Add a narrow lint configuration suppressing only `MissingPermission` in optional upstream SDL Bluetooth/capture implementations; do not suppress app-owned notification or receiver findings.
5. Enable `abortOnError`, point Gradle at the lint configuration, and run `lintFirebaseDebug` before the Android Firebase deployment command.
6. Run the release-contract test and `./gradlew lintFirebaseDebug`; require zero lint errors.

## Task 5: Self-review, verify, and publish the PR-head fixes

**Files:**

- Review all changed files and both repository worktrees.

1. Review diffs for storage fallback, migration idempotence, Android API compatibility, notification behavior, and unrelated changes.
2. Run focused Python and C++ tests, Android lint, the local desktop compile, and an Android build-only check if signing configuration is available. Do not upload a build.
3. Commit and push the SDL compatibility fix to the pinned `SNURhythm/SDL` branch, then commit the updated submodule pointer and main-repository fixes to `develop`.
4. Re-fetch PR #90 review threads by stable thread/comment identifiers, including full bodies, and confirm no new actionable feedback was missed. Do not resolve or reply to threads without explicit authorization.
5. Report the pushed commits, verification evidence, remaining warnings/checks, and PR URL.
