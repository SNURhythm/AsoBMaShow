# Build Storage Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Reduce repeated desktop test compilation and Android dependency installations without reducing debug information.

**Architecture:** Explicit test source/consumer groups reuse small object targets while preserving compile contracts. An Android-only CMake module selects a dependency identity before project initialization and serializes installation into a checkout-local cache.

**Tech Stack:** CMake 3.22+, Ninja, Python unittest, Android Gradle Plugin 8.10.1, vcpkg.

**Spec:** `docs/superpowers/specs/2026-09-24-build-storage-design.md`

## Global Constraints

- Work on the user's current branch `fix/optimize-build-artifacts`; create no worktree.
- Preserve compiler options, debug information, assertions, test names, fixtures, and platform guards.
- Preserve NDK 28.2.13676358 and deployment/signing behavior; verification uses build-only scripts.
- Edit no amalgamated parser source, shader, or iOS deployment policy.
- The user approved the spec and explicitly requested continuous execution without further approval stops.

## Review Focus

- A test-specific definition must never leak into another consumer's shared object (Task 1 variant fixture).
- A source used by only one test must not be injected into unrelated executables (Task 1 subset fixture).
- Debug/release and equivalent path spellings must reuse dependencies (Task 2 identity fixtures).
- Changed overlay/manifest/toolchain inputs must invalidate the installation (Task 2 invalidation fixtures).
- Concurrent configure and failed configure must not corrupt or strand the installation (Task 2 process-lock fixtures).

### Task 1: Share explicitly selected desktop test sources

**Files:** `cmake/SharedTestSources.cmake`, `cmake/SharedTestDependencies.cmake`, `CMakeLists.txt`, `tests/shared_test_sources_tests.py`.

**Interfaces:** A helper accepts a named group, explicit source list, and explicit consumers. It removes each eligible source from that consumer and adds only its corresponding object. Consumers with differing compilation contracts retain separate objects. Existing source and test registration APIs remain intact.

- [x] Capture the current compile database, CTest inventory, and object sizes under the task workspace before regenerating.
- [x] Write and run a failing fixture: two executables use a common source, a third uses a different definition, and one executable owns an additional source. Configure/build/run in Debug and Release; expect identical consumers to compile the common source once, distinct output for the variant, and no accidental linking of the extra source.
- [x] Implement small object targets with explicit compiler-property ownership and dependency usage requirements. Preserve consumer link dependencies. Keep unsupported target-sensitive settings unshared rather than guessing.
- [x] Declare source/consumer groups for parser/result primitives, archive-backed tests, and Lua decoder tests. Include the declarations after existing test setup so assertion and platform settings are final.
- [x] Regenerate the existing desktop tree and compare the new effective commands with the captured commands. Build affected tests and main, then run the full parallel CTest suite.

```sh
python3 tests/shared_test_sources_tests.py -v
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Expected: fixture passes, migrated sources have equivalent compile options, all affected executables link, and existing tests remain registered. Diagnose pre-existing suite failures separately.

### Task 2: Consolidate Android dependencies

**Files:** `cmake/AndroidDependencies.cmake`, `CMakeLists.txt`, `android/app/build.gradle`, `tests/android_dependency_cache_tests.py`.

**Interfaces:** `asobmashow_prepare_android_dependencies()` sets `VCPKG_INSTALLED_DIR` before `project()` and holds a process lock. `asobmashow_release_android_dependencies()` releases it immediately after initialization. Desktop builds do not invoke these functions.

- [x] Write failing CMake fixtures that select an installation using miniature manifest, triplet, overlay, vcpkg, and NDK trees. Assert path equivalence, build-configuration independence, invalidation, missing-input failure, lock timeout, and release after process failure.
- [x] Implement the versioned identity from canonical paths and dependency input contents. Serialize before `project()` with `file(LOCK ... GUARD PROCESS TIMEOUT ...)`; expose the selected path and persist identity inputs for diagnosis.
- [x] Canonicalize Gradle's toolchain roots and opt Android configurations into the module. Retain AGP's separate native object directories.
- [x] Run fixtures and both supported build-only commands. Inspect CMake caches to confirm dependency reuse and distinct native build roots. Repeat a build to verify incremental reuse.

```sh
python3 tests/android_dependency_cache_tests.py -v
scripts/android_firebase_deploy.sh --build-only
scripts/android_firebase_deploy.sh --build-only --variant playDebug
```

Expected: fixture contracts pass, both variants build, and both caches point to the same compatible dependency installation.

### Task 3: Measure, review, and publish the branch

**Files:** implementation plan and build-storage usage documentation as needed; task-only changes from Tasks 1–2.

**Interfaces:** Consume baseline/output manifests and passing verification evidence. Publish verified commits to `origin/fix/optimize-build-artifacts` and configure its upstream.

- [x] Remove only proven retired desktop object outputs from the captured graph after builds finish. Report retained Android configurations separately; do not broadly delete build directories.
- [x] Record live object savings and dependency installation sizes without double-counting stale directories.
- [x] Request one fresh whole-branch review as required by the execution skill; address material findings and rerun relevant checks.
- [x] Verify the diff and working tree, commit only task changes, and push with `git push -u origin fix/optimize-build-artifacts`.

```sh
git diff --check
git status --short
```

Expected: no whitespace errors, no unrelated changes included, verified task commits on the user's new branch.

## Verification outcome

See `docs/build-storage.md` for command-equivalence checks, measured savings,
Android build results, and the parallel audio-test timeout limitation. All 404
cases passed across the full suite and isolated audio verification; the full
parallel suite itself returned a timeout failure and is not reported as green.
The three independent review findings were reproduced and addressed with
regression coverage before final builds.
