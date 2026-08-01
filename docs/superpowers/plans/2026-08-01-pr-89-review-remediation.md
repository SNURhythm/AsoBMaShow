# PR #89 Review Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Address every actionable review thread on PR #89 without reintroducing event-time BGA loading or weakening the iOS 0.0.1 release contract.

**Architecture:** Keep BGA timing deterministic by testing the eager-preload invariant and its replay-tail consequence. Strengthen release boundaries in the artifact scanner and TestFlight lane, add a durable non-secret marker queue around cross-store profile deletion, and make image decode tickets and resize-driven target changes observable to `ImageView`.

**Tech Stack:** C++23, CMake/CTest, bgfx Noop tests, Python `unittest`, Bash, Ruby/Fastlane, Xcode command-line tools.

## Global Constraints

- Marketing version remains `0.0.1`.
- The app deployment target remains iOS 14.
- `NSAllowsArbitraryLoads = true` remains enabled for difficulty-table loading.
- No privacy manifest is added.
- BGA file/archive access and decoder initialization must finish before playback; timed activation performs lookup plus seek/play only.
- Firebase PR iteration remains independent of the full release verifier; TestFlight remains gated.
- Do not upload a local build.

---

### Task 1: Protect Replay Export BGA Tail Duration

**Files:**

- Modify: `tests/jukebox_restore_tests.cpp`

**Interfaces:**

- Consumes: `Jukebox::loadVisuals()` and `Jukebox::getScheduledVisualEndMicros()`.
- Produces: A regression proving the preloaded video's duration extends the scheduled visual end beyond its event timestamp.

- [x] **Step 1: Add the regression assertion**

  Extend the existing temporary Y4M video fixture test with an event at a nonzero timestamp and assert that `getScheduledVisualEndMicros()` is greater than the event timestamp immediately after `loadVisuals()` and before `play()`.

- [x] **Step 2: Verify the regression protects the reviewed failure**

  Run the test against the current eager-preload implementation, then confirm the previously observed descriptor-only implementation at `7e2d64f6` would return only the event timestamp because it records a zero video duration.

- [x] **Step 3: Run the focused test**

  ```bash
  cmake --build cmake-build-debug --target jukebox_restore_tests -j 6
  ctest --test-dir cmake-build-debug -R '^foundation_av_jukebox_restore$' --output-on-failure
  ```

### Task 2: Scan Binary Payloads for Embedded Credentials

**Files:**

- Modify: `tests/ios_artifact_audit_tests.py`
- Modify: `scripts/ios_artifact_audit.sh`

**Interfaces:**

- Consumes: the artifact audit's `BINARIES` list containing the main Mach-O and embedded framework executables.
- Produces: failure when a credential signature exists in either a text resource or printable binary strings.

- [x] **Step 1: Write the failing binary-secret fixture**

  Compile a used string such as `APP_STORE_KEY=embedded-binary-fixture` into the fixture executable, run the real audit script, and assert a nonzero exit with the credential diagnostic.

- [x] **Step 2: Run the test and observe red**

  ```bash
  python3 -m unittest tests.ios_artifact_audit_tests.IOSArtifactAuditTests.test_embedded_binary_secret_fails
  ```

  Expected: the current `grep -R -I` scan skips the Mach-O and the audit incorrectly succeeds.

- [x] **Step 3: Add binary byte scanning**

  Retain the recursive text-resource scan, then scan every validated entry in
  `BINARIES` as raw bytes with the same credential pattern. Report only the
  binary path, never unrelated binary bytes. (`strings -a` was rejected because
  Apple's implementation omitted printable bytes outside recognized Mach-O
  sections.)

- [x] **Step 4: Run the artifact-audit suite**

  ```bash
  python3 tests/ios_artifact_audit_tests.py
  ```

### Task 3: Make Failed Profile Credential Cleanup Retryable

**Files:**

- Create: `src/ir/PendingIrCredentialCleanup.h`
- Create: `src/ir/PendingIrCredentialCleanup.cpp`
- Create: `tests/pending_ir_credential_cleanup_tests.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `src/context.h`
- Modify: `src/scene/ProfileSettingsControllerContext.cpp`

**Interfaces:**

- Produces: `ir::PendingIrCredentialCleanup`, which durably stores only validated profile IDs as marker files under the application-data root.
- Produces: `ir::coordinateProfileCredentialDeletion(...)`, which queues before profile deletion, cancels the marker on deletion failure, and retains it when secure credential removal fails.
- Produces: `ir::retryPendingProfileCredentialCleanup(...)`, which cancels markers for live profiles and retries credential removal only for profiles no longer present.
- Consumes: `PlayerProfileManager::deleteProfile()`, `IrCredentialBackend::removeProfile()`, and `PlayerProfileManager::listProfiles()` through narrow callbacks.

- [x] **Step 1: Write failing persistence and coordination tests**

  Cover these literal outcomes with a real temporary directory:

  - deletion failure leaves the profile credential remover untouched and clears the marker;
  - deletion success plus credential failure leaves a marker visible to a new queue instance;
  - a later retry for an absent profile removes credentials and clears the marker;
  - retry never removes credentials for a still-live profile.

- [x] **Step 2: Run the new target and observe red**

  ```bash
  cmake --build cmake-build-debug --target pending_ir_credential_cleanup_tests -j 6
  ```

  Expected: the target or required queue/coordinator symbols do not yet exist.

- [x] **Step 3: Implement the durable marker queue and coordinator**

  Use `atomic_file::writeWithoutBackup()` for marker creation, validate profile IDs with `isValidCredentialProfileId()`, reject symlink/non-regular marker entries, and sync directory metadata after marker removal. Markers contain no credential values.

- [x] **Step 4: Integrate deletion and startup retry**

  Initialize the queue from `ApplicationContext::applicationDataRoot`. Route profile deletion through the coordinator. After successful profile initialization, retry pending markers: cancel markers for IDs still listed by the manager, and call `removeProfileIrCredentials()` for missing IDs.

- [x] **Step 5: Run focused profile and credential tests**

  ```bash
  cmake --build cmake-build-debug --target \
    pending_ir_credential_cleanup_tests foundation_profile_controller \
    ir_credential_store_tests ir_credential_migration_tests -j 6
  ctest --test-dir cmake-build-debug \
    -R '^(pending_ir_credential_cleanup_tests|foundation_profile_controller|ir_credential_store_tests|ir_credential_migration_tests)$' \
    --output-on-failure
  ```

### Task 4: Recover Live Image Views After Decode Eviction

**Files:**

- Modify: `src/view/ImageDecodeCoordinator.h`
- Modify: `src/view/ImageDecodeCoordinator.cpp`
- Modify: `src/view/ImageView.cpp`
- Modify: `src/view/ImageView.h`
- Modify: `tests/image_decode_coordinator_tests.cpp`
- Modify: `tests/image_view_fade_tests.cpp`

**Interfaces:**

- Produces: `ImageDecodeCoordinator::isTracked(Ticket) const`.
- Consumes: `ImageView::asyncTicket` before polling or reusing an asynchronous request.
- Produces: a replacement ticket when `dropAll()` invalidates a live view's old ticket.

- [x] **Step 1: Write failing coordinator and ImageView regressions**

  Assert a ticket is tracked before `dropAll()` and untracked afterward. Bind a missing image asynchronously, record the ticket through the existing test-only build surface, evict decoded work, bind the same path again, and assert a new ticket is issued.

- [x] **Step 2: Run focused tests and observe red**

  ```bash
  cmake --build cmake-build-debug --target image_decode_coordinator_tests image_view_fade_tests -j 6
  ctest --test-dir cmake-build-debug \
    -R '^(image_decode_coordinator_tests|image_view_fade_tests)$' \
    --output-on-failure
  ```

- [x] **Step 3: Implement ticket observability and recovery**

  Add the locked `isTracked()` query. Before an `ImageView` polls or reuses `asyncTicket`, reset it when the coordinator no longer tracks it, allowing the normal request path to issue a fresh ticket.

- [x] **Step 4: Re-run the focused tests**

  Run the commands from Step 2 and require both tests to pass.

### Task 5: Refresh Downsampled Artwork When Layout Grows

**Files:**

- Modify: `src/view/ImageView.cpp`
- Modify: `src/view/ImageView.h`
- Modify: `tests/image_view_fade_tests.cpp`

**Interfaces:**

- Consumes: `View::onLayout()` and the current asynchronous image path.
- Produces: resize-aware asynchronous target dimensions while retaining the old texture until the larger decode is ready.

- [x] **Step 1: Write the failing growth regression**

  Load a 512x256 PPM into a 64x32 `ImageView`, wait for the 64x32 decode, enlarge the layout to 256x128 inside `View::LayoutBatchScope`, render/poll without rebinding the path, and assert the decoded image becomes 256x128.

- [x] **Step 2: Run the ImageView test and observe red**

  ```bash
  cmake --build cmake-build-debug --target image_view_fade_tests -j 6
  ctest --test-dir cmake-build-debug -R '^image_view_fade_tests$' --output-on-failure
  ```

  Expected: the decoded dimensions remain 64x32 because no new sized key is requested.

- [x] **Step 3: Implement growth-triggered refresh**

  Track the last asynchronous target width and height, override `onLayout()`, and issue a prioritized request when either laid-out dimension grows. Preserve a valid old texture for the same path until the larger cached/decoded image replaces it.

- [x] **Step 4: Re-run ImageView and memory-pressure tests**

  ```bash
  cmake --build cmake-build-debug --target image_view_fade_tests mobile_memory_pressure_tests -j 6
  ctest --test-dir cmake-build-debug \
    -R '^(image_view_fade_tests|mobile_memory_pressure_tests)$' \
    --output-on-failure
  ```

### Task 6: Gate TestFlight on the Signed IPA Audit

**Files:**

- Modify: `tests/ios_release_workflow_tests.py`
- Modify: `ios/Xcode/AsoBMaShow/fastlane/Fastfile`

**Interfaces:**

- Consumes: `lane_context[SharedValues::IPA_OUTPUT_PATH]` after `temporary_fix_ios_post_build`.
- Produces: `scripts/ios_artifact_audit.sh --require-signature <IPA>` before `upload_to_testflight`.

- [x] **Step 1: Write the failing lane-order regression**

  Parse the TestFlight lane, require the signed audit command and `IPA_OUTPUT_PATH`, and assert the audit appears after the post-build IPA fix but before `upload_to_testflight`.

- [x] **Step 2: Run the workflow tests and observe red**

  ```bash
  python3 -m unittest tests.ios_release_workflow_tests.IOSReleaseWorkflowTests.test_testflight_audits_signed_ipa_before_upload
  ```

- [x] **Step 3: Add the signed audit gate**

  Resolve the repository-root script from `__dir__`, validate the IPA path is present, shell-escape both paths, and invoke the audit with `--require-signature` between the post-build fix and upload.

- [x] **Step 4: Run release workflow and artifact tests**

  ```bash
  python3 tests/ios_release_workflow_tests.py
  python3 tests/ios_artifact_audit_tests.py
  ```

### Task 7: Record, Verify, and Publish the Review Fixes

**Files:**

- Modify: `docs/release-readiness-audit-2026-08-01.md`
- Modify: this plan as tasks complete

**Interfaces:**

- Consumes: all task-specific test results.
- Produces: an exact-commit verification record and an updated PR head.

- [x] **Step 1: Update the audit record**

  Record binary credential scanning, durable failed-Keychain cleanup retries, resize/ticket image recovery, replay-tail coverage, and the signed TestFlight audit gate.

- [x] **Step 2: Run focused and full native verification**

  ```bash
  cmake --build cmake-build-debug --target main -j 6
  ctest --test-dir cmake-build-debug --output-on-failure -j 6
  git diff --check
  ```

- [x] **Step 3: Run iOS release policy tests**

  ```bash
  python3 tests/ios_build_setup_tests.py
  python3 tests/ios_release_workflow_tests.py
  python3 tests/ios_artifact_audit_tests.py
  python3 tests/ios_release_documentation_tests.py
  ```

- [ ] **Step 4: Commit and run exact-commit verification**

  Commit the review remediation, then run `scripts/ios_release_verify.sh` without deploying.

- [ ] **Step 5: Push and update PR #89**

  Push the existing branch, summarize addressed and already-fixed threads on the PR, and report any remaining unresolved non-actionable thread without resolving or replying unless explicitly authorized.
