# Find BMS Storage Identity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent unrelated same-named Find BMS packages from sharing storage, preserve retained archive extensions, remove stale same-ID archive variants, and keep explicit desktop fallback folders removable.

**Architecture:** Introduce a focused pure naming component that turns a display archive name and stable identity seed into a readable, bounded storage key and extension-preserving archive filename. Feed provider IDs or chart hashes into that component at the download boundary, then make the validated commit layer remove every supported archive variant with the same storage key. Restrict built-in fallback-row treatment to Android, the only platform that synthesizes the row.

**Tech Stack:** C++23, `std::filesystem`, existing `ArchiveFile` extension helpers, existing `bms_parser` SHA-256, SQLite repository tests, CMake/CTest, Python source-flow audit.

**Approved design:** `docs/superpowers/specs/2026-07-21-find-bms-storage-identity-design.md`

## Global Constraints

- Generated storage keys use `<sanitized-name>--<16-hex-id>`.
- The complete retained archive filename is at most 128 bytes.
- Identity suffixes and archive extensions are never truncated.
- Horie uses its stable candidate file ID; other sources use the chart hash, then a URL fallback.
- Same-name downloads with different identity seeds must never share destinations.
- Cleanup removes only supported archive files with an exact extension-free storage-key match.
- Cleanup remains best effort after a successful destination commit.
- Existing unsuffixed downloads are not migrated.
- Android keeps its built-in fallback; explicit desktop and iOS entries remain removable.
- Do not reply to or resolve GitHub review threads.
- Do not deploy. Verify with all CTest targets and `scripts/ios_firebase_deploy.sh --build-only`.

## File Structure

- `src/bms_search/DownloadStorageIdentity.h/.cpp`: pure safe storage-key and archive-filename construction.
- `src/bms_search/DownloadSupport.cpp`, `Internal.h`, and provider call sites: choose and propagate the identity seed.
- `src/bms_search/DownloadStaging.cpp`: best-effort cleanup of every same-key supported archive variant.
- `src/repositories/ChartRepository.cpp`: Android-only built-in fallback presentation.
- `tests/find_bms_download_tests.cpp`: naming, extension, collision, and cleanup regressions.
- `tests/chart_repository_tests.cpp`: explicit default-path row removability regression.
- `scripts/check_find_bms_archive_flow.py`: source contract for provider identity propagation.
- `src/bms_search/CMakeLists.txt`, `CMakeLists.txt`: compile the focused naming component in the app and test target.

---

### Task 1: Keep explicit fallback rows removable

**Files:**
- Modify: `tests/chart_repository_tests.cpp`
- Modify: `src/repositories/ChartRepository.cpp`

**Interfaces:**
- Consumes: `ChartRepository::Session::SelectEffectiveEntries()`.
- Produces: explicit non-Android rows with `ChartEntry::removable == true`; Android synthesized fallback behavior is unchanged.

- [ ] **Step 1: Write the failing repository test**

In `testFindBmsDownloadEntrySelectionLifecycle()`, require the explicitly inserted fallback row to remain removable on non-Android builds:

```cpp
#if !TARGET_OS_ANDROID
  assert(fallbackEntry != nullptr);
  assert(fallbackEntry->removable);
#endif
```

- [ ] **Step 2: Run RED**

```bash
cmake --build cmake-build-debug --target chart_repository_tests -j 6
./cmake-build-debug/chart_repository_tests
```

Expected: assertion failure because `selectEffectiveEntries()` currently marks every stored default-path row non-removable.

- [ ] **Step 3: Limit built-in row decoration to Android**

Move `hasDefaultEntry`, the loop that sets `removable = false`, and fallback synthesis under `#if TARGET_OS_ANDROID`. Leave non-Android `selectAllEntries()` results unchanged.

- [ ] **Step 4: Run GREEN**

```bash
cmake --build cmake-build-debug --target chart_repository_tests -j 6
./cmake-build-debug/chart_repository_tests
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/repositories/ChartRepository.cpp tests/chart_repository_tests.cpp
git commit -m "fix: keep explicit fallback folders removable"
```

### Task 2: Generate collision-resistant extension-preserving names

**Files:**
- Create: `src/bms_search/DownloadStorageIdentity.h`
- Create: `src/bms_search/DownloadStorageIdentity.cpp`
- Modify: `src/bms_search/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `tests/find_bms_download_tests.cpp`
- Modify: `src/bms_search/Internal.h`
- Modify: `src/bms_search/DownloadSupport.cpp`
- Modify: `src/bms_search/HorieYuukaDriver.cpp`
- Modify: `src/bms_search/PackageSourceDrivers.cpp`
- Modify: `scripts/check_find_bms_archive_flow.py`

**Interfaces:**
- Produces: `FindBmsStorageNames findBmsStorageNames(std::string_view archiveName, std::string_view fallbackExtension, std::string_view identitySeed)`.
- Produces: `FindBmsStorageNames::storageKey` and `FindBmsStorageNames::archiveName`.
- Changes: `downloadAndExtractArchive(..., const std::string &suggestedArchiveName, const std::string &storageIdentity = "", bool *downloadedArchive = nullptr)`.
- Consumes: `archive_file::archiveExtensionFromName()` and `bms_parser::sha256()`.

- [ ] **Step 1: Write the failing naming tests**

Include `bms_search/DownloadStorageIdentity.h` and add tests that express the new API:

```cpp
void testStorageNamesDistinguishSameNamedPackages() {
  const auto first = asobmshow::bms_search::findBmsStorageNames(
      "song.zip", ".zip", "provider:file-a");
  const auto second = asobmshow::bms_search::findBmsStorageNames(
      "song.zip", ".zip", "provider:file-b");
  const auto repeated = asobmshow::bms_search::findBmsStorageNames(
      "song.zip", ".zip", "provider:file-a");
  assert(first.storageKey != second.storageKey);
  assert(first.archiveName != second.archiveName);
  assert(first.storageKey == repeated.storageKey);
  assert(first.archiveName == repeated.archiveName);
  assert(first.archiveName == first.storageKey + ".zip");
}

void testStorageNamesPreserveLongAndCompoundExtensions() {
  const auto zip = asobmshow::bms_search::findBmsStorageNames(
      std::string(180, 'a'), ".zip", "zip-id");
  assert(zip.archiveName.ends_with(".zip"));
  assert(zip.archiveName.size() <= 128);
  const auto tar = asobmshow::bms_search::findBmsStorageNames(
      std::string(180, 'b') + ".tar.gz", ".zip", "tar-id");
  assert(tar.archiveName.ends_with(".tar.gz"));
  assert(tar.archiveName.size() <= 128);
}
```

- [ ] **Step 2: Run RED**

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
```

Expected: compile failure because `DownloadStorageIdentity.h` and the naming API do not exist.

- [ ] **Step 3: Implement the pure naming component**

Define:

```cpp
struct FindBmsStorageNames {
  std::string storageKey;
  std::string archiveName;
};

FindBmsStorageNames findBmsStorageNames(
    std::string_view archiveName, std::string_view fallbackExtension,
    std::string_view identitySeed);
```

The implementation must:

1. Prefer the recognized extension on `archiveName`, otherwise use a recognized `fallbackExtension`, otherwise use `.archive`.
2. Remove the chosen extension from the readable base when present.
3. SHA-256 hash `identitySeed`, using the archive name only as a defensive empty-seed fallback, and keep the first 16 hex characters.
4. Sanitize the base with the current allowed characters and cap it so `base + "--" + id + extension` is at most 128 bytes.
5. Return `archive` when sanitization empties the base.

Add `DownloadStorageIdentity.cpp` to both the app target and `find_bms_download_tests`.

- [ ] **Step 4: Run the naming tests GREEN**

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
./cmake-build-debug/find_bms_download_tests
```

Expected: pass.

- [ ] **Step 5: Propagate provider identity into download naming**

Extend `downloadAndExtractArchive` with `storageIdentity`. Choose the seed as:

```cpp
const std::string identitySeed =
    !storageIdentity.empty()
        ? storageIdentity
        : (!archiveKey.empty()
               ? archiveKey
               : (!result.downloadUrl.empty() ? result.downloadUrl
                                               : downloadUrl));
```

Build `FindBmsStorageNames` after `preferredArchiveName()`, use its
`archiveName` for staging and retained archives, and its `storageKey` for
extracted destinations. Pass `candidate.id` from Horie and the chart/package
hash from package sources. BMS Search may use the existing `archiveKey`
fallback.

- [ ] **Step 6: Extend and run the source-flow audit**

Update `check_find_bms_archive_flow.py` to read `HorieYuukaDriver.cpp`,
`DownloadSupport.cpp`, and `PackageSourceDrivers.cpp`, then require
`candidate.id` at the Horie download call and `storageIdentity` plus the
`archiveKey`/URL fallback at the download boundary.

```bash
python3 scripts/check_find_bms_archive_flow.py .
```

Expected: `Find BMS archive-flow audit passed`.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/bms_search/CMakeLists.txt \
  src/bms_search/DownloadStorageIdentity.h \
  src/bms_search/DownloadStorageIdentity.cpp \
  src/bms_search/Internal.h src/bms_search/DownloadSupport.cpp \
  src/bms_search/HorieYuukaDriver.cpp src/bms_search/PackageSourceDrivers.cpp \
  scripts/check_find_bms_archive_flow.py tests/find_bms_download_tests.cpp
git commit -m "feat: give Find BMS downloads stable storage IDs"
```

### Task 3: Remove every same-ID archive variant

**Files:**
- Modify: `tests/find_bms_download_tests.cpp`
- Modify: `src/bms_search/DownloadStaging.cpp`

**Interfaces:**
- Consumes: validated `BmsSearchPendingArtifact::downloadRoot`, `storageKey`, and `destinationPath`.
- Produces: best-effort removal of supported `_archives/<storageKey><extension>` variants other than the installed destination.

- [ ] **Step 1: Write the failing changed-extension cleanup test**

Add a test that stages extracted `package` files, creates
`_archives/package.7z`, sets the current artifact archive name to
`package.zip`, commits, and asserts the `.7z` file is gone while the extracted
destination exists.

- [ ] **Step 2: Run RED**

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
./cmake-build-debug/find_bms_download_tests
```

Expected: assertion failure because only `_archives/package.zip` is removed.

- [ ] **Step 3: Implement exact-key archive enumeration**

After the destination swap, iterate `downloadRoot / "_archives"` with an
`std::error_code`. For each regular file:

1. Get its supported extension with `archive_file::archiveExtensionFromName`.
2. Strip the complete extension from the filename.
3. Require an exact match with `artifact.storageKey`.
4. Skip `artifact.destinationPath`.
5. Remove the path with an ignored cleanup error.

Keep the existing exact opposite-form removal. Never return failure from this
post-commit cleanup.

- [ ] **Step 4: Run GREEN**

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
./cmake-build-debug/find_bms_download_tests
```

Expected: pass, including current-extension cleanup and forced cleanup-failure coverage.

- [ ] **Step 5: Commit**

```bash
git add src/bms_search/DownloadStaging.cpp tests/find_bms_download_tests.cpp
git commit -m "fix: clear same-ID archive variants"
```

### Task 4: Full verification and publication

**Files:**
- Verify only.

**Interfaces:**
- Consumes: all earlier tasks.
- Produces: a clean, pushed `feature/skip-unzip` head for PR #78.

- [ ] **Step 1: Run all desktop tests**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Expected: all tests pass.

- [ ] **Step 2: Run the iOS build-only path**

```bash
scripts/ios_firebase_deploy.sh --build-only
```

Expected: `** BUILD SUCCEEDED **`; no upload occurs.

- [ ] **Step 3: Audit the branch**

```bash
git status -sb
git diff --check
git log --oneline origin/feature/skip-unzip..HEAD
```

Expected: clean worktree, no whitespace errors, only intended commits ahead.

- [ ] **Step 4: Push and confirm PR head**

```bash
git push -u origin feature/skip-unzip
gh pr view 78 --json url,headRefName,headRefOid
```

Expected: PR #78 points at local `HEAD`.
