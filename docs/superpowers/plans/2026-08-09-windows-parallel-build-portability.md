# Windows Parallel Build and Skin Portability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make parallel MSVC builds reliable and make `main` plus the directly related skin test targets build and pass on Windows.

**Architecture:** Coordinate MSVC compiler-PDB writes with `/FS`, replace POSIX-only archive types with libarchive-derived types, and remove non-portable standard-library noexcept assumptions while allowing coordinator delivery operations to propagate allocation failures instead of terminating. Retain the existing bounded queues, retry ownership, private Windows security descriptors, and archive no-follow checks.

**Tech Stack:** CMake 4.x, Visual Studio 2022/MSVC, C++20/C++23, libarchive, Python `unittest`, CTest.

## Global Constraints

- Preserve existing archive security checks and non-Windows behavior.
- Preserve coordinator ticket, lease, retry, acknowledgement, queue-bound, and revalidation semantics.
- Keep `/Zi` debug information and use parallel compilation.
- Preserve the user's pre-existing edits in `CMakeLists.txt` and `src/scene/play/PlayfieldVisualState.h`.
- Verify both `main` and the selected Windows skin tests.

## File Structure

- `CMakeLists.txt`: owns global MSVC compiler options, including compiler-PDB coordination.
- `tests/cross_platform_release_contract_tests.py`: guards compiler-option partitioning.
- `src/skin/package/SkinArchiveImporter.cpp`: owns portable archive metadata interpretation and Windows private staging security.
- `tests/skin_archive_importer_windows_contract_tests.py`: guards Windows importer security and portability source contracts.
- `src/skin/SkinCommitCoordinator.cpp`: owns bounded terminal delivery and revalidation retry behavior.
- `tests/skin_commit_coordinator_tests.cpp`: exercises coordinator runtime ownership and delivery semantics.

---

### Task 1: Make MSVC compiler-PDB writes parallel-safe

**Files:**
- Modify: `tests/cross_platform_release_contract_tests.py`
- Modify: `CMakeLists.txt:95-103`

**Interfaces:**
- Consumes: the existing top-level `if (MSVC)` compiler-option branch.
- Produces: `/FS` on every MSVC C and C++ compilation launched by this build.

- [ ] **Step 1: Add the failing compiler-option contract**

Add this test method to `CrossPlatformReleaseContractTests`:

```python
def test_msvc_parallel_builds_serialize_compiler_pdb_writes(self):
    msvc = self.cmake.split("if (MSVC)", 1)[1].split("else ()", 1)[0]
    self.assertRegex(msvc, r"add_compile_options\([^\)]*/FS")
```

- [ ] **Step 2: Run the contract and verify RED**

Run `python tests/cross_platform_release_contract_tests.py`.

Expected: FAIL because the MSVC `add_compile_options` call lacks `/FS`.

- [ ] **Step 3: Add the minimal CMake fix**

```cmake
add_compile_options(/Zc:__cplusplus /Zc:preprocessor /wd4819 /utf-8 /bigobj /FS)
```

- [ ] **Step 4: Run the contract and verify GREEN**

Run `python tests/cross_platform_release_contract_tests.py`.

Expected: all tests PASS.

### Task 2: Make archive metadata and Windows security access compile on MSVC

**Files:**
- Modify: `tests/skin_archive_importer_windows_contract_tests.py`
- Modify: `src/skin/package/SkinArchiveImporter.cpp:634-645,876-878,1098,1674-1685,2197-2200`

**Interfaces:**
- Consumes: `archive_entry_filetype()`, `AE_IFMT`, `AE_IFREG`, `AE_IFDIR`, and the initialized `SECURITY_ATTRIBUTES` member.
- Produces: portable inferred file-type values and `SECURITY_ATTRIBUTES *PrivateWindowsSecurity::attributes() const noexcept`.

- [ ] **Step 1: Add failing Windows portability contracts**

```python
def test_windows_build_uses_libarchive_types_and_const_security_access(self):
    source = self.source
    self.assertNotRegex(source, r"\bmode_t\b")
    self.assertIn(
        "SECURITY_ATTRIBUTES *attributes() const noexcept", source
    )
```

- [ ] **Step 2: Run the importer contract and verify RED**

Run `python tests/skin_archive_importer_windows_contract_tests.py`.

Expected: FAIL on native `mode_t` usage and the non-const accessor.

- [ ] **Step 3: Replace POSIX-only type declarations**

```cpp
const auto type =
    static_cast<decltype(archive_entry_filetype(nullptr))>(
        (externalAttributes >> 16U) & AE_IFMT);
const auto fileType = archive_entry_filetype(entry);
const auto expectedType =
    expected.kind == MemberKind::Regular ? AE_IFREG : AE_IFDIR;
```

- [ ] **Step 4: Make the Windows attributes API boundary const-callable**

```cpp
SECURITY_ATTRIBUTES *attributes() const noexcept {
  return const_cast<SECURITY_ATTRIBUTES *>(&attributes_);
}
```

The Windows APIs consume this structure as creation input; no security check or ownership state changes.

- [ ] **Step 5: Run contract and compile verification**

Run:

```powershell
python tests/skin_archive_importer_windows_contract_tests.py
cmake --build cmake-build-debug-visual-studio --target skin_archive_importer_tests --parallel
```

Expected: contract PASS and target build exit 0.

### Task 3: Remove non-portable coordinator noexcept assumptions

**Files:**
- Modify: `src/skin/SkinCommitCoordinator.cpp:22-25,79-106,257-286,300-303`
- Test: `tests/skin_commit_coordinator_tests.cpp`

**Interfaces:**
- Consumes: result/settings types whose nested `std::map` moves are not declared `noexcept` by MSVC's STL.
- Produces: the same coordinator APIs and bounded-delivery semantics without unconditional `std::terminate` on a potentially throwing move.

- [ ] **Step 1: Verify the existing Windows compile failure**

Run `cmake --build cmake-build-debug-visual-studio --target skin_commit_coordinator_tests --parallel`.

Expected: FAIL at the nothrow move/swap assertions in `SkinCommitCoordinator.cpp`.

- [ ] **Step 2: Remove implementation-specific aggregate assertions**

Delete the four namespace-level result assertions, the `FixedBoundedQueue<Value>` move assertion, and the optional revalidation swap assertion. These are false for MSVC because nested node containers may allocate during move construction.

- [ ] **Step 3: Permit delivery moves to report allocation failure**

Remove `noexcept` from:

```cpp
bool push(Value value)
bool recordActivation(SkinActivationClientId client,
                      std::uint64_t coordinatorTicket,
                      CommitActivationResult &&result)
bool recordProfile(SkinActivationClientId client,
                   std::uint64_t coordinatorTicket,
                   SkinProfileCommitResult &&result)
```

Do not change capacity checks, transaction erasure, acknowledgement order, or revalidation replacement order.

- [ ] **Step 4: Build and run coordinator tests**

Run:

```powershell
cmake --build cmake-build-debug-visual-studio --target skin_commit_coordinator_tests --parallel
ctest --test-dir cmake-build-debug-visual-studio -C Debug --output-on-failure -R "^skin_commit_coordinator_tests$"
```

Expected: target build and test PASS.

### Task 4: Verify the complete requested Windows surface

**Files:**
- Verify: `CMakeLists.txt`
- Verify: `src/scene/play/PlayfieldVisualState.h`
- Verify: `src/skin/SkinCommitCoordinator.cpp`
- Verify: `src/skin/package/SkinArchiveImporter.cpp`

**Interfaces:**
- Consumes: Tasks 1-3 plus explicit compression-package discovery and portable playfield timestamp arithmetic.
- Produces: a parallel-buildable Windows application and passing selected skin suites.

- [ ] **Step 1: Regenerate Visual Studio files**

Run `cmake -S . -B cmake-build-debug-visual-studio`.

Expected: configure and generate exit 0.

- [ ] **Step 2: Build application and skin targets in parallel**

```powershell
cmake --build cmake-build-debug-visual-studio --target main skin_commit_coordinator_tests skin_archive_importer_tests skin_package_store_tests skin_package_operation_service_tests gameplay_skin_settings_tests --parallel
```

Expected: exit 0 with no C1041 shared-PDB failures.

- [ ] **Step 3: Run selected CTest cases**

```powershell
ctest --test-dir cmake-build-debug-visual-studio -C Debug --output-on-failure -R "^(skin_commit_coordinator_tests|skin_archive_importer_tests|skin_package_store_tests|skin_package_operation_service_tests|gameplay_skin_settings_tests)$"
```

Expected: five tests PASS and zero fail.

- [ ] **Step 4: Run source-level Windows contracts**

```powershell
python tests/cross_platform_release_contract_tests.py
python tests/skin_archive_importer_windows_contract_tests.py
```

Expected: both suites PASS.

- [ ] **Step 5: Review the final diff**

```powershell
git diff --check
git status --short
git diff -- CMakeLists.txt src/scene/play/PlayfieldVisualState.h src/skin/SkinCommitCoordinator.cpp src/skin/package/SkinArchiveImporter.cpp tests/cross_platform_release_contract_tests.py tests/skin_archive_importer_windows_contract_tests.py
```

Expected: no whitespace errors, no scratch files, and only intended source/test changes beyond the committed design and plan documents.
