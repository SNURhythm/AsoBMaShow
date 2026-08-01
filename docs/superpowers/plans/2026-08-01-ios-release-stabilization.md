# iOS First-Release Stabilization Implementation Plan

> **Execution note:** Follow the test-driven steps in order. Do not upload to
> Firebase or TestFlight. Keep `NSAllowsArbitraryLoads`, version `0.0.1`, and
> the iOS 14 minimum. Do not add privacy manifests.

**Goal:** Fix the scoped iOS release blockers and high-risk defects while
preserving the approved product and distribution constraints.

**Architecture:** Introduce small testable policies for transaction completion,
IR credential/transport safety, decoded-image ownership, media budgets, and
video decode state. Keep platform services behind injected interfaces. Separate
verification from distribution and validate the resulting app/IPA locally.

**Primary languages/tools:** C++23, Objective-C++, SQLite, FFmpeg, SDL2, bgfx,
CMake/CTest, Xcode 26, Python `unittest`, Ruby/Fastlane, GitHub Actions.

**Design:** `docs/superpowers/specs/2026-08-01-ios-release-stabilization-design.md`

## Global Constraints

- Never edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.
- Use `scripts/ios_firebase_deploy.sh --build-only` for local iOS compile
  verification; never invoke an upload lane during this plan.
- Keep `NSAllowsArbitraryLoads = true` in `Info.plist`.
- Keep `MARKETING_VERSION = 0.0.1` and `IPHONEOS_DEPLOYMENT_TARGET = 14.0`.
- Do not add `PrivacyInfo.xcprivacy`.
- Add a failing regression test before each production change.
- Commit cohesive milestones only after their focused tests pass.

## Task 1: Fail Chart Migration at the Commit Boundary

**Files:**

- Modify: `src/repositories/ChartRepository.cpp`
- Create: `src/repositories/ChartRepositoryMigrationTestAccess.h`
- Modify: `tests/chart_repository_tests.cpp`
- Modify: `CMakeLists.txt` only if the new test-access header needs explicit
  project membership

### Steps

1. Add a test-only release-failure seam modeled on the repository's existing
   migration test-access patterns. The seam fires immediately before or at the
   savepoint `RELEASE` operation and is inert unless explicitly installed.
2. Add a regression test that creates a real repository/database, captures the
   durable version/rebuild marker and library revision, injects a failed
   release, and asserts the migration reports failure without advancing any of
   those values.
3. Run the focused test and confirm the new case fails for the current false
   success behavior:

   ```bash
   cmake --build cmake-build-debug --target chart_repository_tests -j 6
   ctest --test-dir cmake-build-debug -R '^chart_repository$' --output-on-failure
   ```

4. Check the `RELEASE` result. On failure, attempt bounded savepoint cleanup,
   return false, and skip the success log, revision bump, and completion flag.
5. Extend the test to remove the seam and assert a clean retry commits exactly
   once.
6. Rerun the focused test and commit:

   ```bash
   git add src/repositories/ChartRepository.cpp \
     src/repositories/ChartRepositoryMigrationTestAccess.h \
     tests/chart_repository_tests.cpp CMakeLists.txt
   git commit -m "fix: fail closed on chart migration commit"
   ```

## Task 2: Enforce HTTPS for Authenticated IR Only

**Files:**

- Modify: `src/ir/IrProfileSettings.h`
- Modify: `src/ir/IrProfileSettings.cpp`
- Modify: `src/ir/IrSettingsPresentation.cpp`
- Modify: `src/ir/tachi/TachiDriver.cpp`
- Modify: `src/ir/tachi/TachiDriver.h` if a shared guard is exposed
- Modify: `tests/ir_settings_presentation_tests.cpp`
- Modify: `tests/tachi_driver_tests.cpp`

### Steps

1. Add tests proving normalized HTTP origins remain valid for anonymous/public
   operations while authenticated settings are not actionable on HTTP.
2. Add driver tests that install a recording HTTP client and assert HTTP
   submissions, private history/account operations, and any request with a
   bearer token fail before the client is called. Assert anonymous public
   ranking resolution remains allowed and HTTPS behavior is unchanged.
3. Run the relevant targets and confirm the new tests fail:

   ```bash
   cmake --build cmake-build-debug --target \
     ir_settings_presentation_tests tachi_driver_tests -j 6
   ctest --test-dir cmake-build-debug \
     -R '^(ir_settings_presentation|tachi_driver)$' --output-on-failure
   ```

4. Add a single origin-security predicate next to origin normalization. Use it
   from settings presentation to disable authenticated controls and explain the
   correction without rewriting the stored origin.
5. Add the independent request-layer guard before headers or payloads containing
   credentials/private score data are built or dispatched.
6. Verify `ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist` still contains
   `NSAllowsArbitraryLoads = true`.
7. Rerun focused tests and commit:

   ```bash
   git add src/ir tests/ir_settings_presentation_tests.cpp \
     tests/tachi_driver_tests.cpp
   git commit -m "fix: require HTTPS for authenticated IR"
   ```

## Task 3: Route iOS IR Credentials Through Keychain

**Files:**

- Modify: `src/ir/IrCredentialStore.h`
- Modify: `src/ir/IrCredentialStore.cpp`
- Create: `src/ir/IrCredentialBackend.h`
- Create: `src/ir/IrCredentialMigration.h`
- Create: `src/ir/IrCredentialMigration.cpp`
- Create: `src/ir/IosKeychainCredentialBackend.h`
- Create: `src/ir/IosKeychainCredentialBackend.mm`
- Modify: `src/PlayerProfileManager.cpp`
- Modify: `src/context.h`
- Modify: `src/scene/SettingsSceneIr.cpp`
- Modify: `src/scene/IrUploadsScene.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `tests/ir_credential_store_tests.cpp`
- Modify: `tests/player_profile_manager_tests.cpp`
- Create: `tests/ir_credential_migration_tests.cpp`
- Create: `tests/ios_keychain_credential_tests.mm`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj` only for
  necessary platform membership/link settings; preserve synchronized-group
  behavior

### Steps

1. Characterize current file-backed load/save/remove behavior and add a backend
   contract fake that can fail the Nth write/read/delete without storing secret
   text in diagnostics.
2. Add failing migration tests for full success, partial write, verification
   failure, retry, legacy-file deletion ordering, malformed legacy input, and a
   missing legacy file.
3. Add failing profile lifecycle tests proving duplication/export/import omit
   credentials and profile deletion removes all provider entries for that
   profile identity.
4. Build and run the focused portable tests to establish red:

   ```bash
   cmake --build cmake-build-debug --target \
     ir_credential_store_tests player_profile_manager_tests \
     ir_credential_migration_tests -j 6
   ctest --test-dir cmake-build-debug \
     -R '^(ir_credential_store|player_profile_manager|ir_credential_migration)$' \
     --output-on-failure
   ```

5. Extract a backend interface while keeping the current file implementation
   as the default outside iOS. Make migration orchestration depend on that
   interface and keep format validation/redaction centralized.
6. Implement the iOS backend with Security.framework. Namespace service/account
   values by app, stable profile identity, and provider ID. Use
   `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`. Treat not-found deletion
   as success and never include API-key bytes in errors.
7. Wire the current profile/context/scenes to the selected credential service.
   On iOS, run the transactional legacy migration before enabling authenticated
   IR. A partial failure keeps the file and disables authenticated IR for the
   session.
8. Add the Objective-C++ simulator test for real Keychain save/read/replace/
   delete and accessibility attributes. Keep it out of non-Apple builds.
9. Run portable tests, then the simulator Keychain test using the iOS debugger
   build workflow.
10. Confirm no credential code path was added to profile archive payloads and
    commit:

   ```bash
   git add src/ir src/PlayerProfileManager.cpp src/context.h \
     src/scene/SettingsSceneIr.cpp src/scene/IrUploadsScene.cpp \
     tests/ir_credential_store_tests.cpp \
     tests/ir_credential_migration_tests.cpp \
     tests/ios_keychain_credential_tests.mm \
     tests/player_profile_manager_tests.cpp CMakeLists.txt \
     ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
   git commit -m "fix(ios): protect IR credentials with Keychain"
   ```

## Task 4: Make YUV420 Layout Safe for Odd Dimensions

**Files:**

- Create: `src/video/VideoFrameLayout.h`
- Create: `src/video/VideoFrameLayout.cpp` if non-inline implementation is useful
- Modify: `src/video/VideoPlayer.cpp`
- Modify: `src/video/VideoPlayer.h`
- Create: `tests/video_frame_layout_tests.cpp`
- Modify: `src/video/CMakeLists.txt`
- Modify: `CMakeLists.txt`

### Steps

1. Add table-driven tests for zero/negative/oversized values, 1x1, even sizes,
   odd width, odd height, and multiplication overflow. Assert luma/chroma
   dimensions, pitches, and byte counts.
2. Build/run the new test and confirm odd layouts fail against the current floor
   division behavior.
3. Implement the checked layout helper with ceiling chroma division and checked
   multiplication.
4. Replace all VideoPlayer plane allocation/copy/texture dimension calculations
   with the helper's single result. Invalid layout aborts that visual cleanly.
5. Run the test and a compile of `jukebox_restore_tests`:

   ```bash
   cmake --build cmake-build-debug --target \
     video_frame_layout_tests jukebox_restore_tests -j 6
   ctest --test-dir cmake-build-debug \
     -R '^(video_frame_layout|jukebox_restore)$' --output-on-failure
   ```

6. Commit:

   ```bash
   git add src/video tests/video_frame_layout_tests.cpp CMakeLists.txt
   git commit -m "fix: size odd YUV420 frames safely"
   ```

## Task 5: Drain FFmpeg Without Dropping Packets or Tail Frames

**Files:**

- Create: `src/video/VideoDecodeState.h`
- Create: `src/video/VideoDecodeState.cpp`
- Modify: `src/video/VideoPlayer.h`
- Modify: `src/video/VideoPlayer.cpp`
- Create: `tests/video_decode_state_tests.cpp`
- Create or reuse: `tests/fixtures/video/` delayed-frame fixture
- Modify: `CMakeLists.txt`

### Steps

1. Add deterministic state-machine tests using an injected codec/demux adapter:
   receive-before-send, send `EAGAIN`, retained packet, full output buffer,
   demux EOF, one null flush, delayed frames, decoder EOF, cancellation, and
   seek reset.
2. Add one real FFmpeg fixture test that verifies final frame count/PTS and seek
   after EOF. Store a small deterministic fixture in the repo; do not depend on
   a host `ffmpeg` binary during tests.
3. Run the new target and confirm failures expose the current early-EOF path.
4. Implement the explicit state object and make `predecodeFrames()` advance it
   without reading a new packet until the current decoder state permits it.
5. Reduce `maxBufferSize` to three and cap `recyclePool` at two. Ensure buffer
   waits wake on cancellation, seek, suspension changes, and teardown.
6. Run focused tests plus `jukebox_restore_tests`, then commit:

   ```bash
   git add src/video tests/video_decode_state_tests.cpp tests/fixtures/video \
     CMakeLists.txt
   git commit -m "fix: drain video decoders through EOF"
   ```

## Task 6: Bound Artwork Cache and Async Ownership

**Files:**

- Create: `src/view/DecodedImageCache.h`
- Create: `src/view/DecodedImageCache.cpp`
- Create: `src/view/ImageDecodeCoordinator.h`
- Create: `src/view/ImageDecodeCoordinator.cpp`
- Modify: `src/view/ImageView.h`
- Modify: `src/view/ImageView.cpp`
- Modify: `src/view/CMakeLists.txt`
- Create: `tests/decoded_image_cache_tests.cpp`
- Create: `tests/image_decode_coordinator_tests.cpp`
- Modify: `CMakeLists.txt`

### Steps

1. Add cache tests for exact RGBA byte accounting, LRU touches, replacement,
   pin/unpin, one-item-over-budget behavior, and clearing unpinned entries.
2. Add coordinator tests for two-worker limit, priority ordering, duplicate
   consumers, ticket cancellation, queued orphan removal, in-flight orphan
   discard, stale completion, ready-result accounting, and shutdown.
3. Build and run the new targets to establish red.
4. Implement the cache as a UI-thread-owned byte LRU. Set the iOS budget to
   64 MiB and choose a bounded non-iOS default rather than retaining unbounded
   behavior.
5. Implement the shared two-worker coordinator with consumer tickets and a
   single deduplicated work record per source/size key.
6. Change `ImageView` binding/destruction to acquire/release tickets and accept
   a completion only when its ticket and key are still current.
7. Pass requested render dimensions into decode/downsample and include them in
   the cache key. Preserve archived-thumbnail fallback behavior.
8. Run focused tests and existing image tests, then commit:

   ```bash
   cmake --build cmake-build-debug --target \
     decoded_image_cache_tests image_decode_coordinator_tests \
     image_fade_tests image_view_fade_tests -j 6
   ctest --test-dir cmake-build-debug \
     -R '^(decoded_image_cache|image_decode_coordinator|image_fade|image_view_fade)$' \
     --output-on-failure
   git add src/view tests/decoded_image_cache_tests.cpp \
     tests/image_decode_coordinator_tests.cpp CMakeLists.txt
   git commit -m "perf: bound asynchronous artwork decoding"
   ```

## Task 7: Add Central Low-Memory Eviction

**Files:**

- Modify: `src/main.cpp`
- Modify: `src/view/ImageView.h`
- Modify: `src/view/ImageView.cpp`
- Modify: `src/audio/Jukebox.h`
- Modify: `src/audio/Jukebox.cpp`
- Create: `tests/mobile_memory_pressure_tests.cpp`
- Modify: `CMakeLists.txt`

### Steps

1. Add a pure coordinator test that seeds pinned/unpinned artwork, ready/orphan
   decode work, active/idle BGA resources, and asserts one memory-pressure event
   clears only evictable state.
2. Run the new test to establish red.
3. Route `SDL_APP_LOWMEMORY` through one application callback that invokes the
   artwork and jukebox eviction APIs without blocking the event loop.
4. Keep active visuals pinned and make repeated low-memory notifications
   idempotent.
5. Run focused tests and commit.

## Task 8: Prepare BGA Players Before Playback

**Files:**

- Modify: `src/audio/Jukebox.h`
- Modify: `src/audio/Jukebox.cpp`
- Modify: `tests/jukebox_restore_tests.cpp`

### Steps

1. Add a failing production Jukebox test that requires every referenced video
   to be materialized after chart loading and before `play()`.
2. Verify the test fails because descriptor-only loading leaves zero prepared
   videos and defers decoder initialization until the event timestamp.
3. Materialize images and videos during visual reconciliation, propagating the
   chart-load cancellation token through archive/file reads and decoder setup.
4. Remove event-time materialization and the three-player eviction policy so
   scheduled activation performs only lookup plus seek/play.
5. Verify a four-ID chart retains all four prepared players across activation,
   proving no event-time loading or eviction occurs.
6. Preserve the existing three-frame decode buffer, two-frame recycle pool,
   render rectangles, offsets, suspension, playback-rate, restore, and audio
   lifecycle behavior.
7. Run focused tests plus audio lifecycle/mix regressions and commit:

   ```bash
   cmake --build cmake-build-debug --target \
     jukebox_restore_tests \
     audio_wrapper_lifecycle_tests audio_mix_tests \
     mobile_memory_pressure_tests -j 6
   ctest --test-dir cmake-build-debug \
     -R '^(foundation_av_jukebox_restore|foundation_av_audio_wrapper_lifecycle|foundation_av_audio_mix|mobile_memory_pressure_tests)$' \
     --output-on-failure
   git add src/audio/Jukebox.cpp src/audio/Jukebox.h \
     tests/jukebox_restore_tests.cpp
   git commit -m "fix: preload BGA before playback"
   ```

## Task 9: Align the iOS 14 Build Configuration

**Files:**

- Modify: `ios/Xcode/AsoBMaShow/Podfile`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`
- Modify: `scripts/ios_init.sh`
- Modify: `tests/ios_build_setup_tests.py`

### Steps

1. Add failing setup tests asserting:
   - app Debug/Release target is 14.0;
   - Podfile platform is 14.0;
   - generated bgfx, SDL2, and SDL2_ttf iOS build configurations are patched to
     14.0 by `ios_init.sh`;
   - `MARKETING_VERSION` remains 0.0.1;
   - `NSAllowsArbitraryLoads` remains true; and
   - no `PrivacyInfo.xcprivacy` is introduced.
2. Run `python3 tests/ios_build_setup_tests.py` and confirm the new dependency
   assertions fail.
3. Make `ios_init.sh` patch only the necessary generated/third-party iOS build
   settings, idempotently. Do not grow synchronized-group membership exceptions
   for normal source files.
4. Set the Podfile platform to 14.0 and keep the main app target/version
   unchanged.
5. Run setup tests twice around `scripts/ios_init.sh` to verify idempotence, then
   run plist lint.
6. Commit the setup changes.

## Task 10: Separate Verification From Distribution and Serialize TestFlight

**Files:**

- Modify: `ios/Xcode/AsoBMaShow/fastlane/Fastfile`
- Modify: `.github/workflows/mobile-beta-deploy.yml`
- Create: `scripts/ios_release_verify.sh`
- Create: `tests/ios_release_workflow_tests.py`
- Modify: `tests/ios_build_setup_tests.py`

### Steps

1. Add static workflow/Fastfile tests that fail unless:
   - verification has no upload action;
   - Firebase and TestFlight are explicit lanes;
   - PRs cannot route to TestFlight;
   - TestFlight depends on verification while Firebase PR iteration does not;
   - TestFlight has one non-canceling concurrency group; and
   - the serialized lane allocates `latest_testflight_build_number + 1`.
2. Add `scripts/ios_release_verify.sh` tests/dry-run support so CI can prove the
   command sequence without invoking signing or distribution.
3. Establish red with the Python tests.
4. Split Fastlane lanes and explicit workflow jobs/conditions. Preserve the
   existing PR-to-`develop` Firebase path and its DerivedData/archive object
   behavior.
5. Make verification run the core required tests, iOS setup tests, and unsigned
   iOS build-only path before TestFlight is eligible. Keep the Firebase PR lane
   independent for fast iteration.
6. Run Python tests and shell syntax checks; never invoke a distribution lane.
7. Commit release-engineering changes.

## Task 11: Add Deterministic iOS Artifact Auditing

**Files:**

- Create: `scripts/ios_artifact_audit.sh`
- Create: `tests/ios_artifact_audit_tests.py`
- Modify: `scripts/ios_release_verify.sh`
- Modify: `tests/ios_build_setup_tests.py`

### Steps

1. Create minimal positive/negative synthetic app fixtures in a temporary test
   directory. Add failing tests for version, minimum OS, SDK, bundle ID, device
   family, icon metadata, permissions, ATS exception, architecture, dependency
   resolution, unwanted files, secrets, and conditional signature verification.
2. Explicitly assert the gate does not require a privacy manifest.
3. Implement the audit with `plutil`, `file`/`lipo`, `otool`, and `codesign`
   using resolved, quoted paths. Do not extract into or modify the source tree.
4. Run Python tests and audit the existing IPA as characterization.
5. After a new build is available, audit its app/IPA and record any signing-only
   checks as pending if the build is unsigned.
6. Commit the script and tests.

## Task 12: Update Shipped Privacy and Release Documentation

**Files:**

- Modify: `PRIVACY_POLICY.md`
- Create: `docs/ios-first-release-checklist.md`
- Modify: `README.md` if its workflow badge or release wording is stale

### Steps

1. Add a documentation test or focused grep assertions for the required policy
   facts: iOS Keychain, device-only accessibility, transactional legacy
   migration, HTTPS-only authenticated IR, and continued HTTP-capable anonymous
   difficulty-table loading.
2. Remove statements that credentials remain plaintext or that authenticated
   HTTP is permitted.
3. Create the manual App Store checklist covering privacy labels, age rating,
   URLs, export compliance, content rights/review notes, screenshots, signed
   archive validation, and physical iPhone/iPad smoke.
4. State that privacy manifests are intentionally omitted from this release
   gate, without claiming Apple never requires them.
5. Run Markdown/link/grep checks and commit.

## Task 13: Full Automated Verification

### Steps

1. Check the worktree and inspect every diff for accidental unrelated changes:

   ```bash
   git status --short
   git diff --check
   git diff --stat develop...HEAD
   ```

2. Build and run the full desktop regression suite:

   ```bash
   cmake --build cmake-build-debug --target main -j 6
   cmake --build cmake-build-debug -j 6
   ctest --test-dir cmake-build-debug --output-on-failure
   ```

3. Run iOS setup/release tests:

   ```bash
   python3 tests/ios_build_setup_tests.py
   python3 tests/ios_release_workflow_tests.py
   python3 tests/ios_artifact_audit_tests.py
   ```

4. Run the unsigned iOS build-only path without upload:

   ```bash
   scripts/ios_firebase_deploy.sh --build-only
   ```

5. Run the artifact audit against the produced app/IPA. Verify version 0.0.1,
   iOS 14, ATS arbitrary loads retained, signatures where applicable, and no
   privacy-manifest requirement.

## Task 14: iPhone/iPad Simulator and Performance Verification

### Steps

1. Use the iOS debugger workflow to show defaults, select an available iPhone
   simulator/runtime, build, install, and launch the exact simulator app.
2. Capture logs and screenshots for first launch, profile readiness, library
   scrolling, chart launch, BGA playback/seek, background/foreground, and IR
   HTTP/HTTPS states. Treat main-thread checker, crash, and fatal runtime log
   entries as failures.
3. Repeat core launch/layout/navigation smoke with an iPad simulator.
4. For each focused performance flow, temporarily link ETTrace only into the
   simulator debug app, collect UUID-matched dSYMs, and preserve processed
   `output_*.json` immediately:
   - artwork-heavy library scrolling; and
   - BGA-heavy playback plus one seek.
5. Analyze symbolicated first-party stacks and record run conditions, artifacts,
   hotspots, and caveats. Remove all temporary ETTrace wiring afterward and
   prove the worktree contains no ETTrace integration.
6. If signing credentials are locally available, build and inspect a signed
   archive without calling any upload lane. Otherwise, leave signed export and
   physical-device smoke explicitly pending in the release checklist.
7. Run the verification-before-completion checklist, update the audit with the
   final iOS verdict, and make the final documentation-only commit if needed.
