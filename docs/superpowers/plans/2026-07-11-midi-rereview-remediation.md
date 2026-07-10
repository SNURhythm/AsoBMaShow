# MIDI Re-review Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve CoreMIDI binding identity across coalesced endpoint replacement and make removed Android devices immune to already-posted retry work.

**Architecture:** A portable refresh-plan seam will emit all removals before additions and will be the only ordering source used by CoreMIDI refresh. Android retry scheduling tokens will move into `MidiOpenRetryPolicy`, where consuming the exact posted token is atomic with attempt-budget reservation and removal invalidates it without allowing stale work to alias a later retry.

**Tech Stack:** C++20, CoreMIDI Objective-C++, CTest, Java, Android Gradle, JUnit 4.

## Global Constraints

- Work only on `feature/foundation-input-midi` in `.worktrees/foundation-input-midi`.
- Use test-first red/green cycles for both findings.
- Do not merge, push, deploy, or upload.
- Re-run focused repeat tests, desktop build/CTest, Android unit/native build, and iOS build-only because CoreMIDI changes.

---

### Task 1: CoreMIDI removal-before-addition reconciliation

**Files:**
- Modify: `src/input/LiveMidiDeviceIdAllocator.h`
- Modify: `src/input/LiveMidiDeviceIdAllocator.cpp`
- Modify: `src/input/CoreMidiInputBackend.mm`
- Test: `tests/midi_input_tests.cpp`

**Interfaces:**
- Consumes: existing endpoint keys and newly enumerated endpoint keys as `std::span<const std::uintptr_t>`.
- Produces: `planLiveMidiDeviceRefresh(...)`, returning ordered `LiveMidiDeviceRefreshAction` values with every `Remove` preceding every `Add`.

- [ ] **Step 1: Write the failing same-refresh replacement test**

Add a test that seeds endpoint 1 with `midi:core:42`, requests a refresh from existing `{1}` to current `{2, 3}`, applies the returned actions to the real allocator, and asserts endpoint 2 retains `midi:core:42` while endpoint 3 receives a distinct session ID. Register it in `main()`.

- [ ] **Step 2: Run the MIDI test and verify red**

Run:

`cmake --build cmake-build-debug --target midi_input_tests -j 6`

Expected: compilation fails because the refresh-plan interface does not exist.

- [ ] **Step 3: Implement the portable ordered refresh plan**

Add:

```cpp
enum class LiveMidiDeviceRefreshActionKind { Remove, Add };

struct LiveMidiDeviceRefreshAction {
  LiveMidiDeviceRefreshActionKind kind;
  std::uintptr_t key;
};

std::vector<LiveMidiDeviceRefreshAction>
planLiveMidiDeviceRefresh(std::span<const std::uintptr_t> existingKeys,
                          std::span<const std::uintptr_t> currentKeys);
```

Build a current-key set and emit absent existing keys as `Remove`; then build an existing-key set and emit new current keys as `Add`, preserving input order within each phase.

- [ ] **Step 4: Make CoreMIDI consume the tested plan**

Enumerate sources once, build existing/current key vectors, and iterate `planLiveMidiDeviceRefresh`. For `Remove`, disconnect, close the callback lifetime, enqueue disconnect, and release the claimed ID. For `Add`, find the descriptor and perform the existing activation/connect/rollback path. This makes stale canonical claims disappear before any replacement claims.

- [ ] **Step 5: Verify green and commit**

Run:

`cmake --build cmake-build-debug --target midi_input_tests -j 6 && ctest --test-dir cmake-build-debug -R '^foundation_input_midi$' --output-on-failure`

Expected: build succeeds and the MIDI CTest passes.

Commit message: `fix(input): preserve CoreMIDI IDs on replacement`.

### Task 2: Android scheduled-retry cancellation

**Files:**
- Modify: `android/app/src/main/java/com/snurhythm/asobmashow/MidiOpenRetryPolicy.java`
- Modify: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowMidiManager.java`
- Test: `android/app/src/test/java/com/snurhythm/asobmashow/MidiOpenRetryPolicyTest.java`

**Interfaces:**
- Produces: `long scheduleRetry(int)`, `isRetryScheduled(int)`, and `beginScheduledAttempt(int, long)`.
- `remove(int)` and `clear()` invalidate scheduled tokens as well as attempt budgets.

- [ ] **Step 1: Write failing scheduling/removal tests**

Add one test that schedules device 40, removes it, schedules a replacement retry for the same framework ID, and asserts the old token cannot consume the replacement token. Add another that models one immediate attempt plus scheduled attempts and asserts only three attempts are admitted for a trigger.

- [ ] **Step 2: Run Android unit tests and verify red**

Run:

```sh
ANDROID_HOME=/opt/homebrew/share/android-commandlinetools \
ANDROID_SDK_ROOT=/opt/homebrew/share/android-commandlinetools \
ANDROID_NDK_HOME=/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358 \
VCPKG_ROOT=/Users/xf/vcpkg \
JAVA_HOME=$(/usr/libexec/java_home -v 17) \
SDL/android-project/gradlew -p android :app:testPlayDebugUnitTest
```

Expected: compilation fails because the scheduling-policy methods do not exist.

- [ ] **Step 3: Implement atomic scheduled-attempt admission**

Store a monotonically allocated token per scheduled device inside `MidiOpenRetryPolicy`. `scheduleRetry` returns that token; `beginScheduledAttempt` returns false unless the supplied token exactly matches the current token, then removes it and delegates to the existing capped `beginAttempt`; removal/clear erase tokens.

- [ ] **Step 4: Route the manager through the policy**

Remove the manager-owned retry set. Check `isRetryScheduled` in `requestOpen`, capture the token from `scheduleRetry` in the posted runnable, and require `beginScheduledAttempt(deviceId, token)` before opening. Extract the native open call into a helper so immediate and scheduled paths reserve exactly one budget unit.

- [ ] **Step 5: Verify green and commit**

Run the Android unit command from Step 2.

Expected: all policy tests pass.

Commit message: `fix(input): cancel stale Android MIDI retries`.

### Task 3: Cross-platform verification and handoff

**Files:**
- Modify: `.worktrees/player-foundations/.superpowers/sdd/input-task-5-report.md` outside the feature branch.

- [ ] **Step 1: Repeat focused tests**

Run:

`ctest --test-dir cmake-build-debug -R '^foundation_input_(midi|registry)$' --repeat until-fail:50 --output-on-failure`

Expected: both selected tests pass 50 consecutive runs.

- [ ] **Step 2: Run desktop gates**

Run:

```sh
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: app build succeeds and CTest reports zero failures.

- [ ] **Step 3: Run platform gates**

Run:

```sh
scripts/android_firebase_deploy.sh --build-only --variant playDebug
scripts/ios_firebase_deploy.sh --build-only
```

Expected: Android reports `BUILD SUCCESSFUL`; iOS reports `** BUILD SUCCEEDED **`. Neither command uploads.

- [ ] **Step 4: Run hygiene and update report**

Run `git diff --check a474c03..HEAD`, `plutil -lint ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`, `xmllint --noout android/app/src/main/AndroidManifest.xml`, and `git status --short --branch`. Update the integration report with both finding dispositions, exact range, and fresh evidence.
