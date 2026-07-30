# iOS 7-Zip Codec Registration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make real LZMA/LZMA2-compressed `.7z` archives open and stream through the 7-Zip SDK on iOS instead of returning `S_FALSE` and falling back to libarchive.

**Architecture:** Preserve the current archive handler registration and explicitly link only the codec registration objects exercised by a real `7zz` fixture. Cover both the CMake regression target and every iOS device/simulator build configuration, while retaining libarchive fallback for unsupported codecs.

**Tech Stack:** C++23, 7-Zip SDK static library, libarchive, CMake, Xcode project settings, Python `unittest`.

## Global Constraints

- Work only in the existing `perf/dynamic-chart-scan-scheduling` isolated worktree.
- Execute inline with self-review; do not dispatch subagents or pause for plan approval.
- Do not add `PpmdRegister.cpp.o`, because the repository documents duplicate PPMd symbol conflicts with libarchive.
- Do not run a local performance benchmark; this machine has no representative archive library.
- Use `scripts/ios_firebase_deploy.sh --build-only --skip-init` for final iOS link verification; do not upload a local build.
- Ignore image-loading telemetry because it is unrelated to parsing.

---

### Task 1: Reproduce encoded-header 7z failure

**Files:**
- Create: `tests/fixtures/archive/encoded-header-payload.txt`
- Create: `tests/fixtures/archive/encoded-header-lzma.7z`
- Create: `tests/fixtures/archive/encoded-header-lzma2.7z`
- Modify: `tests/archive_file_concurrency_tests.cpp`

**Interfaces:**
- Consumes: `archive_file::listEntries`, `archive_file::readArchiveEntries`, and `archive_file::debugLogLines`.
- Produces: `testEncodedHeaderSevenZipUsesSdk()`, which proves both the 7z header and payload decode through the SDK.

- [ ] **Step 1: Create a deterministic compressed-header fixture**

Create a payload containing the literal `compressed-header-payload\n`, then run:

```bash
(cd tests/fixtures/archive && 7zz a -t7z -mx=9 -mhc=on -m0=LZMA encoded-header-lzma.7z encoded-header-payload.txt)
(cd tests/fixtures/archive && 7zz a -t7z -mx=9 -mhc=on -m0=LZMA2 encoded-header-lzma2.7z encoded-header-payload.txt)
```

- [ ] **Step 2: Write the failing real-archive test**

Add `testEncodedHeaderSevenZipUsesSdk()` that exercises both archives, reads `encoded-header-payload.txt`, asserts the literal payload bytes, and asserts that each fixture-specific log line says `Indexed archive with 7-Zip SDK:` rather than the libarchive fallback.

- [ ] **Step 3: Run the test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target archive_file_concurrency_tests -j 6
./cmake-build-debug/archive_file_concurrency_tests
```

Expected: FAIL because the SDK indexes the archive but returns an empty decoded payload before the matching codec registration is linked.

### Task 2: Link the proven LZMA codec registrations

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`
- Modify: `tests/ios_build_setup_tests.py`

**Interfaces:**
- Consumes: `LzmaRegister.cpp.o` and `Lzma2Register.cpp.o` from both 7-Zip static-library slices.
- Produces: resolved Xcode linker inputs and CMake test inputs that retain the static codec initializers.

- [ ] **Step 1: Extend the iOS resolved-linker regression and verify RED**

Require `LzmaRegister.cpp.o` and `Lzma2Register.cpp.o` alongside `7zRegister.cpp.o` for Debug/Release and device/simulator, then run the focused Python test. Expected: FAIL because the codec objects are absent.

- [ ] **Step 2: Add the minimal registration objects**

Extract the two codec registration objects in the existing Xcode preparation phase, add them to all four resolved linker configurations, and add the same external objects to `archive_file_concurrency_tests` in CMake.

- [ ] **Step 3: Run both regressions to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target archive_file_concurrency_tests -j 6
./cmake-build-debug/archive_file_concurrency_tests
python3 -m unittest tests.ios_build_setup_tests.IOSBuildSetupTests.test_ios_links_7zip_archive_registration_for_device_and_simulator
```

Expected: both pass; the fixture logs the SDK backend and yields the literal payload.

### Task 3: Verify the iOS product and publish

**Files:**
- Verify: all files changed by Tasks 1 and 2
- Update: GitHub PR #84 description

**Interfaces:**
- Consumes: the Release iOS application binary produced by the repository build script.
- Produces: an iOS Mach-O containing the 7z handler plus LZMA and LZMA2 registration initializers.

- [ ] **Step 1: Run repository validation**

Run the relevant C++ test, focused iOS setup test, `plutil -lint`, and `git diff --check`. Run the full iOS setup suite and record the existing unrelated `ir/CMakeLists.txt` membership-exception failure separately if it remains.

- [ ] **Step 2: Build and inspect the iOS binary**

Run:

```bash
scripts/ios_firebase_deploy.sh --build-only --skip-init
```

Use `nm -arch arm64 | c++filt` on the resulting app binary and require `__GLOBAL__sub_I_7zRegister.cpp`, `__GLOBAL__sub_I_LzmaRegister.cpp`, and `__GLOBAL__sub_I_Lzma2Register.cpp`.

- [ ] **Step 3: Self-review and publish**

Review the complete diff for unintended link objects, generated files, and stale paths. Commit only the scoped files, push `perf/dynamic-chart-scan-scheduling`, update PR #84 with the second-stage root cause and validation, and confirm local/remote commit equality.
