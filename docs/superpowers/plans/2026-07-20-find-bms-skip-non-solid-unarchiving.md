# Find BMS Skip Non-Solid Unarchiving Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the persistent `Skip unarchiving for non-solid archives` Find BMS option and require an explicit Keep Files or Delete Files decision for every confirmed hash mismatch.

**Architecture:** Keep source discovery and HTTP drivers unchanged, then route each completed download through a focused, dependency-injected archive workflow. The workflow uses `ArchiveFile` to classify and validate non-solid archives, keeps undecided artifacts in a UUID-named system-temp attempt, and transactionally promotes only validated or explicitly kept files. A pure dialog policy makes the mismatch modal non-dismissible until resolution succeeds.

**Tech Stack:** C++23, `std::filesystem`, existing `ArchiveFile`/7-Zip/unarr/libarchive/miniz backends, existing `bms_parser` hashes, SDL/Yoga UI, nlohmann JSON settings, CMake/CTest, Python source-contract audit.

**Approved design:** `docs/superpowers/specs/2026-07-20-find-bms-skip-non-solid-unarchiving-design.md`

## Global Constraints

- The visible setting name is exactly `Skip unarchiving for non-solid archives`.
- The persistent boolean defaults to `false`; old settings remain false without a schema-version increment.
- Only Find BMS changes; archive import, archive preview, and general library scanning remain unchanged.
- An archive stays packed only when `ArchiveFile` lists at least one file and every file entry is non-solid.
- Any inconclusive list or BMS read falls back to unarchiving; uncertainty is never a hash mismatch.
- A confirmed mismatch stages either the archive or extracted folder and requires Keep Files or Delete Files.
- The mismatch dialog cannot close through Close, Cancel, Escape, Back, overlay input, or normal scene actions before resolution.
- Recursive deletion is restricted to a UUID child of `std::filesystem::temp_directory_path() / "AsoBMaShowFindBms"`.
- Keep/delete failures preserve the pending artifact for retry.
- Keep Files refreshes the library; Delete Files does not.
- Do not deploy. Finish with focused tests and `cmake --build cmake-build-debug --target main -j 6`.

## File Structure

- `src/AppSettings.h`, `src/AppSettingsStore.cpp`: persistent boolean and false default.
- `src/BmsSearchService.h`: options, pending-artifact metadata, exact label, and resolution API.
- `src/bms_search/ArchiveDecision.h/.cpp`: pure direct-use/hash decision over injected archive reads.
- `src/bms_search/DownloadStaging.h/.cpp`: safe temp attempts and transactional keep/delete.
- `src/bms_search/DownloadedArchiveWorkflow.h/.cpp`: direct validation, fallback extraction, commit, and mismatch staging.
- `src/bms_search/DownloadSupport.cpp` and source drivers: download plus options propagation.
- `src/scene/FindBmsDialogPolicy.h/.cpp`: tested modal dismissal/action policy.
- Settings/Main Menu scene files: UI, settings snapshot, and Keep/Delete workers.
- `tests/app_settings_store_tests.cpp`, `tests/find_bms_download_tests.cpp`: automated behavior coverage.
- `scripts/check_find_bms_archive_flow.py`: exact-label and wiring audit.

---

### Task 1: Persist the opt-in preference

**Files:**
- Modify: `src/AppSettings.h:120-135`
- Modify: `src/AppSettingsStore.cpp:110-135,225-245`
- Modify: `tests/app_settings_store_tests.cpp:80-190,750-775`

**Interfaces:**
- Produces: `AppSettings::findBmsSkipUnarchivingForNonSolidArchives: bool`, default `false`.
- Consumes: existing `AppSettingsStore::Save`/`Load` JSON behavior.

- [ ] **Step 1: Add the failing settings tests**

Set the field true in `makeDistinctSettings()`, add this test, and call it from `main()`:

```cpp
void testFindBmsArchivePreferenceDefaultsAndRoundTrips() {
  AppSettings defaults;
  expect(!defaults.findBmsSkipUnarchivingForNonSolidArchives,
         "Find BMS archive preservation defaults off");

  TempDirectory temp;
  const auto missingPath = temp.path() / "missing-find-bms-option.json";
  writeFile(missingPath, R"({"schemaVersion":3})");
  const auto missing = AppSettingsStore::Load(missingPath);
  expect(missing.status == AppSettingsLoadStatus::Loaded &&
             !missing.settings.findBmsSkipUnarchivingForNonSolidArchives,
         "old settings without the field retain the false default");

  const auto enabledPath = temp.path() / "enabled-find-bms-option.json";
  AppSettings enabled;
  enabled.findBmsSkipUnarchivingForNonSolidArchives = true;
  std::string error;
  expect(AppSettingsStore::Save(enabledPath, enabled, error),
         "Find BMS preference saves: " + error);
  const auto loaded = AppSettingsStore::Load(enabledPath);
  expect(loaded.status == AppSettingsLoadStatus::Loaded &&
             loaded.settings.findBmsSkipUnarchivingForNonSolidArchives,
         "Find BMS preference survives a JSON round trip");
  expect(readFile(enabledPath).find(
             "\"findBmsSkipUnarchivingForNonSolidArchives\": true") !=
             std::string::npos,
         "saved JSON contains the Find BMS preference");
}
```

- [ ] **Step 2: Run RED**

```bash
cmake --build cmake-build-debug --target app_settings_store_tests -j 6
```

Expected: compile failure because the member does not exist.

- [ ] **Step 3: Add the member and JSON mapping**

Add beside `archiveChartPreviewEnabled`:

```cpp
bool findBmsSkipUnarchivingForNonSolidArchives = false;
```

Add to `settingsToJson` and `settingsFromJson`:

```cpp
{"findBmsSkipUnarchivingForNonSolidArchives",
 settings.findBmsSkipUnarchivingForNonSolidArchives},

readValue(document, "findBmsSkipUnarchivingForNonSolidArchives",
          settings.findBmsSkipUnarchivingForNonSolidArchives, diagnostics);
```

Do not increment `kCurrentSchemaVersion`.

- [ ] **Step 4: Run GREEN**

```bash
cmake --build cmake-build-debug --target app_settings_store_tests -j 6
ctest --test-dir cmake-build-debug -R foundation_profile_settings --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/AppSettings.h src/AppSettingsStore.cpp tests/app_settings_store_tests.cpp
git commit -m "feat: persist Find BMS archive preference"
```

### Task 2: Add safe staging and pending-artifact resolution

**Files:**
- Create: `src/bms_search/DownloadStaging.h`
- Create: `src/bms_search/DownloadStaging.cpp`
- Create: `src/bms_search/PendingArtifact.cpp`
- Create: `tests/find_bms_download_tests.cpp`
- Modify: `src/BmsSearchService.h:19-80`
- Modify: `src/bms_search/CMakeLists.txt`
- Modify: `CMakeLists.txt:1130-1200,2640-2720`

**Interfaces:**
- Produces: `BmsSearchDownloadOptions`, pending-artifact types, `BmsSearchResult::pendingArtifact`, and `BmsSearchService::resolvePendingArtifact`.
- Produces: `createFindBmsDownloadAttempt`, `commitFindBmsPendingArtifact`, `deleteFindBmsPendingArtifact`.
- Consumes: `uuid::generateV4()` and derived `BMSSEARCH` destinations.

- [ ] **Step 1: Write failing staging tests**

Create a test fixture with `writeText`/`readText` and RAII cleanup for each unique `AsoBMaShowFindBmsTestLibrary-<attempt UUID>` temp parent and any intentionally retained staging root, then add:

```cpp
void testExtractedCommitMergesTransactionally() {
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  const auto downloadRoot = std::filesystem::temp_directory_path() /
      ("AsoBMaShowFindBmsTestLibrary-" +
       attempt->root.filename().string()) /
      "BMSSEARCH";
  const auto destination = downloadRoot / "package";
  writeText(destination / "preserved.txt", "old");
  writeText(destination / "chart.bms", "old chart");
  writeText(attempt->extractedPath / "chart.bms", "new chart");
  BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::ExtractedDirectory,
      .stagingRoot = attempt->root,
      .sourcePath = attempt->extractedPath,
      .downloadRoot = downloadRoot,
      .destinationPath = destination};
  assert(asobmshow::bms_search::commitFindBmsPendingArtifact(artifact, error));
  assert(readText(destination / "chart.bms") == "new chart");
  assert(readText(destination / "preserved.txt") == "old");
  assert(!std::filesystem::exists(attempt->root));
}

void testDeleteRemovesOnlyAttempt() {
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  const auto downloadRoot = std::filesystem::temp_directory_path() /
      ("AsoBMaShowFindBmsTestLibrary-" +
       attempt->root.filename().string()) /
      "BMSSEARCH";
  const auto destination = downloadRoot / "package";
  writeText(destination / "existing.bms", "existing");
  writeText(attempt->extractedPath / "wrong.bms", "wrong");
  BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::ExtractedDirectory,
      .stagingRoot = attempt->root,
      .sourcePath = attempt->extractedPath,
      .downloadRoot = downloadRoot,
      .destinationPath = destination};
  assert(asobmshow::bms_search::deleteFindBmsPendingArtifact(artifact, error));
  assert(!std::filesystem::exists(attempt->root));
  assert(readText(destination / "existing.bms") == "existing");
}

void testUnsafeStagingRootIsRefused() {
  const auto base = asobmshow::bms_search::findBmsStagingBasePath();
  BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::ExtractedDirectory,
      .stagingRoot = base,
      .sourcePath = base / "extracted",
      .downloadRoot = base / "library/BMSSEARCH",
      .destinationPath = base / "library/BMSSEARCH/package"};
  std::string error;
  assert(!asobmshow::bms_search::deleteFindBmsPendingArtifact(artifact, error));
  assert(!error.empty());
}

void testCommitRestoresDestinationWhenSwapFails() {
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  const auto downloadRoot = std::filesystem::temp_directory_path() /
      ("AsoBMaShowFindBmsTestLibrary-" +
       attempt->root.filename().string()) /
      "BMSSEARCH";
  const auto destination = downloadRoot / "package";
  writeText(destination / "chart.bms", "old chart");
  writeText(attempt->extractedPath / "chart.bms", "new chart");
  BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::ExtractedDirectory,
      .stagingRoot = attempt->root,
      .sourcePath = attempt->extractedPath,
      .downloadRoot = downloadRoot,
      .destinationPath = destination};
  auto failCommitSwap = [destination](const std::filesystem::path &from,
                                      const std::filesystem::path &to,
                                      std::error_code &ec) {
    if (from.filename().string().find(".commit-") != std::string::npos &&
        to == destination) {
      ec = std::make_error_code(std::errc::io_error);
      return;
    }
    std::filesystem::rename(from, to, ec);
  };
  assert(!asobmshow::bms_search::commitFindBmsPendingArtifact(
      artifact, error, failCommitSwap));
  assert(readText(destination / "chart.bms") == "old chart");
  assert(std::filesystem::exists(attempt->root));
  assert(!error.empty());
}
```

Call them from `main()`.

- [ ] **Step 2: Register the test and run RED**

Add `find_bms_download_tests` to CMake with `tests/find_bms_download_tests.cpp` and the existing `src/Uuid.cpp`; register it with `asobmashow_register_test`. Do not list the new staging sources yet: the test's `#include "bms_search/DownloadStaging.h"` should be the RED compile failure.

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
```

Expected: missing-interface compile failure.

- [ ] **Step 3: Add the public result contract**

Add to `BmsSearchService.h`:

```cpp
struct BmsSearchDownloadOptions {
  bool skipUnarchivingForNonSolidArchives = false;
};
enum class BmsSearchPendingArtifactKind { Archive, ExtractedDirectory };
struct BmsSearchPendingArtifact {
  BmsSearchPendingArtifactKind kind = BmsSearchPendingArtifactKind::Archive;
  std::filesystem::path stagingRoot;
  std::filesystem::path sourcePath;
  std::filesystem::path downloadRoot;
  std::filesystem::path destinationPath;
};
enum class BmsSearchPendingArtifactDecision { Keep, Delete };
```

Add `std::optional<BmsSearchPendingArtifact> pendingArtifact;` to `BmsSearchResult`, plus:

```cpp
static constexpr const char *kSkipUnarchivingSettingLabel =
    "Skip unarchiving for non-solid archives";
BmsSearchResult resolvePendingArtifact(
    BmsSearchResult result,
    BmsSearchPendingArtifactDecision decision) const;
```

- [ ] **Step 4: Implement the staging API**

Create `DownloadStaging.h`:

```cpp
#pragma once
#include "../BmsSearchService.h"
#include <functional>
#include <system_error>
namespace asobmshow::bms_search {
struct FindBmsDownloadAttempt {
  std::filesystem::path root;
  std::filesystem::path archivePath;
  std::filesystem::path extractedPath;
};
std::filesystem::path findBmsStagingBasePath();
std::optional<FindBmsDownloadAttempt>
createFindBmsDownloadAttempt(const std::string &archiveName,
                             std::string &errorMessage);
using FindBmsRenameOperation = std::function<void(
    const std::filesystem::path &, const std::filesystem::path &,
    std::error_code &)>;
bool commitFindBmsPendingArtifact(const BmsSearchPendingArtifact &artifact,
                                  std::string &errorMessage,
                                  FindBmsRenameOperation renameOperation = {});
bool deleteFindBmsPendingArtifact(const BmsSearchPendingArtifact &artifact,
                                  std::string &errorMessage);
} // namespace asobmshow::bms_search
```

Implementation requirements:

- staging base is exactly `temp_directory_path() / "AsoBMaShowFindBms"`;
- attempt is `<base>/<uuid>`, archive is `<attempt>/<archiveName>`, extracted path is `<attempt>/extracted`;
- reject an empty archive name, `.`, `..`, or any name that differs from its own `filename()`;
- normalized `stagingRoot.parent_path()` must equal the base and `sourcePath` must be a strict descendant;
- `downloadRoot` must be outside the staging base, its filename must be `BMSSEARCH`, destination must be a strict descendant of it, archive parent is `_archives`, and extracted parent is `downloadRoot`;
- archive commit copies to a `.commit-<uuid>` sibling, backs up an existing destination to `.backup-<uuid>`, swaps, restores on failure, then removes staging;
- directory commit copies the old destination to a `.commit-<uuid>` sibling, overlays staging with recursive/overwrite options, then uses the same backup/swap/rollback;
- an empty rename operation uses `std::filesystem::rename`; tests inject the operation to force the final swap to fail after backup and prove restoration;
- Delete treats missing validated staging as success and otherwise removes only `stagingRoot`.

- [ ] **Step 5: Implement result resolution**

In `PendingArtifact.cpp`:

```cpp
BmsSearchResult BmsSearchService::resolvePendingArtifact(
    BmsSearchResult result,
    BmsSearchPendingArtifactDecision decision) const {
  if (!result.pendingArtifact) {
    result.message = "No downloaded files are awaiting a decision.";
    return result;
  }
  const auto artifact = *result.pendingArtifact;
  std::string error;
  const bool ok = decision == BmsSearchPendingArtifactDecision::Keep
      ? asobmshow::bms_search::commitFindBmsPendingArtifact(artifact, error)
      : asobmshow::bms_search::deleteFindBmsPendingArtifact(artifact, error);
  if (!ok) {
    result.message = error.empty() ? "Could not resolve downloaded files."
                                   : error;
    return result;
  }
  result.pendingArtifact.reset();
  if (decision == BmsSearchPendingArtifactDecision::Keep) {
    result.outputPath = artifact.destinationPath;
    result.message = "Mismatched files kept.";
  } else {
    result.outputPath.clear();
    result.message = "Mismatched files deleted.";
  }
  return result;
}
```

- [ ] **Step 6: Add failure/retention assertions and run GREEN**

Add `DownloadStaging.cpp` and `PendingArtifact.cpp` to the test and production source lists. Test that a missing archive source fails commit without replacing an existing destination, leaves staging present, and that `resolvePendingArtifact` retains metadata on failure. Run the injected swap-failure test from Step 1; assert that Delete also succeeds when its already-validated staging attempt is already missing, and that commit refuses a `downloadRoot` inside the staging base.

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
ctest --test-dir cmake-build-debug -R find_bms_download_tests --output-on-failure
```

Expected: pass.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/BmsSearchService.h src/bms_search/CMakeLists.txt src/bms_search/DownloadStaging.h src/bms_search/DownloadStaging.cpp src/bms_search/PendingArtifact.cpp tests/find_bms_download_tests.cpp
git commit -m "feat: stage Find BMS mismatch artifacts"
```

### Task 3: Decide direct use with ArchiveFile

**Files:**
- Create: `src/bms_search/ArchiveDecision.h`
- Create: `src/bms_search/ArchiveDecision.cpp`
- Modify: `src/bms_search/CMakeLists.txt`
- Modify: `tests/find_bms_download_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `DirectArchiveDisposition`, `DirectArchiveDecision`, `ArchiveReaderDependencies`, `decideDownloadedArchive`.
- Consumes: `archive_file::Entry/FileData`, `listEntries`, `readArchiveEntries`, and `bms_parser` hashes.

- [ ] **Step 1: Add failing decision tests**

Use injected fake list/read lambdas to assert:

```cpp
void testDirectArchiveDecision() {
  const std::string chart = "#TITLE TEST\n#00111:01\n";
  const std::vector<unsigned char> bytes(chart.begin(), chart.end());
  const std::string sha256 = bms_parser::sha256(bytes);
  const archive_file::Entry entry{.path = "song/chart.bms",
                                  .directory = false,
                                  .size = bytes.size(),
                                  .solid = false};
  const archive_file::FileData file{.path = entry.path, .bytes = bytes};

  auto match = fakeArchiveReader({entry}, {file});
  assert(decideDownloadedArchive("package.zip", sha256, true, nullptr, match)
             .disposition == DirectArchiveDisposition::KeepArchive);
  auto disabled = fakeArchiveReader({entry}, {file});
  assert(decideDownloadedArchive("package.zip", sha256, false, nullptr,
                                 disabled)
             .disposition == DirectArchiveDisposition::Unarchive);
  auto mismatch = fakeArchiveReader({entry}, {file});
  assert(decideDownloadedArchive("package.zip", std::string(64, '0'), true,
                                 nullptr, mismatch)
             .disposition == DirectArchiveDisposition::HashMismatch);
  auto incomplete = fakeArchiveReader({entry}, {}, true, false);
  assert(decideDownloadedArchive("package.zip", sha256, true, nullptr,
                                 incomplete)
             .disposition == DirectArchiveDisposition::Unarchive);
}
```

Also cover: any solid file -> Unarchive; empty archive -> Unarchive; complete nonempty archive without BMS plus valid hash -> HashMismatch; MD5 match; invalid/no hash -> KeepArchive with `foundBmsFile` reflecting filenames.

- [ ] **Step 2: Link archive dependencies and run RED**

Add `#include "bms_search/ArchiveDecision.h"` and the new test calls, but do not list the not-yet-created `ArchiveDecision.cpp`. The missing header/interface is the intended RED failure.

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
```

Expected: missing-interface compile failure.

- [ ] **Step 3: Define the decision interface**

Create `ArchiveDecision.h` with:

```cpp
#pragma once
#include "../ArchiveFile.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
namespace asobmshow::bms_search {
enum class DirectArchiveDisposition { KeepArchive, Unarchive, HashMismatch };
struct DirectArchiveDecision {
  DirectArchiveDisposition disposition = DirectArchiveDisposition::Unarchive;
  bool foundBmsFile = false;
  std::string message;
};
struct ArchiveReaderDependencies {
  std::function<bool(const std::filesystem::path &,
                     std::vector<archive_file::Entry> &, std::string *,
                     archive_file::PauseCallback)> listEntries;
  std::function<bool(const std::filesystem::path &,
                     const std::vector<std::filesystem::path> &,
                     std::vector<archive_file::FileData> &, std::string *,
                     archive_file::PauseCallback)> readEntries;
};
ArchiveReaderDependencies defaultArchiveReaderDependencies();
DirectArchiveDecision decideDownloadedArchive(
    const std::filesystem::path &archivePath, const std::string &archiveKey,
    bool skipUnarchivingForNonSolidArchives,
    archive_file::PauseCallback pauseCallback,
    const ArchiveReaderDependencies &reader);
} // namespace asobmshow::bms_search
```

- [ ] **Step 4: Implement conclusive decision rules**

Return Unarchive without listing when disabled. Otherwise list through the injected reader; return Unarchive for list failure, no regular files, or any solid file. Collect BMS paths with `bms_chart_file::isBmsChartPath`. With no 32/64-hex key, return KeepArchive and the filename-presence flag. With a valid key and zero BMS candidates, return HashMismatch. Read every candidate in one call; incomplete/failing reads return Unarchive. Hash complete bytes with SHA-256/MD5; return KeepArchive on match and HashMismatch only after all complete reads fail to match. Keep trim/lower/hex helpers local to `ArchiveDecision.cpp`; production dependencies are thin `ArchiveFile` adapters.

Add `ArchiveDecision.cpp`, `ArchiveFile.cpp`, `MinizBridge.c`, `bms_parser.cpp`, `Utils.cpp`, and `path.cpp` to the test target. Mirror `chart_library_scanner_tests` by selecting the available SDL2 target, linking `${COMMON_LIBS}`, Apple `iconv`, and Windows `shell32 ole32`; add `ArchiveDecision.cpp` to the production source list.

- [ ] **Step 5: Run GREEN**

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
ctest --test-dir cmake-build-debug -R find_bms_download_tests --output-on-failure
```

Expected: direct-decision matrix passes.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/bms_search/CMakeLists.txt src/bms_search/ArchiveDecision.h src/bms_search/ArchiveDecision.cpp tests/find_bms_download_tests.cpp
git commit -m "feat: validate non-solid Find BMS archives directly"
```

### Task 4: Route completed downloads through the staged workflow

**Files:**
- Create: `src/bms_search/DownloadedArchiveWorkflow.h`
- Create: `src/bms_search/DownloadedArchiveWorkflow.cpp`
- Modify: `src/bms_search/DownloadSupport.cpp:420-597`
- Modify: `src/bms_search/Internal.h:210-230`
- Modify: `src/bms_search/HorieYuukaDriver.h:10-30`
- Modify: `src/bms_search/HorieYuukaDriver.cpp:230-355`
- Modify: `src/bms_search/PackageSourceDrivers.h:20-31`
- Modify: `src/bms_search/PackageSourceDrivers.cpp:130-205`
- Modify: `src/BmsSearchService.h:55-90`
- Modify: `src/BmsSearchService.cpp:30-260`
- Modify: `src/bms_search/CMakeLists.txt`
- Modify: `tests/find_bms_download_tests.cpp`

**Interfaces:**
- Produces: `ExtractedArchiveDisposition`, `ExtractedArchiveDecision`, `decideExtractedArchive`, `DownloadedArchiveWorkflowRequest`, `DownloadedArchiveWorkflowDependencies`, and `processDownloadedArchive`.
- Changes: all download paths consume the same `BmsSearchDownloadOptions` value.
- Consumes: staging, direct-decision, extracted hash validation, and pending metadata.

- [ ] **Step 1: Add failing workflow tests**

Add dependency fakes and these explicit tests:

```cpp
void testWorkflowKeepsDirectArchiveWithoutExtraction();
void testWorkflowStagesDirectArchiveMismatch();
void testWorkflowCommitsFallbackExtractionMatch();
void testWorkflowStagesFallbackExtractionMismatch();
void testWorkflowRejectsInconclusiveExtractedValidation();
void testExtractedDecisionMatchesSha256AndMd5();
void testExtractedDecisionDistinguishesMismatchAndInconclusive();
```

Implement each body with an actual `FindBmsDownloadAttempt`, a `DownloadedArchiveWorkflowDependencies` object, and assertions:

- direct success: decision is `KeepArchive`, extraction is never called, commit kind is `Archive`, status is `Downloaded`, final output is `_archives/<name>`, message is `Downloaded BMS archive.`;
- direct mismatch: no commit, status `HashMismatch`, empty output, pending kind `Archive`;
- extraction match: decision `Unarchive`, extraction writes the staging folder, extracted-decision dependency returns `Match`, commit kind `ExtractedDirectory`, message `Downloaded and unarchived BMS archive.`;
- extraction mismatch: no commit, source archive removed, pending kind `ExtractedDirectory`, source is `attempt.extractedPath`.
- inconclusive extracted validation: no commit or pending artifact, status `DownloadFailed`, and the validation error is preserved;
- real extracted validator: SHA-256 and MD5 fixtures match, complete nonmatching BMS files are `HashMismatch`, an invalid/no key succeeds while reporting filename presence, and a missing/unreadable root is `Inconclusive`.

- [ ] **Step 2: Add the source and run RED**

Add `#include "bms_search/DownloadedArchiveWorkflow.h"` and the seven new test calls, but do not list the not-yet-created workflow source. The missing header/interface is the intended RED failure.

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
```

Expected: missing workflow interface compile failure.

- [ ] **Step 3: Define the workflow seam**

Create `DownloadedArchiveWorkflow.h`:

```cpp
#pragma once
#include "ArchiveDecision.h"
#include "DownloadStaging.h"
#include <atomic>
#include <functional>
#include <string>
namespace asobmshow::bms_search {
enum class ExtractedArchiveDisposition { Match, HashMismatch, Inconclusive };
struct ExtractedArchiveDecision {
  ExtractedArchiveDisposition disposition =
      ExtractedArchiveDisposition::Inconclusive;
  bool foundBmsFile = false;
  std::string message;
};
ExtractedArchiveDecision decideExtractedArchive(
    const std::filesystem::path &root, const std::string &archiveKey);
struct DownloadedArchiveWorkflowRequest {
  FindBmsDownloadAttempt attempt;
  std::filesystem::path downloadRoot;
  std::string archiveName;
  std::string storageKey;
  std::string archiveKey;
  BmsSearchDownloadOptions options;
};
struct DownloadedArchiveWorkflowDependencies {
  std::function<DirectArchiveDecision(
      const std::filesystem::path &, const std::string &, bool,
      archive_file::PauseCallback)> decideArchive;
  std::function<bool(const std::filesystem::path &,
                     const std::filesystem::path &, std::string &,
                     BmsSearchDownloadProgressCallback)> extractArchive;
  std::function<ExtractedArchiveDecision(
      const std::filesystem::path &, const std::string &)>
      decideExtracted;
  std::function<bool(const BmsSearchPendingArtifact &, std::string &)>
      commitArtifact;
};
bool processDownloadedArchive(
    const DownloadedArchiveWorkflowRequest &request,
    std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    BmsSearchResult &result,
    const DownloadedArchiveWorkflowDependencies &dependencies);
} // namespace asobmshow::bms_search
```

- [ ] **Step 4: Implement the state machine**

Implement these complete transitions:

```text
KeepArchive -> commit attempt.archivePath to downloadRoot/_archives/archiveName
            -> Downloaded + final archive output + direct success/warning
HashMismatch -> HashMismatch + pending Archive + empty output
Unarchive -> extract to attempt.extractedPath
          -> validate the extracted tree with decideExtractedArchive
          -> mismatch: remove source archive, pending ExtractedDirectory
          -> inconclusive: DownloadFailed, no pending artifact
          -> match/no-key: commit to downloadRoot/request.storageKey
          -> Downloaded + final directory output + unarchived success/warning
```

Implement `decideExtractedArchive` as a complete recursive scan: traversal errors are `Inconclusive`; after a complete scan, invalid/no hash returns `Match` with filename presence; valid hashes compare every readable BMS file; a complete scan with no match is `HashMismatch`; BMS read failures without a match are `Inconclusive`. Convert bytes to `std::string` for the existing MD5 API. Keep hash normalization helpers local to this source so the focused test target does not depend on the larger source-driver implementation. Commit failure becomes `DownloadFailed`. Confirmed mismatch text says the selected chart is absent and the user must keep or delete the files. Cancellation during inspection/extraction becomes `DownloadFailed` with `Lookup cancelled.` and no pending artifact.

Emit `Inspecting downloaded archive` before direct inspection, `Validating archive contents` while hashing direct BMS entries, and `Unarchiving archive` before the fallback extraction. Add `DownloadedArchiveWorkflow.cpp` to both production and test source lists after the interface and implementation exist.

- [ ] **Step 5: Replace direct-to-library extraction**

Change `downloadAndExtractArchive` to accept `const BmsSearchDownloadOptions &options`. Create a temp attempt before download:

```cpp
std::string stagingError;
const auto attempt = createFindBmsDownloadAttempt(archiveName, stagingError);
if (!attempt) {
  result.status = BmsSearchResult::Status::DownloadFailed;
  result.message = stagingError;
  return false;
}
auto cleanup = makeScopeExit([root = attempt->root] {
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
});
```

Use `attempt->archivePath` for HTTP, Google Drive confirmation, HTML checks, and debug artifacts. Put the already-computed `key = storageKeyFromArchiveName(archiveName)` into `request.storageKey`, so the pure workflow does not depend on `Common.cpp`. Construct the production dependencies here: adapt `decideDownloadedArchive` with `defaultArchiveReaderDependencies()`, use the existing extraction function, use `decideExtractedArchive`, and use `commitFindBmsPendingArtifact`. Then call `processDownloadedArchive`. Dismiss cleanup only for `result.pendingArtifact`; successful commits already remove staging. Preserve the existing `downloadedArchive` signal used to stop package fallbacks after a response was downloaded.

- [ ] **Step 6: Propagate one options snapshot through every source**

Append `BmsSearchDownloadOptions options` to both public `BmsSearchService` download methods, after their existing optional callback/title/artist parameters. Append `const BmsSearchDownloadOptions &options` immediately before the result parameter in `EndlessDreamSourcesDriver::tryDownloadByMd5`, `HorieYuukaDriver::tryDownload`, and `HorieYuukaDriver::downloadCandidateById`. In `downloadAndExtractArchive`, place the options reference immediately before `BmsSearchResult &result` and retain the existing suggested-name and downloaded-signal tail parameters.

Only the public service methods get default `{}` values. Pass the same options into package, direct BMS Search, automatic Horie, and selected Horie candidate calls.

- [ ] **Step 7: Run GREEN and compile main**

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
ctest --test-dir cmake-build-debug -R find_bms_download_tests --output-on-failure
cmake --build cmake-build-debug --target main -j 6
```

Expected: workflow tests pass and all driver signatures compile.

- [ ] **Step 8: Commit**

```bash
git add src/BmsSearchService.h src/BmsSearchService.cpp src/bms_search/CMakeLists.txt src/bms_search/Internal.h src/bms_search/DownloadSupport.cpp src/bms_search/DownloadedArchiveWorkflow.h src/bms_search/DownloadedArchiveWorkflow.cpp src/bms_search/HorieYuukaDriver.h src/bms_search/HorieYuukaDriver.cpp src/bms_search/PackageSourceDrivers.h src/bms_search/PackageSourceDrivers.cpp tests/find_bms_download_tests.cpp
git commit -m "feat: keep eligible Find BMS archives packed"
```

### Task 5: Add the setting UI and mandatory mismatch actions

**Files:**
- Create: `src/scene/FindBmsDialogPolicy.h`
- Create: `src/scene/FindBmsDialogPolicy.cpp`
- Create: `scripts/check_find_bms_archive_flow.py`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `src/scene/SettingsScene.h:120-170`
- Modify: `src/scene/SettingsSceneLayout.cpp:60-125,2050-2120`
- Modify: `src/scene/SettingsSceneControls.cpp:130-225,320-430`
- Modify: `src/scene/MainMenuScene.h:340-370,730-755`
- Modify: `src/scene/MainMenuScene.cpp:2450-2570,7090-7540,10480-10650`
- Modify: `tests/find_bms_download_tests.cpp`
- Modify: `CMakeLists.txt:500-560`

**Interfaces:**
- Produces: `FindBmsDialogPolicy`, `findBmsDialogPolicy`, and `MainMenuScene::startFindBmsPendingArtifactResolution`.
- Consumes: persisted setting, download options, pending resolution, and `startLibraryRefresh()`.

- [ ] **Step 1: Add failing dialog-policy tests**

```cpp
void testPendingMismatchCannotDismiss() {
  BmsSearchResult result;
  result.status = BmsSearchResult::Status::HashMismatch;
  result.pendingArtifact = BmsSearchPendingArtifact{};
  const auto pending = findBmsDialogPolicy(false, result);
  assert(!pending.canDismiss);
  assert(!pending.showCloseOrCancel);
  assert(pending.showPendingActions);
  assert(!pending.showNormalResultActions);

  const auto resolving = findBmsDialogPolicy(true, result);
  assert(!resolving.canDismiss);
  assert(!resolving.showCloseOrCancel);
  assert(!resolving.showPendingActions);

  result.pendingArtifact.reset();
  const auto resolved = findBmsDialogPolicy(false, result);
  assert(resolved.canDismiss);
  assert(resolved.showCloseOrCancel);
  assert(!resolved.showPendingActions);
}
```

Include the not-yet-created `FindBmsDialogPolicy.h` and call this from `main()`. Do not add `FindBmsDialogPolicy.cpp` to the test source list yet; the missing header/interface is the intended RED failure.

- [ ] **Step 2: Add a failing source-contract audit**

Create `scripts/check_find_bms_archive_flow.py` following the existing Main Menu audits. Read `SettingsSceneLayout.cpp` and `MainMenuScene.cpp`, accumulate failures, and require:

```python
require(
    "BmsSearchService::kSkipUnarchivingSettingLabel" in settings_source,
    "Settings must use the canonical Find BMS option label",
)
require(
    settings_source.count("findBmsSkipUnarchivingForNonSolidArchives") >= 2,
    "Settings must toggle the persisted Find BMS option",
)
require(
    main_menu_source.count("skipUnarchivingForNonSolidArchives") >= 2,
    "automatic and candidate downloads must capture the option",
)
require(
    "startFindBmsPendingArtifactResolution(" in main_menu_source
    and "BmsSearchPendingArtifactDecision::Keep" in main_menu_source
    and "BmsSearchPendingArtifactDecision::Delete" in main_menu_source,
    "Find BMS mismatch UI must expose Keep and Delete",
)
require(
    "findBmsDialogPolicy(findBmsJobRunning.load(), findBmsResult)"
    in main_menu_source,
    "Find BMS dismissal must use the tested dialog policy",
)
```

Register `find_bms_archive_flow_audit` with Python beside the existing Main Menu audits.

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
python3 scripts/check_find_bms_archive_flow.py .
```

Expected: missing dialog-policy compile failure and missing-wiring audit failures.

- [ ] **Step 3: Implement the pure dialog policy**

Create `FindBmsDialogPolicy.h/.cpp`:

```cpp
struct FindBmsDialogPolicy {
  bool canDismiss = false;
  bool showCloseOrCancel = false;
  bool showPendingActions = false;
  bool showNormalResultActions = false;
};
FindBmsDialogPolicy findBmsDialogPolicy(bool running,
                                        const BmsSearchResult &result);

FindBmsDialogPolicy findBmsDialogPolicy(bool running,
                                        const BmsSearchResult &result) {
  const bool pending = result.pendingArtifact.has_value();
  return {.canDismiss = !running && !pending,
          .showCloseOrCancel = !pending,
          .showPendingActions = pending && !running,
          .showNormalResultActions = !pending};
}
```

Add production source to `src/scene/CMakeLists.txt`. Also add `FindBmsDialogPolicy.cpp` to `find_bms_download_tests` now that the source exists.

- [ ] **Step 4: Add the exact Settings -> Misc control**

Add `findBmsSkipUnarchivingModeText`/`Button` members and clear them in both Settings pointer-reset paths. In `refreshSettingsText`, display `On`/`Off` and Success/Info tones.

Add after Archive Preview:

```cpp
auto *findBmsArchiveControls = new View();
findBmsArchiveControls->setFlexDirection(FlexDirection::Column);
findBmsArchiveControls->setGap(metrics.compact ? 12.0f : 16.0f);
findBmsArchiveControls->setAlignItems(YGAlignFlexStart);
findBmsSkipUnarchivingModeText =
    makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
             TextView::CENTER, TextView::MIDDLE);
findBmsSkipUnarchivingModeButton =
    makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                     findBmsSkipUnarchivingModeText, ui_theme::lime());
findBmsSkipUnarchivingModeButton->setOnClickListener([this]() {
  context.settings.findBmsSkipUnarchivingForNonSolidArchives =
      !context.settings.findBmsSkipUnarchivingForNonSolidArchives;
  persistSettings();
});
findBmsArchiveControls->addView(findBmsSkipUnarchivingModeButton);
cardsColumn->addView(makeCard(
    metrics, BmsSearchService::kSkipUnarchivingSettingLabel,
    "Keep readable non-solid Find BMS downloads as archives. Solid archives "
    "are still unarchived.",
    findBmsArchiveControls, metrics.modeCardHeight, metrics.cardsWidth));
```

- [ ] **Step 5: Capture the setting for both workers**

Before automatic and candidate workers, construct and capture by value:

```cpp
const BmsSearchDownloadOptions downloadOptions{
    .skipUnarchivingForNonSolidArchives =
        context.settings.findBmsSkipUnarchivingForNonSolidArchives};
```

Pass it to `service.findAndDownload` and `service.downloadCandidate`. Do not read `context.settings` inside worker lambdas.

- [ ] **Step 6: Add Keep/Delete buttons and a resolution worker**

Add button/text members, pointer cleanup, footer construction, and handlers. The visible button labels are exactly `Keep Files` and `Delete Files`:

```cpp
findBmsKeepFilesButton->setOnClickListener([this]() {
  startFindBmsPendingArtifactResolution(
      BmsSearchPendingArtifactDecision::Keep);
});
findBmsDeleteFilesButton->setOnClickListener([this]() {
  startFindBmsPendingArtifactResolution(
      BmsSearchPendingArtifactDecision::Delete);
});
```

`startFindBmsPendingArtifactResolution` joins the completed download thread, copies `findBmsResult`, sets `findBmsJobRunning`, and invokes `BmsSearchService::resolvePendingArtifact` in `findBmsThread`. Publish through `pendingFindBmsResult`. Do not allow cancellation during commit/delete.

In `refreshFindBmsModal`, use `findBmsDialogPolicy`: pending and idle shows only Keep Files/Delete Files; pending and resolving shows no action; Source/Search/Refresh and Close/Cancel are hidden while pending. Show `Keeping files`/`Deleting files` during work and the result message afterward.

Guard every programmatic dismissal:

```cpp
void MainMenuScene::hideFindBmsModal() {
  if (findBmsModalRoot == nullptr ||
      !findBmsDialogPolicy(findBmsJobRunning.load(), findBmsResult)
           .canDismiss) {
    return;
  }
  findBmsModalRoot->setVisible(false);
}
```

`BlockingOverlayView` already consumes key, pointer, and overlay input. Do not add another Escape/Back route. In `applyFindBmsUpdates`, refresh for `Downloaded` and for resolved `HashMismatch` with no pending artifact and nonempty `outputPath`; do not refresh after Delete or resolution failure.

- [ ] **Step 7: Run GREEN**

```bash
cmake --build cmake-build-debug --target find_bms_download_tests app_settings_store_tests -j 6
ctest --test-dir cmake-build-debug -R 'find_bms_download_tests|foundation_profile_settings|find_bms_archive_flow_audit' --output-on-failure
```

Expected: policy, audit, and settings tests pass.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt scripts/check_find_bms_archive_flow.py src/scene/CMakeLists.txt src/scene/FindBmsDialogPolicy.h src/scene/FindBmsDialogPolicy.cpp src/scene/SettingsScene.h src/scene/SettingsSceneLayout.cpp src/scene/SettingsSceneControls.cpp src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp tests/find_bms_download_tests.cpp
git commit -m "feat: add Find BMS archive and mismatch controls"
```

### Task 6: Complete regression and build verification

**Files:**
- Verification only; fix any failure in the owning file from Tasks 1-5 before proceeding.

**Interfaces:**
- Consumes: all feature tasks and repository build/test contracts.
- Produces: verified desktop feature with no Firebase upload.

- [ ] **Step 1: Run focused tests**

```bash
cmake --build cmake-build-debug --target app_settings_store_tests find_bms_download_tests -j 6
ctest --test-dir cmake-build-debug -R 'foundation_profile_settings|find_bms_download_tests|find_bms_archive_flow_audit' --output-on-failure
```

Expected: pass.

- [ ] **Step 2: Run archive/library regressions**

```bash
cmake --build cmake-build-debug --target chart_library_scanner_tests main_menu_library_tests -j 6
ctest --test-dir cmake-build-debug -R 'chart_library_scanner_tests|main_menu_library_tests' --output-on-failure
```

Expected: pass.

- [ ] **Step 3: Run the required desktop compile check**

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: `main` builds. Do not run Firebase deployment scripts.

- [ ] **Step 4: Inspect final state**

```bash
git diff --check
git status --short
git log --oneline -6
```

Expected: no whitespace errors; only intentional feature files are present; task commits are visible.
