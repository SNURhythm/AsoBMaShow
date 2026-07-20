# Find BMS Download Folder Selection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let users persistently select a writable BMS Library entry for Find BMS downloads, retain `Documents/BMS` as the no-entry fallback, and restore current/total byte progress in the Find BMS modal.

**Architecture:** Store the selection on the chart repository's `entries` row so it remains paired with the path and iOS bookmark. Repository operations enforce one eligible selection, deterministic first-entry selection, and transactional promotion after deletion; Settings only presents and invokes those operations. Find BMS resolves the repository selection immediately before either download path, while a small pure presentation unit formats live byte progress for the modal.

**Tech Stack:** C++23, SQLite, SDL/Yoga views, CMake/CTest, Python source-flow audit, iOS security-scoped bookmarks.

## Global Constraints

- `Documents/BMS` is an implicit fallback, not an eligible manual selection.
- The oldest eligible manually added folder is selected when no valid selection exists.
- Removing the selected folder promotes the oldest remaining eligible folder atomically.
- Android `@androidtree@` entries remain visible but are not selectable because Find BMS cannot write to them.
- An inaccessible explicitly selected folder reports its real error and never silently redirects to the fallback.
- Both automatic and candidate Find BMS downloads use the same selected destination.
- The progress status shows current and total sizes when the server supplies a total, and current size only otherwise.
- Do not restore the removed scrolling Find BMS log.
- Preserve the non-dismissible hash-mismatch Keep Files/Delete Files flow.
- Do not upload or install an iOS build during verification.

---

### Task 1: Persist and normalize the selected library entry

**Files:**
- Modify: `tests/chart_repository_tests.cpp`
- Modify: `src/repositories/ChartRepository.h`
- Modify: `src/repositories/ChartRepository.cpp`

**Interfaces:**
- Consumes: existing normalized entry paths, `ChartRepository::DefaultBmsFolderPath()`, `ChartStorageIdentity`, and SQLite transaction helpers.
- Produces: `ChartEntry::findBmsDownloadFolder`, `ChartEntry::findBmsDownloadEligible`, `ChartRepository::Session::SetFindBmsDownloadEntry(const std::filesystem::path &)`, and `ChartRepository::Session::SelectFindBmsDownloadEntry()`.

- [ ] **Step 1: Write failing repository lifecycle tests**

Add helpers and tests to `tests/chart_repository_tests.cpp`:

```cpp
const ChartEntry *entryAtPath(const std::vector<ChartEntry> &entries,
                              const std::filesystem::path &path) {
  const auto it = std::find_if(
      entries.begin(), entries.end(), [&path](const ChartEntry &entry) {
        return std::filesystem::path(entry.path).lexically_normal() ==
               path.lexically_normal();
      });
  return it == entries.end() ? nullptr : &*it;
}

void testFindBmsDownloadEntrySelectionLifecycle() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);

  const auto fallback = ChartRepository::DefaultBmsFolderPath();
  const auto first = temporary.path() / "first";
  const auto second = temporary.path() / "second";
  assert(session->InsertEntry(fallback));
  assert(!session->SelectFindBmsDownloadEntry());

  assert(session->InsertEntry(first, "first-bookmark"));
  auto selected = session->SelectFindBmsDownloadEntry();
  assert(selected && std::filesystem::path(selected->path) == first);
  assert(selected->findBmsDownloadFolder);
  assert(selected->findBmsDownloadEligible);

  assert(session->InsertEntry(first, "updated-bookmark"));
  assert(session->InsertEntry(second, "second-bookmark"));
  selected = session->SelectFindBmsDownloadEntry();
  assert(selected && std::filesystem::path(selected->path) == first);
  assert(selected->iosBookmark == "updated-bookmark");

  assert(session->SetFindBmsDownloadEntry(second));
  selected = session->SelectFindBmsDownloadEntry();
  assert(selected && std::filesystem::path(selected->path) == second);

  int removedChartCount = -1;
  assert(session->DeleteEntryAndChartMetaInDirectory(second,
                                                     removedChartCount));
  selected = session->SelectFindBmsDownloadEntry();
  assert(selected && std::filesystem::path(selected->path) == first);

  assert(session->DeleteEntryAndChartMetaInDirectory(first,
                                                     removedChartCount));
  assert(!session->SelectFindBmsDownloadEntry());
  const auto entries = session->SelectEffectiveEntries();
  const auto *fallbackEntry = entryAtPath(entries, fallback);
  if (fallbackEntry != nullptr) {
    assert(!fallbackEntry->findBmsDownloadFolder);
    assert(!fallbackEntry->findBmsDownloadEligible);
  }
}

void testFindBmsDownloadEntryRejectsIneligiblePaths() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);

  const std::filesystem::path virtualTree =
      std::filesystem::path("@androidtree@") / "tree-id" / "Charts";
  const auto normal = temporary.path() / "normal";
  assert(session->InsertEntry(virtualTree, "content://tree/example"));
  assert(!session->SelectFindBmsDownloadEntry());
  assert(!session->SetFindBmsDownloadEntry(virtualTree));
  assert(!session->SetFindBmsDownloadEntry(temporary.path() / "missing"));

  assert(session->InsertEntry(normal));
  const auto entries = session->SelectAllEntries();
  const auto *virtualEntry = entryAtPath(entries, virtualTree);
  assert(virtualEntry != nullptr);
  assert(!virtualEntry->findBmsDownloadEligible);
  assert(!virtualEntry->findBmsDownloadFolder);
  assert(session->SelectFindBmsDownloadEntry());
}
```

Call both tests from `main()`.

- [ ] **Step 2: Run the focused target and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target chart_repository_tests -j 6
```

Expected: compilation fails because the new `ChartEntry` fields and session methods do not exist.

- [ ] **Step 3: Add the schema and public repository interface**

Extend `ChartEntry` and `ChartRepository::Session` in `src/repositories/ChartRepository.h`:

```cpp
struct ChartEntry {
  path_t path;
  std::string iosBookmark;
  bool removable = true;
  bool findBmsDownloadFolder = false;
  bool findBmsDownloadEligible = false;
};

bool SetFindBmsDownloadEntry(const std::filesystem::path &path);
std::optional<ChartEntry> SelectFindBmsDownloadEntry();
```

Change `createEntriesTable()` in `src/repositories/ChartRepository.cpp` so new databases include the flag and old databases migrate idempotently:

```cpp
"CREATE TABLE IF NOT EXISTS entries ("
"path TEXT primary key,"
"ios_bookmark TEXT DEFAULT '',"
"find_bms_download_folder INTEGER NOT NULL DEFAULT 0"
")"
```

Then add:

```cpp
if (!execSqlAllowDuplicateColumn(
        db,
        "ALTER TABLE entries ADD COLUMN find_bms_download_folder "
        "INTEGER NOT NULL DEFAULT 0",
        "migrating Find BMS download folder selection")) {
  return false;
}
```

- [ ] **Step 4: Implement eligibility, stable upsert, and selection normalization**

In `src/repositories/ChartRepository.cpp`, make `IsDefaultBmsFolderPath()` compare the normalized path on every platform:

```cpp
bool ChartRepository::IsDefaultBmsFolderPath(
    const std::filesystem::path &path) {
  return !path.empty() &&
         path.lexically_normal() ==
             DefaultBmsFolderPath().lexically_normal();
}
```

Add a platform-independent sentinel check and eligibility helper:

```cpp
static bool isAndroidTreeVirtualPath(const std::filesystem::path &path) {
  const auto normalized = path.lexically_normal();
  const auto first = normalized.begin();
  return first != normalized.end() &&
         first->generic_string() == "@androidtree@";
}

static bool isFindBmsDownloadEligible(const std::filesystem::path &path) {
  return !path.empty() &&
         !ChartRepository::IsDefaultBmsFolderPath(path) &&
         !isAndroidTreeVirtualPath(path);
}
```

Read entries in stable insertion order with:

```sql
SELECT rowid, path, COALESCE(ios_bookmark, ''),
       COALESCE(find_bms_download_folder, 0)
FROM entries
ORDER BY rowid
```

After converting stored paths to absolute paths, set `findBmsDownloadEligible`, choose the first selected eligible row or the first eligible row, and normalize all persisted flags with one atomic statement:

```sql
UPDATE entries
SET find_bms_download_folder =
  CASE WHEN path = @selected_path THEN 1 ELSE 0 END
```

When no eligible row exists, run:

```sql
UPDATE entries SET find_bms_download_folder = 0
```

Set the returned `ChartEntry::findBmsDownloadFolder` values from the normalized winner.

Replace `REPLACE INTO entries` with an upsert that preserves rowid and the selection flag:

```sql
INSERT INTO entries (path, ios_bookmark)
VALUES (@path, @ios_bookmark)
ON CONFLICT(path) DO UPDATE SET
  ios_bookmark = excluded.ios_bookmark
```

Make insertion normalize selection before it commits. Make `SetFindBmsDownloadEntry()` validate the target against normalized entries, reject fallback/virtual/unknown paths, and update every flag with the single `CASE` statement. Make `SelectFindBmsDownloadEntry()` return the normalized selected eligible row.

- [ ] **Step 5: Make deletion and promotion atomic**

Split raw deletion from the public transaction boundary:

```cpp
static bool deleteEntryRow(sqlite3 *db,
                           const std::filesystem::path &path);
static bool normalizeFindBmsDownloadEntry(sqlite3 *db,
                                          std::vector<ChartEntry> *entries);
```

`Session::DeleteEntry()` starts `BEGIN`, deletes the row, normalizes the replacement, and commits. `DeleteEntryAndChartMetaInDirectory()` uses its existing transaction, calls the same raw delete and normalization helpers, and then commits. Any failure returns without committing, preserving the old row and selection.

- [ ] **Step 6: Add a migration and duplicate-state regression test**

Create a legacy entries table before `EnsureReady()` and verify deterministic normalization:

```cpp
void testFindBmsDownloadEntryMigratesLegacyAndNormalizesDuplicates() {
  TempDirectory temporary;
  const auto databasePath = temporary.path() / "chart.db";
  const auto first = temporary.path() / "legacy-first";
  const auto second = temporary.path() / "legacy-second";
  {
    Database database = openDatabase(databasePath);
    assert(database);
    assert(execute(database.get(),
                   "CREATE TABLE entries (path TEXT PRIMARY KEY, "
                   "ios_bookmark TEXT DEFAULT '')"));
    assert(execute(database.get(),
                   "INSERT INTO entries(path) VALUES ('" +
                       first.generic_string() + "')"));
    assert(execute(database.get(),
                   "INSERT INTO entries(path) VALUES ('" +
                       second.generic_string() + "')"));
  }

  ChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);
  auto selected = session->SelectFindBmsDownloadEntry();
  assert(selected && std::filesystem::path(selected->path) == first);

  {
    Database database = openDatabase(databasePath);
    assert(database);
    assert(execute(database.get(),
                   "UPDATE entries SET find_bms_download_folder = 1"));
  }
  selected = session->SelectFindBmsDownloadEntry();
  assert(selected && std::filesystem::path(selected->path) == first);

  Database verification = openDatabase(databasePath);
  assert(verification);
  assert(queryInt(verification.get(),
                  "SELECT COUNT(*) FROM entries "
                  "WHERE find_bms_download_folder = 1") == 1);
}
```

Call it from `main()`.

- [ ] **Step 7: Run focused tests and commit**

Run:

```bash
cmake --build cmake-build-debug --target chart_repository_tests -j 6
ctest --test-dir cmake-build-debug -R '^chart_repository_tests$' --output-on-failure
```

Expected: `chart_repository_tests` passes.

Commit:

```bash
git add src/repositories/ChartRepository.h src/repositories/ChartRepository.cpp tests/chart_repository_tests.cpp
git commit -m "feat: persist Find BMS download folder"
```

---

### Task 2: Expose download-folder selection in BMS Library settings

**Files:**
- Modify: `scripts/check_find_bms_archive_flow.py`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsSceneTables.cpp`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/SettingsSceneShared.h`

**Interfaces:**
- Consumes: Task 1's `ChartEntry` flags and `SetFindBmsDownloadEntry()`.
- Produces: `SettingsScene::setFindBmsDownloadEntry(const std::string &)`, canonical Settings row labels, and immediate selected-state refresh.

- [ ] **Step 1: Add failing Settings flow-audit assertions**

Extend `scripts/check_find_bms_archive_flow.py` to read `SettingsSceneTables.cpp` and require the UI contract:

```python
settings_tables_path = root / "src/scene/SettingsSceneTables.cpp"
settings_tables_source = settings_tables_path.read_text(encoding="utf-8")

require(
    'makeText("Use for Downloads"' in settings_source
    and '"Download folder"' in settings_source,
    "BMS Library rows must expose download-folder selection",
)
require(
    '"Not writable by Find BMS"' in settings_source,
    "ineligible Android tree rows must explain why selection is disabled",
)
require(
    "SetFindBmsDownloadEntry" in settings_tables_source,
    "Settings must persist the selected Find BMS download folder",
)
```

Add `settings_tables_path` to the file-existence loop.

- [ ] **Step 2: Run the audit and verify RED**

Run:

```bash
ctest --test-dir cmake-build-debug -R '^find_bms_archive_flow_audit$' --output-on-failure
```

Expected: failure reports the missing selection labels and repository wiring.

- [ ] **Step 3: Add the Settings selection action**

Declare in `src/scene/SettingsScene.h`:

```cpp
void setFindBmsDownloadEntry(const std::string &entryPathText);
```

Implement in `src/scene/SettingsSceneTables.cpp`:

```cpp
void SettingsScene::setFindBmsDownloadEntry(
    const std::string &entryPathText) {
  auto session = context.chartRepository.OpenSession();
  if (!session) {
    chartFolderStatusMessage = "Could not open chart database.";
    chartFolderStatusColor = {255, 177, 170, 255};
  } else if (!session->SetFindBmsDownloadEntry(
                 std::filesystem::path(utf8_to_path_t(entryPathText)))) {
    chartFolderStatusMessage =
        "Could not use this folder for Find BMS downloads.";
    chartFolderStatusColor = {255, 177, 170, 255};
  } else {
    chartFolderStatusMessage = "Find BMS download folder updated.";
    chartFolderStatusColor = {181, 228, 165, 255};
    loadChartEntries();
    refreshChartEntryBackupStatuses();
  }
  if (chartFolderStatusText != nullptr) {
    chartFolderStatusText->setText(chartFolderStatusMessage);
    chartFolderStatusText->setColor(chartFolderStatusColor);
  }
  lastLayoutWidth = -1;
}
```

- [ ] **Step 4: Render row state and fallback copy**

In `buildBmsLibraryTab()`, add this action before Delete/iCloud actions:

```cpp
if (entry.findBmsDownloadFolder) {
  actions->addView(makeWrappedText("Download folder",
                                   metrics.smallTextSize,
                                   ui_theme::lime()));
} else if (entry.findBmsDownloadEligible) {
  auto *downloadButton = makeAccentButton(
      metrics.compact ? 180 : 210, metrics.actionButtonHeight,
      makeText("Use for Downloads", metrics.smallTextSize,
               ui_theme::textPrimary(), TextView::CENTER,
               TextView::MIDDLE),
      ui_theme::cyan());
  downloadButton->setOnClickListener([this, entryPathText]() {
    setFindBmsDownloadEntry(entryPathText);
  });
  actions->addView(downloadButton);
} else if (ChartRepository::IsDefaultBmsFolderPath(
               std::filesystem::path(entry.path))) {
  actions->addView(makeWrappedText("Fallback download folder",
                                   metrics.smallTextSize,
                                   ui_theme::textMuted()));
} else {
  actions->addView(makeWrappedText("Not writable by Find BMS",
                                   metrics.smallTextSize,
                                   ui_theme::textMuted()));
}
```

When `chartEntries` has no default row, keep the existing empty-list copy and add:

```cpp
folderList->addView(makeWrappedText(
    "Find BMS downloads use Documents/BMS until a writable library "
    "folder is added.",
    metrics.smallTextSize, ui_theme::textMuted()));
```

Update `formatChartEntrySource()` in `SettingsSceneShared.h` so the default path says it is the built-in Find BMS fallback, while normal entries retain their saved-access description.

- [ ] **Step 5: Run the audit, build the app, and commit**

Run:

```bash
ctest --test-dir cmake-build-debug -R '^find_bms_archive_flow_audit$' --output-on-failure
cmake --build cmake-build-debug --target main -j 6
```

Expected: the audit passes and `main` links.

Commit:

```bash
git add scripts/check_find_bms_archive_flow.py src/scene/SettingsScene.h src/scene/SettingsSceneTables.cpp src/scene/SettingsSceneLayout.cpp src/scene/SettingsSceneShared.h
git commit -m "feat: select Find BMS download folder"
```

---

### Task 3: Route every Find BMS download to the repository selection

**Files:**
- Modify: `scripts/check_find_bms_archive_flow.py`
- Modify: `src/scene/MainMenuScene.cpp`

**Interfaces:**
- Consumes: Task 1's `SelectFindBmsDownloadEntry()` and existing iOS bookmark resolver.
- Produces: one selected/fallback destination used by automatic and candidate downloads.

- [ ] **Step 1: Add failing destination-wiring audit assertions**

Add to `scripts/check_find_bms_archive_flow.py`:

```python
require(
    "SelectFindBmsDownloadEntry()" in main_menu_source,
    "Find BMS must resolve the repository-selected download folder",
)
require(
    main_menu_source.count("preferredBmsDownloadRoot()") >= 3,
    "automatic and candidate downloads must share destination resolution",
)
```

- [ ] **Step 2: Run the audit and verify RED**

Run:

```bash
ctest --test-dir cmake-build-debug -R '^find_bms_archive_flow_audit$' --output-on-failure
```

Expected: failure reports the missing repository-selected destination call.

- [ ] **Step 3: Replace first-entry selection with explicit selection**

Replace `MainMenuScene::preferredBmsDownloadRoot()` with:

```cpp
std::filesystem::path MainMenuScene::preferredBmsDownloadRoot() {
  const auto fallback = ChartRepository::DefaultBmsFolderPath();
  if (!chartSession) {
    ensureDirectoryExistsLogged(fallback, "BMS download root");
    return fallback;
  }

  const auto selected = chartSession->SelectFindBmsDownloadEntry();
  if (!selected) {
    ensureDirectoryExistsLogged(fallback, "BMS download root");
    return fallback;
  }

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return ResolveIOSFolderEntryPath(*selected);
#else
  return std::filesystem::path(selected->path);
#endif
}
```

Do not insert the fallback into `entries` from this method. Leave both existing callers—`showFindBmsModal()` and `startFindBmsCandidateDownload()`—using `preferredBmsDownloadRoot()`.

- [ ] **Step 4: Run focused tests/build and commit**

Run:

```bash
cmake --build cmake-build-debug --target chart_repository_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(chart_repository_tests|find_bms_archive_flow_audit)$' --output-on-failure
```

Expected: both tests pass and `main` links.

Commit:

```bash
git add scripts/check_find_bms_archive_flow.py src/scene/MainMenuScene.cpp
git commit -m "feat: route Find BMS downloads to selected folder"
```

---

### Task 4: Restore current and total byte progress in the modal

**Files:**
- Create: `src/scene/FindBmsProgressPresentation.h`
- Create: `src/scene/FindBmsProgressPresentation.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `tests/find_bms_download_tests.cpp`
- Modify: `src/scene/MainMenuScene.cpp`

**Interfaces:**
- Consumes: existing `findBmsProgressMessage`, `findBmsProgressCurrent`, and `findBmsProgressTotal` values.
- Produces: `formatFindBmsBytes(std::uint64_t)` and `findBmsProgressDisplayText(const std::string &, std::uint64_t, std::uint64_t, bool)` as pure tested presentation functions.

- [ ] **Step 1: Write failing progress presentation tests**

Include `../src/scene/FindBmsProgressPresentation.h` in `tests/find_bms_download_tests.cpp` and add:

```cpp
void testFindBmsDownloadProgressDisplaysSizes() {
  assert(findBmsProgressDisplayText("Downloading archive", 19503513,
                                    46451917, true) ==
         "Downloading archive - 42% (18.6 MB / 44.3 MB)");
  assert(findBmsProgressDisplayText("Downloading archive", 12345678, 0,
                                    true) ==
         "Downloading archive (11.8 MB)");
  assert(findBmsProgressDisplayText("Extracting archive", 12345678,
                                    46451917, true) ==
         "Extracting archive");
}
```

Call it from `main()`. Add `src/scene/FindBmsProgressPresentation.cpp` to the `find_bms_download_tests` executable in the top-level `CMakeLists.txt`.

- [ ] **Step 2: Configure/build and verify RED**

Run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
```

Expected: compilation fails because the new presentation header/function does not exist.

- [ ] **Step 3: Extract the existing formatter into a pure presentation unit**

Create `src/scene/FindBmsProgressPresentation.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>

std::string formatFindBmsBytes(std::uint64_t bytes);
std::string findBmsProgressDisplayText(const std::string &message,
                                       std::uint64_t downloadedBytes,
                                       std::uint64_t totalBytes,
                                       bool includeBytes);
```

Create `src/scene/FindBmsProgressPresentation.cpp` by moving the existing `formatFindBmsBytes()`, `progressPercentText()`, and string-argument `findBmsProgressDisplayText()` implementations out of `MainMenuScene.cpp`. Keep `progressPercentText()` private in an anonymous namespace.

Add `FindBmsProgressPresentation.cpp` to `src/scene/CMakeLists.txt`. Include the new header from `MainMenuScene.cpp`, remove the moved static implementations there, and keep the overload taking `BmsSearchDownloadProgress` as a local adapter:

```cpp
std::string
findBmsProgressDisplayText(const BmsSearchDownloadProgress &progress,
                           bool includeBytes) {
  return findBmsProgressDisplayText(progress.message,
                                    progress.downloadedBytes,
                                    progress.totalBytes, includeBytes);
}
```

- [ ] **Step 4: Render sizes in the active modal status**

In `refreshFindBmsModal()`, change only the normal running-download formatter call:

```cpp
statusText = findBmsProgressDisplayText(findBmsProgressMessage,
                                        findBmsProgressCurrent,
                                        findBmsProgressTotal, true);
```

Keep pending Keep/Delete decisions and all non-download stages unchanged. Increase the status view height only if the existing width clips the tested string; prefer wrapping to a second line over truncating the byte counts.

- [ ] **Step 5: Run focused tests/build and commit**

Run:

```bash
cmake --build cmake-build-debug --target find_bms_download_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(find_bms_download_tests|find_bms_archive_flow_audit)$' --output-on-failure
```

Expected: tests pass and `main` links.

Commit:

```bash
git add CMakeLists.txt src/scene/CMakeLists.txt src/scene/FindBmsProgressPresentation.h src/scene/FindBmsProgressPresentation.cpp src/scene/MainMenuScene.cpp tests/find_bms_download_tests.cpp
git commit -m "fix: restore Find BMS byte progress"
```

---

### Task 5: Cross-platform verification and branch handoff

**Files:**
- Verify only; modify implementation files only if a test exposes a defect.

**Interfaces:**
- Consumes: all prior tasks.
- Produces: a clean, verified feature branch with no uploaded or installed build.

- [ ] **Step 1: Run focused regression tests**

Run:

```bash
cmake --build cmake-build-debug --target chart_repository_tests find_bms_download_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(chart_repository_tests|find_bms_download_tests|find_bms_archive_flow_audit)$' --output-on-failure
```

Expected: all three tests pass and `main` is up to date.

- [ ] **Step 2: Run the full desktop suite**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: 100% of tests pass.

- [ ] **Step 3: Compile the real iOS target without deployment**

Run:

```bash
scripts/ios_firebase_deploy.sh --build-only
```

Expected: `** BUILD SUCCEEDED **`. Do not run the upload path and do not install the app.

- [ ] **Step 4: Review branch state**

Run:

```bash
git diff --check
git status --short --branch
git log --oneline -8
```

Expected: no whitespace errors, no uncommitted implementation files, and the feature commits appear on `feature/skip-unzip`.
