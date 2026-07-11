# Portable MIDI Backend Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make MIDI activation, callback teardown, live CoreMIDI identity, Android retry, and Android text conversion safe under the reviewed races.

**Architecture:** Portable helpers own the concurrency and identity invariants: `NativeCallbackLifetime` resolves non-reused opaque tokens into counted leases, `LiveMidiDeviceIdAllocator` owns collision-free live claims, and `utf16ToUtf8` validates JNI text. Native adapters consume those helpers, while `QueuedMidiInputBackend::DeviceActivation` guarantees connect publication before native activation with rollback.

**Tech Stack:** C++23, CMake/CTest, CoreMIDI Objective-C++, WinMM, Android Java/JNI, Gradle, Xcode project membership.

## Global Constraints

- Work only in `/Users/xf/workspace/SNURhythm/AsoBMaShow/.worktrees/foundation-input-midi` on `feature/foundation-input-midi`.
- Do not merge or deploy.
- Native callbacks enqueue only; registry publication stays on main-thread `pump()`.
- Opaque callback IDs are monotonic and never reused during the process.
- Failed native activation queues a compensating disconnect before the next pump.
- Two simultaneously live CoreMIDI endpoints never share a stable ID.
- Android automatic retries are capped at three per trigger and canceled by generation changes.

---

### Task 1: Opaque callback lifetime registry

**Files:**
- Create: `src/input/NativeCallbackLifetime.h`
- Create: `src/input/NativeCallbackLifetime.cpp`
- Modify: `tests/midi_input_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/input/CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: an owner `void *` registered while a native callback context is live.
- Produces: `void *token()`, `static Lease acquire(void *)`, and `closeAndWait()`.

- [ ] **Step 1: Write failing lifetime tests**

Add tests which use the wished-for interface:

```cpp
int owner = 7;
NativeCallbackLifetime lifetime(&owner);
const void *token = lifetime.token();

std::binary_semaphore entered(0);
std::binary_semaphore release(0);
std::atomic_bool closed = false;
std::jthread callback([&] {
  auto lease = NativeCallbackLifetime::acquire(const_cast<void *>(token));
  require(lease.ownerAs<int>() == &owner, "lease retains owner");
  entered.release();
  release.acquire();
});
entered.acquire();
std::jthread closer([&] {
  lifetime.closeAndWait();
  closed.store(true);
});
require(!closed.load(), "close waits for active lease");
release.release();
closer.join();
require(!NativeCallbackLifetime::acquire(const_cast<void *>(token)),
        "stale token cannot reacquire");
```

Add separate delayed-entry and non-reuse tests: close before `acquire(token)`, and verify a subsequently registered owner gets a different token.

- [ ] **Step 2: Run red**

Run:

```bash
cmake --build cmake-build-debug --target midi_input_tests -j 6
```

Expected: compile failure because `NativeCallbackLifetime` does not exist.

- [ ] **Step 3: Implement the minimal registry**

Implement a process-lifetime registry keyed by `std::uintptr_t`. `Lease` holds `std::shared_ptr<State>`, increments `activeCallbacks` only after checking `closed`, and decrements/notifies in its destructor. `closeAndWait()` erases the token before closing/waiting its state. Generate tokens from an atomic counter and never return zero or reuse a prior value.

Core shape:

```cpp
class NativeCallbackLifetime {
public:
  class Lease {
  public:
    Lease(Lease &&) noexcept;
    Lease &operator=(Lease &&) noexcept;
    ~Lease();
    explicit operator bool() const;
    template <typename T> T *ownerAs() const {
      return static_cast<T *>(owner_);
    }
  };

  explicit NativeCallbackLifetime(void *owner);
  ~NativeCallbackLifetime();
  void *token() const;
  void closeAndWait();
  static Lease acquire(void *token);
};
```

- [ ] **Step 4: Wire and run green**

Add the `.cpp` to `midi_input_tests` and `asobmashow_add_midi_backend`, and add its path to the iOS membership exceptions. Reconfigure if required, build, and run:

```bash
cmake --build cmake-build-debug --target midi_input_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_input_midi$' --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/input/NativeCallbackLifetime.* tests/midi_input_tests.cpp \
  CMakeLists.txt src/input/CMakeLists.txt \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "fix(input): make native callback entry lifetime safe"
```

---

### Task 2: Activation boundary and live ID allocator

**Files:**
- Create: `src/input/LiveMidiDeviceIdAllocator.h`
- Create: `src/input/LiveMidiDeviceIdAllocator.cpp`
- Modify: `src/input/QueuedMidiInputBackend.h`
- Modify: `src/input/QueuedMidiInputBackend.cpp`
- Modify: `tests/midi_input_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/input/CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Produces: move-only `QueuedMidiInputBackend::DeviceActivation beginDeviceActivation(snapshot)` with `commit()`.
- Produces: `claim(std::uintptr_t key, std::string preferred)`, `release(key)`, and `clear()`.

- [ ] **Step 1: Write failing activation tests**

Expose a test-backend method that begins activation, synchronously enqueues a packet, and either commits or lets rollback run. Assert success publishes connected snapshot before input; assert failure publishes connected then disconnected snapshots.

```cpp
void activate(std::string id, bool success, bool emitPacket) {
  auto activation = beginDeviceActivation({.stableId = id,
      .displayName = "Activation", .deviceClass = input::DeviceClass::Midi,
      .connected = true});
  if (emitPacket) packet(id, {0x90, 60, 127}, 200);
  if (success) activation.commit();
}
```

- [ ] **Step 2: Write failing allocator tests**

Cover preservation and the review sequence:

```cpp
LiveMidiDeviceIdAllocator ids;
require(ids.claim(1, "base") == "base", "A claims base");
const auto b = ids.claim(2, "base:identical:2");
ids.release(1);
const auto readdedA = ids.claim(3, "base:identical:2");
require(readdedA != b, "re-added A cannot collide with live B");
require(ids.claim(2, "changed-preference") == b,
        "live assignment is preserved");
```

- [ ] **Step 3: Run red**

Build `midi_input_tests`; expect missing `beginDeviceActivation` and allocator symbols.

- [ ] **Step 4: Implement minimal activation RAII and allocator**

`DeviceActivation` queues a connected snapshot on construction; its destructor queues the same snapshot with `connected=false` unless `commit()` cleared its owner. The allocator stores key-to-ID and used-ID maps; collision candidates append `:session:<n>` until unused.

- [ ] **Step 5: Wire and run green**

Add allocator source to platform backend targets/tests and iOS membership, then run focused build/CTest. Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add src/input/LiveMidiDeviceIdAllocator.* \
  src/input/QueuedMidiInputBackend.* tests/midi_input_tests.cpp \
  CMakeLists.txt src/input/CMakeLists.txt \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "fix(input): order MIDI activation and live IDs"
```

---

### Task 3: Valid UTF-16 to UTF-8 conversion

**Files:**
- Create: `src/input/Utf16ToUtf8.h`
- Create: `src/input/Utf16ToUtf8.cpp`
- Modify: `tests/midi_input_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/input/CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Produces: `std::string utf16ToUtf8(std::u16string_view value)`.

- [ ] **Step 1: Write failing conversion tests**

Assert ASCII/BMP, `U+1F3B9` from `0xD83C 0xDFB9`, and U+FFFD replacement for lone high/low surrogates.

- [ ] **Step 2: Run red**

Build `midi_input_tests`; expect missing converter symbols.

- [ ] **Step 3: Implement scalar conversion**

Decode valid surrogate pairs into code points, replace malformed surrogates with `0xFFFD`, and append canonical one-to-four-byte UTF-8 sequences.

- [ ] **Step 4: Wire and run green**

Add source to test/backend targets and iOS membership. Build and run focused CTest; expected pass.

- [ ] **Step 5: Commit**

```bash
git add src/input/Utf16ToUtf8.* tests/midi_input_tests.cpp \
  CMakeLists.txt src/input/CMakeLists.txt \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "fix(input): preserve Unicode MIDI names"
```

---

### Task 4: CoreMIDI and WinMM consume the safety contracts

**Files:**
- Modify: `src/input/CoreMidiInputBackend.mm`
- Modify: `src/input/WinMidiInputBackend.cpp`

**Interfaces:**
- Consumes: `NativeCallbackLifetime`, `DeviceActivation`, and `LiveMidiDeviceIdAllocator`.

- [ ] **Step 1: Refactor CoreMIDI callback entry**

Give each connection a `NativeCallbackLifetime(this)` and pass `lifetime.token()` to `MIDIPortConnectSource`. The callback acquires a lease before accessing the connection. On failure/removal/stop: mark disconnected, disconnect native source, close-and-wait lifetime, then destroy connection. Remove `CoreMidiCallbackGate` and `retiredConnections_`.

- [ ] **Step 2: Add shared CoreMIDI client service**

Create an explicitly process-lifetime service in the `.mm` file. It lazily creates one `MIDIClientRef`, never explicitly disposes the last client, stores active backend notification tokens under a mutex, and dispatches by `NativeCallbackLifetime::acquire`. Backend start subscribes; stop unsubscribes before closing its notification lifetime.

- [ ] **Step 3: Apply activation and live-ID allocation to CoreMIDI**

Claim the endpoint's live ID, begin queued activation, call `MIDIPortConnectSource`, commit on success, and rollback/release/close on failure. Release the claim on disconnect and clear it on shutdown.

- [ ] **Step 4: Refactor WinMM callback entry and activation**

Pass a per-connection opaque token as `dwInstance`, acquire before dereference, unregister/drain before freeing, remove `WinMidiCallbackGate` and retired storage, and begin queued activation before `midiInStart` with rollback on failure.

- [ ] **Step 5: Build and run focused tests**

```bash
cmake --build cmake-build-debug --target \
  midi_input_tests input_device_registry_tests main -j 6
ctest --test-dir cmake-build-debug \
  -R '^foundation_input_(midi|registry)$' --output-on-failure
```

Expected: pass and CoreMIDI compile/link succeeds. WinMM receives static review on this host.

- [ ] **Step 6: Commit**

```bash
git add src/input/CoreMidiInputBackend.mm src/input/WinMidiInputBackend.cpp
git commit -m "fix(input): harden desktop MIDI callbacks"
```

---

### Task 5: Android activation, retry, and JNI text

**Files:**
- Create: `android/app/src/main/java/com/snurhythm/asobmashow/MidiOpenRetryPolicy.java`
- Create: `android/app/src/test/java/com/snurhythm/asobmashow/MidiOpenRetryPolicyTest.java`
- Modify: `android/app/build.gradle`
- Modify: `src/input/AndroidMidiInputBackend.cpp`
- Modify: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowMidiManager.java`

**Interfaces:**
- Consumes: `utf16ToUtf8`.
- Produces: at most three open attempts per start/add/status trigger.

- [ ] **Step 1: Write the failing retry-policy unit test**

Add JUnit coverage for three allowed attempts, rejection of the fourth, per-device isolation, reset after a new trigger, and `clear()` at generation shutdown. Use the wished-for `MidiOpenRetryPolicy` package-private API.

- [ ] **Step 2: Run retry-policy red**

Run `:app:testPlayDebugUnitTest`; expect compilation failure because `MidiOpenRetryPolicy` does not exist.

- [ ] **Step 3: Implement and wire the minimal retry policy**

Create a pure Java class backed by `Map<Integer, Integer>` with `beginAttempt`, `reset`, `remove`, and `clear`. Add `testImplementation 'junit:junit:4.13.2'`, rerun the unit task, and expect pass.

- [ ] **Step 4: Convert JNI strings through UTF-16**

Use `GetStringLength`/`GetStringChars`, copy into `std::u16string_view`, call `utf16ToUtf8`, and always release the JNI chars. Leave JNI exceptions pending for allocation failure so the callback returns without publishing partial text.

- [ ] **Step 5: Publish Android connect before receiver activation**

Call `nativeMidiDevice(stableId, displayName, true)` before `outputPort.connect(receiver)`. If connection throws, close the receiver/port and call the matching native disconnect. Add the port to `connection.ports` only after successful activation.

- [ ] **Step 6: Add bounded generation-safe retry**

Track attempts per framework device ID, cap at three, and schedule retry with `mainHandler.postDelayed`. Startup/device-add resets a budget, status change re-arms only an unconnected/non-pending device, successful open clears state, and stop/removal clears state while generation checks make delayed work inert.

- [ ] **Step 7: Build Android Java and arm64 native code**

```bash
scripts/android_firebase_deploy.sh --build-only --variant playDebug
```

Expected: `BUILD SUCCESSFUL`; no upload.

- [ ] **Step 8: Commit**

```bash
git add src/input/AndroidMidiInputBackend.cpp android/app/build.gradle \
  android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowMidiManager.java \
  android/app/src/main/java/com/snurhythm/asobmashow/MidiOpenRetryPolicy.java \
  android/app/src/test/java/com/snurhythm/asobmashow/MidiOpenRetryPolicyTest.java
git commit -m "fix(input): harden Android MIDI lifecycle"
```

---

### Task 6: Complete verification and handoff

**Files:**
- Modify: `.superpowers/sdd/input-task-5-report.md` in the integration worktree.

**Interfaces:**
- Produces: exact remediation range and reproducible verification evidence for re-review.

- [ ] **Step 1: Repeat focused tests**

```bash
ctest --test-dir cmake-build-debug \
  -R '^foundation_input_(midi|registry)$' \
  --repeat until-fail:50 --output-on-failure
```

- [ ] **Step 2: Run full desktop build and CTest**

```bash
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
```

- [ ] **Step 3: Run iOS and Android build-only verification**

```bash
scripts/ios_firebase_deploy.sh --build-only
scripts/android_firebase_deploy.sh --build-only --variant playDebug
```

Neither command uploads.

- [ ] **Step 4: Run hygiene checks**

```bash
git diff --check e3b2d49..HEAD
plutil -lint ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
xmllint --noout android/app/src/main/AndroidManifest.xml
git status --short --branch
```

- [ ] **Step 5: Update report and commit any final tracked changes**

Record exact base/head/range, finding disposition, Windows static-only limitation, and fresh command results. Ensure generated build products are ignored and the feature worktree is clean.
