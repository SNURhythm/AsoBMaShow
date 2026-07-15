# Database Repository Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the chart, score, replay, and music-playlist database helpers with context-owned concrete repositories grouped under `src/repositories/`, preserving observable behavior, database compatibility, and performance except for the explicitly approved fail-closed chart-open fix.

**Architecture:** First lock down behavior and hot-query budgets, then relocate and rename the existing implementations without semantic edits. Convert one ownership boundary at a time, introduce a move-only chart session before replacing the attached-score query seam, and finally extract chart scanning and difficulty-table I/O behind domain batches so no scene or application service owns SQLite resources.

**Tech Stack:** C++23, bundled SQLite C API, SDL2 logging, CMake/CTest, nlohmann JSON, libcurl, libarchive, iOS file-system-synchronized Xcode groups.

## Global Constraints

- SQLite is the only persistence backend; do not add repository interfaces, a generic `Repository<T>`, or `SQLite*` implementation prefixes.
- Concrete classes are named `ChartRepository`, `ScoreRepository`, `ReplayRepository`, and `MusicPlaylistRepository`.
- Repository implementation and private SQLite support files live under `src/repositories/`.
- `ChartLibraryScanner` and `DifficultyTableImporter` live outside `src/repositories/` and never receive a `sqlite3*`.
- Existing database filenames, schemas, `PRAGMA user_version` values, migrations, indexes, pragmas, and stored bytes remain compatible.
- Existing query ordering, filtering, pagination, recovery, cancellation,
  progress, logging, and user-visible errors remain unchanged except for the
  approved chart fail-closed case below.
- Score, replay, and music validated-open paths remain fail-closed and
  unmodified. Harden chart opening with the same preflight: corrupt and
  future-version chart database families must be rejected before read-write
  open, pragmas, migration, checkpoint, or sidecar mutation. Supported chart
  databases must still use WAL, `synchronous=NORMAL`, and the current
  checkpoint-on-last-close policy.
- Apart from that approved chart fix, change behavior only for a major,
  straightforward correctness or data-safety defect backed by a deterministic
  regression test. Isolate and report every such fix; do not fold opportunistic
  cleanup into the refactor.
- Existing transaction/savepoint boundaries, profile activity guards, and atomic connection replacement remain intact.
- Main-menu paging keeps a retained chart connection and bounded page cache; chart background work keeps independent connections.
- Attached score/chart SQL joins, prepared-statement reuse, revision caches, replay corruption budgets, and scan batching must not regress.
- Preserve the project's existing PascalCase public-method convention during this refactor; architectural changes must not be mixed with cosmetic method renaming.
- Add every new `.cpp` path under `src` to `membershipExceptions` in `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.
- Do not deploy an iOS or Android build during this plan.

## File Structure

The final source layout is:

```text
src/repositories/
  ChartMetaSql.h
  ChartRepository.h
  ChartRepository.cpp
  ChartRepositoryDifficulty.cpp
  ChartRepositoryInternal.h
  ChartRepositoryQueries.cpp
  ChartScanStore.cpp
  ChartScanStore.h
  ChartSqlExpressions.h
  ChartStorageIdentity.cpp
  ChartStorageIdentity.h
  MusicPlaylistRepository.h
  MusicPlaylistRepository.cpp
  ReplayRepository.h
  ReplayRepository.cpp
  ReplayRepositoryInternal.h
  ReplayRepositoryRecords.cpp
  ReplayRepositorySchema.cpp
  ScoreCacheQueries.h
  ScoreRepository.h
  ScoreRepository.cpp
  ScoreRepositoryInternal.h
  ScoreRepositoryModels.h
  ScoreRepositoryQueries.cpp
  ScoreRepositorySchema.cpp
  SqliteRAII.h

src/ChartLibraryScanner.h
src/ChartLibraryScanner.cpp
src/DifficultyTableImporter.h
src/DifficultyTableImporter.cpp
src/DifficultyTableModel.h
src/LibraryFolderClearData.h

tests/RepositorySqliteTestSupport.h
tests/chart_repository_tests.cpp
tests/chart_library_scanner_tests.cpp
tests/difficulty_table_importer_tests.cpp
tests/music_playlist_repository_tests.cpp
tests/replay_repository_tests.cpp
tests/repository_boundary_tests.cpp
```

`ChartStorageIdentity.*` owns the storage-path normalization functions currently exposed by `ChartDBHelper` so music persistence and score migration do not depend on the complete chart repository. `LibraryFolderClearData.h` owns the folder aggregate value types currently declared in `scene/MainMenuLibrary.h`; it contains no SQL. Repository `Internal.h` files are private implementation headers and must not be included from scenes, services, or tests.

---

### Task 1: Characterize behavior and deterministic performance before moving code

**Files:**
- Create: `tests/RepositorySqliteTestSupport.h`
- Modify: `tests/main_menu_library_tests.cpp`
- Modify: `tests/profile_switch_tests.cpp`
- Modify: `tests/score_cache_query_tests.cpp`

**Interfaces:**
- Consumes: SQLite's `SQLITE_TRACE_STMT` callback and existing database fixtures.
- Produces: database-family snapshots, `repository_test::ScopedStatementTrace`,
  `repository_test::explainPlan`, and fixed statement/query-plan budgets used
  unchanged after each cutover.

- [ ] **Step 1: Add reusable statement and query-plan observation support**

Add this complete header:

```cpp
#pragma once

#include "../src/sqlite3.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace repository_test {

struct RawDatabaseFamilySnapshot {
  std::array<std::optional<std::string>, 4> files;

  bool operator==(const RawDatabaseFamilySnapshot &) const = default;
};

inline std::string readFileBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  assert(input);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

inline RawDatabaseFamilySnapshot
rawDatabaseFamilySnapshot(const std::filesystem::path &databasePath) {
  constexpr std::array<const char *, 4> suffixes{
      "", "-journal", "-wal", "-shm"};
  RawDatabaseFamilySnapshot snapshot;
  for (std::size_t i = 0; i < suffixes.size(); ++i) {
    const std::filesystem::path path =
        databasePath.string() + suffixes[i];
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    assert(!error);
    if (exists) {
      snapshot.files[i] = readFileBytes(path);
    }
  }
  return snapshot;
}

struct StatementTrace {
  int count = 0;
  std::vector<std::string> sql;

  static int callback(unsigned mask, void *context, void *statement,
                      void *) {
    if ((mask & SQLITE_TRACE_STMT) == 0 || context == nullptr ||
        statement == nullptr) {
      return 0;
    }
    auto &trace = *static_cast<StatementTrace *>(context);
    ++trace.count;
    const char *text =
        sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
    trace.sql.emplace_back(text != nullptr ? text : "");
    return 0;
  }
};

class ScopedStatementTrace {
public:
  ScopedStatementTrace(sqlite3 *database, StatementTrace &trace)
      : database_(database) {
    assert(database_ != nullptr);
    assert(sqlite3_trace_v2(database_, SQLITE_TRACE_STMT,
                            StatementTrace::callback, &trace) == SQLITE_OK);
  }

  ~ScopedStatementTrace() {
    sqlite3_trace_v2(database_, 0, nullptr, nullptr);
  }

  ScopedStatementTrace(const ScopedStatementTrace &) = delete;
  ScopedStatementTrace &operator=(const ScopedStatementTrace &) = delete;

private:
  sqlite3 *database_;
};

inline std::vector<std::string> explainPlan(sqlite3 *database,
                                            std::string_view query) {
  std::vector<std::string> details;
  const std::string sql = "EXPLAIN QUERY PLAN " + std::string(query);
  sqlite3_stmt *raw = nullptr;
  assert(sqlite3_prepare_v2(database, sql.c_str(), -1, &raw, nullptr) ==
         SQLITE_OK);
  while (sqlite3_step(raw) == SQLITE_ROW) {
    const auto *text = sqlite3_column_text(raw, 3);
    details.emplace_back(text != nullptr
                             ? reinterpret_cast<const char *>(text)
                             : "");
  }
  sqlite3_finalize(raw);
  return details;
}

inline bool planContains(const std::vector<std::string> &plan,
                         std::string_view text) {
  for (const std::string &line : plan) {
    if (line.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace repository_test
```

- [ ] **Step 2: Lock down chart count, page, index, and score-backed semantics**

Add `testChartQueryBehaviorMatrix()` beside
`testRealChartDbScoreQueryRetainsPreparedAttachmentGuard()` in
`tests/profile_switch_tests.cpp`. Use an in-memory chart connection, the
active score helper temporarily bound to a `SwitchFixture` score path, and
three chart rows:

```text
path         title  artist    bpm  level  sha256
alpha.bms    Alpha  Artist A  120  2      64 x 'a'
beta.bms     Beta   Artist B  180  7      64 x 'b'
gamma.bms    Gamma  Artist C  240  12     64 x 'c'
```

Insert persistent score-summary rows for long-note mode `1` with clear ranks
`1, 3, 2` and best scores `100, 300, 200`, then mark Gamma as favorite. Assert
the complete ordered paths from `QueryChartMeta`, the unpaginated total from
`CountChartMeta`, and every returned path's position from
`FindChartMetaIndex` for this exact matrix:

```text
default/title order                 alpha, beta, gamma
limit 1, offset 1                   beta (count remains 3)
keyword "Artist B"                  beta
bpmMin 170, bpmMax 200              beta
favoritesOnly                       gamma
clearMarkFilter rank 3, LN mode 1   beta
Score sort descending, LN mode 1    beta, gamma, alpha
```

Restore the singleton score path with an RAII guard even when an assertion
fails. Keep the existing concurrent profile-switch guard test immediately
after this matrix; together they freeze both result semantics and the
attachment lifetime before the API changes.

- [ ] **Step 3: Lock down chart schema migration behavior**

Add `testChartMigrationCompatibilityMatrix()` in
`tests/profile_switch_tests.cpp`. For each case, create a fresh in-memory
database by calling the current chart schema creator once, insert one complete
chart row and one favorite row, then set `PRAGMA user_version` to the case's
input version before calling the schema creator again.

Assert this exact matrix:

```text
input version  final version  chart rows  favorite rows  rebuild required
0              3              0           1              1
1              3              0           1              1
2              3              0           1              1
3              3              1           1              absent-or-0
```

For inputs 0 and 1, store uppercase favorite hashes and assert version-2
normalization lowercases them. For input 2, insert already-normalized hashes,
as required by that schema version. For input 3, assert the authored `total`
and `has_total` values survive. This freezes the intentional chart-cache drop
and rebuild marker used by migrations 1 and 3 while proving non-cache favorite
data survives.

- [ ] **Step 4: Lock down the folder-clear aggregation query budget**

In `tests/main_menu_library_tests.cpp`, include the support header, start `ScopedStatementTrace` immediately before `LoadFolderClearDataByLongNoteMode`, and assert:

```cpp
repository_test::StatementTrace trace;
main_menu_library::FolderClearDataByLongNoteMode data;
{
  repository_test::ScopedStatementTrace observation(db, trace);
  data =
      main_menu_library::LoadFolderClearDataByLongNoteMode(db, scoreRanks);
}
ASSERT_EQ(4, trace.count,
          "folder clear aggregation keeps four streaming SELECTs");
```

Do not count fixture creation statements.

- [ ] **Step 5: Lock down attached-score preparation and index use**

In `tests/score_cache_query_tests.cpp`, wrap the existing successful `prepareScoreQueryDatabase` case with `ScopedStatementTrace`. Assert one `ATTACH` occurs, a repeated preparation for the same path performs no second `ATTACH`, and the existing summary indexes remain selected by an `EXPLAIN QUERY PLAN` assertion:

```cpp
repository_test::StatementTrace trace;
{
  repository_test::ScopedStatementTrace observation(chartDb, trace);
  ASSERT_FALSE(score_cache_queries::prepareScoreQueryDatabase(
                   chartDb, scoreDbPath).has_value(),
               "first score attachment succeeds");
  ASSERT_FALSE(score_cache_queries::prepareScoreQueryDatabase(
                   chartDb, scoreDbPath).has_value(),
               "equivalent score attachment is reused");
}
int attachmentCount = 0;
for (const std::string &sql : trace.sql) {
  attachmentCount +=
      sql.find("ATTACH DATABASE") != std::string::npos ? 1 : 0;
}
ASSERT_EQ(1, attachmentCount,
          "equivalent preparation does not reattach");

const auto clearPlan = repository_test::explainPlan(
    chartDb,
    "SELECT chart_sha256, ln_mode, rank "
    "FROM score_db.score_sha256_clear_rank_cache "
    "WHERE chart_sha256 = 'abcdef' AND ln_mode = 0");
ASSERT_TRUE(repository_test::planContains(clearPlan, "PRIMARY KEY"),
            "WITHOUT ROWID score lookup retains its composite primary-key "
            "search");
```

Count `ATTACH` by inspecting `trace.sql` rather than assuming every traced statement is an attachment.

- [ ] **Step 6: Preserve replay bounded-read budgets**

Do not add a second observation mechanism. The existing
`testLimitedSummaryScansHaveFiniteCorruptBudget` already inserts 600 corrupt
rows, requests one result, and asserts the emitted `inspected=513` and
`budget=513` diagnostics for both chart and course summaries. The existing
oversized-course fixtures also enforce
`kMaxCourseStagesPerCandidate + 1` rejection. Keep those exact tests in every
focused run before and after the replay rename; do not replace them with timing
assertions.

- [ ] **Step 7: Run the characterization tests**

Run:

```sh
cmake --build cmake-build-debug --target main_menu_library_tests score_cache_query_tests replay_db_helper_tests profile_switch_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'main_menu_library_tests|score_cache_query_tests|replay_db_helper_tests|foundation_profile_switch'
```

Expected: all four tests pass. Record fresh durations in the commit message
body. The pre-plan baseline on this machine was approximately 13.4 seconds for
the replay helper, 0.02 seconds each for the two small query tests, and 3.6
seconds for the profile-switch suite. Record the post-matrix profile-switch
duration as well. These numbers are supporting evidence, not CI thresholds.

- [ ] **Step 8: Commit the characterization layer**

```sh
git add tests/RepositorySqliteTestSupport.h tests/main_menu_library_tests.cpp tests/profile_switch_tests.cpp tests/score_cache_query_tests.cpp
git commit -m "test: characterize repository query boundaries"
```

---

### Task 2: Group shared SQLite and SQL helpers under repositories

**Files:**
- Move: `src/SqliteRAII.h` → `src/repositories/SqliteRAII.h`
- Move: `src/ChartMetaSql.h` → `src/repositories/ChartMetaSql.h`
- Move: `src/ChartSqlExpressions.h` → `src/repositories/ChartSqlExpressions.h`
- Move: `src/ScoreCacheQueries.h` → `src/repositories/ScoreCacheQueries.h`
- Create: `src/repositories/ChartStorageIdentity.h`
- Create: `src/repositories/ChartStorageIdentity.cpp`
- Modify: every current include of the four moved headers
- Modify: `tests/chart_meta_index_order_tests.cpp`
- Modify: `tests/score_cache_query_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: the existing inline SQLite and SQL helper APIs byte-for-byte.
- Produces: repository-local include paths and `chart_storage_identity::{StoredPathText, ToRelativePath, ToAbsolutePath}`.

- [ ] **Step 1: Point one focused test at the new include path**

Change `tests/chart_meta_index_order_tests.cpp` to:

```cpp
#include "../src/repositories/ChartSqlExpressions.h"
```

Run:

```sh
cmake --build cmake-build-debug --target chart_meta_index_order_tests -j 6
```

Expected: compilation fails because `src/repositories/ChartSqlExpressions.h` does not exist.

- [ ] **Step 2: Move the four shared headers without changing their contents**

Use repository-aware renames so Git records history:

```sh
mkdir -p src/repositories
git mv src/SqliteRAII.h src/repositories/SqliteRAII.h
git mv src/ChartMetaSql.h src/repositories/ChartMetaSql.h
git mv src/ChartSqlExpressions.h src/repositories/ChartSqlExpressions.h
git mv src/ScoreCacheQueries.h src/repositories/ScoreCacheQueries.h
```

Update include paths only. Includes from files under `src/repositories/` use
local names; other `src` files use the `repositories/` prefix; tests prepend
`../src/` to that same prefixed path.

- [ ] **Step 3: Extract chart storage identity as a shared persistence primitive**

Create `src/repositories/ChartStorageIdentity.h` with:

```cpp
#pragma once

#include "../path.h"

#include <filesystem>
#include <string>

namespace chart_storage_identity {

std::string StoredPathText(std::filesystem::path path);
void ToRelativePath(std::filesystem::path &path);
void ToAbsolutePath(std::filesystem::path &path);

} // namespace chart_storage_identity
```

Move the exact implementations plus their three iOS-only helpers
(`relativeToCurrentDocumentsPath`, `storedDocumentsPath`, and
`relativeFromStoredDocumentsPath`) into `ChartStorageIdentity.cpp`. Make the
existing static chart-helper methods delegate to these functions for the
rename-only transition. Do not change their current `Documents/` marker,
`BMS/` fallback, UTF-8 conversion, lexical normalization, or absolute-path
behavior. Add the new `.cpp` to desktop/audio targets that use it and to the
iOS `membershipExceptions` list; music and score code must link this focused
translation unit rather than the complete chart repository.

- [ ] **Step 4: Rebuild all direct consumers**

Run:

```sh
cmake --build cmake-build-debug --target chart_meta_index_order_tests score_cache_query_tests main_menu_library_tests replay_db_helper_tests result_persistence_integration_tests profile_switch_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'chart_meta_index_order_tests|score_cache_query_tests|main_menu_library_tests|replay_db_helper_tests|result_persistence_integration_tests|foundation_profile_switch'
```

Expected: build succeeds and all selected tests pass with the Task 1 budgets unchanged.

- [ ] **Step 5: Commit the repository-local infrastructure**

```sh
git add src tests CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "refactor: group database infrastructure"
```

---

### Task 3: Relocate and rename the four concrete persistence classes

**Files:**
- Move: `src/ChartDBHelper.h/.cpp` → `src/repositories/ChartRepository.h/.cpp`
- Move: `src/ScoreDBHelper.h/.cpp` → `src/repositories/ScoreRepository.h/.cpp`
- Move: `src/ReplayDBHelper.h/.cpp` → `src/repositories/ReplayRepository.h/.cpp`
- Move: `src/audio/MusicPlaylistDB.h/.cpp` → `src/repositories/MusicPlaylistRepository.h/.cpp`
- Move: `tests/replay_db_helper_tests.cpp` → `tests/replay_repository_tests.cpp`
- Modify: all includes and class references listed by `rg -l '(ChartDBHelper|ScoreDBHelper|ReplayDBHelper|MusicPlaylistDB)' src tests`
- Modify: `src/CMakeLists.txt`
- Modify: `src/audio/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: the exact existing methods and behavior.
- Produces: `ChartRepository`, `ScoreRepository`, `ReplayRepository`, and `MusicPlaylistRepository` at their final include paths. This task deliberately changes names and locations only; Tasks 4–7 remove global/raw ownership.

- [ ] **Step 1: Convert the replay test to the final class name first**

In the still-existing `tests/replay_db_helper_tests.cpp`, change its include and
construction sites without renaming the file yet:

```cpp
#include "../src/repositories/ReplayRepository.h"

ReplayRepository repository(databasePath);
```

Run:

```sh
cmake --build cmake-build-debug --target replay_db_helper_tests -j 6
```

Expected: compilation fails because `ReplayRepository.h` and `ReplayRepository` do not exist.

- [ ] **Step 2: Move source files and perform exact symbol replacements**

Use these mappings and no additional API edits:

```text
ChartDBHelper       -> ChartRepository
ScoreDBHelper       -> ScoreRepository
ReplayDBHelper      -> ReplayRepository
MusicPlaylistDB     -> MusicPlaylistRepository
```

Keep `GetInstance()` temporarily for chart, score, and replay so this mechanical commit stays behavior-neutral. Keep all raw connection parameters temporarily. Update constructor/destructor names, comments, include guards supplied by `#pragma once`, CMake sources, and iOS membership paths.

At the end of this step, rename
`tests/replay_db_helper_tests.cpp` to
`tests/replay_repository_tests.cpp`; Step 3 updates its target in the same
working slice before the next build.

- [ ] **Step 3: Rename the replay CMake target**

Change the executable and CTest name from `replay_db_helper_tests` to `replay_repository_tests` and use:

```cmake
add_executable(replay_repository_tests
    tests/replay_repository_tests.cpp
    src/CourseIdentity.cpp
    src/FileChecksum.cpp
    src/repositories/ReplayRepository.cpp
    src/ResultPersistenceModel.cpp
    src/ScoreProvenance.cpp
    src/Uuid.cpp
    src/Utils.cpp
    src/path.cpp
    src/sqlite3.c
)
```

Update all test filters and documentation references in this plan's commands from this point forward.

- [ ] **Step 4: Prove the relocation is mechanical**

Run:

```sh
cmake --build cmake-build-debug --target replay_repository_tests score_provenance_db_tests result_persistence_integration_tests main_menu_library_tests app_database_initializer_tests profile_switch_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'replay_repository_tests|foundation_provenance_db|result_persistence_integration_tests|main_menu_library_tests|app_database_initializer_tests|foundation_profile_switch'
rg -n 'ChartDBHelper|ScoreDBHelper|ReplayDBHelper|MusicPlaylistDB' src tests CMakeLists.txt
```

Expected: all selected tests pass and the final `rg` command returns no matches.

- [ ] **Step 5: Commit the concrete repository names**

```sh
git add src tests CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "refactor: name concrete repositories"
```

---

### Task 4: Make score and replay repositories context-owned

**Files:**
- Modify: `src/repositories/ScoreRepository.h/.cpp`
- Modify: `src/repositories/ReplayRepository.h/.cpp`
- Modify: `src/repositories/ChartRepository.h/.cpp`
- Modify: `src/ResultPersistenceCoordinator.h/.cpp`
- Modify: `src/ProfileSessionCoordinator.h/.cpp`
- Modify: `src/AppDatabaseInitializer.h`
- Modify: `src/context.h`
- Modify: `src/ResultPresentationUtils.h`
- Modify: `src/ApplicationResultRecovery.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/ChartViewerScene.cpp`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `src/ResultImageExporter.cpp`
- Modify: `tests/app_database_initializer_tests.cpp`
- Modify: `tests/result_persistence_coordinator_tests.cpp`
- Modify: `tests/result_persistence_integration_tests.cpp`
- Modify: `tests/profile_switch_tests.cpp`

**Interfaces:**
- Consumes: concrete repository constructors and existing result/profile domain outcomes.
- Produces: `ApplicationContext::scoreRepository`, `ApplicationContext::replayRepository`, repository-reference coordinator constructors, and no score/replay `GetInstance()`.

- [ ] **Step 1: Add a failing ownership test**

In `tests/app_database_initializer_tests.cpp`, replace singleton-oriented assertions with:

```cpp
static_assert(!std::is_copy_constructible_v<ScoreRepository>);
static_assert(!std::is_copy_constructible_v<ReplayRepository>);
static_assert(std::is_constructible_v<
              result_persistence::Coordinator,
              ScoreRepository &, ReplayRepository &>);
static_assert(requires(ScoreRepository &scores,
                       ReplayRepository &replays) {
  app_database_initializer::initializeApplicationDatabases(scores, replays);
});
```

Keep the existing `initializeApplicationDatabasesWith` cases that prove every
readiness callback runs exactly once and that failures do not short-circuit.
Build `app_database_initializer_tests`.

Expected: compilation fails because the initializer still owns singleton selection and the context-owned overload is absent.

- [ ] **Step 2: Add repository members in dependency order**

In `ApplicationContext`, declare:

```cpp
ScoreRepository scoreRepository;
ReplayRepository replayRepository;
result_persistence::Coordinator resultPersistence;
```

Initialize the coordinator with those exact members:

```cpp
resultPersistence(scoreRepository, replayRepository)
```

After successful profile initialization, call `SetDatabasePath` on the members. Construct `ProfileSessionCoordinator` with the same members. In the destructor, call `Shutdown()` on those members after background threads stop.

- [ ] **Step 3: Parameterize initialization and coordinators**

Use these concrete signatures:

```cpp
bool initializeScoreDatabase(ScoreRepository &repository);
bool initializeReplayDatabase(ReplayRepository &repository);

DatabaseInitializationStatus initializeApplicationDatabases(
    ScoreRepository &scores,
    ReplayRepository &replays);

Coordinator(ScoreRepository &score, ReplayRepository &replay);

ProfileSessionCoordinator(PlayerProfileManager &manager,
                          ScoreRepository &score,
                          ReplayRepository &replay,
                          Blocker blocker,
                          InputLoader inputLoader,
                          InputRestorer inputRestorer,
                          CacheRefresher cacheRefresher,
                          ProfileSessionDependencies dependencies);
```

The intermediate two-repository aggregate keeps chart initialization on the
renamed chart singleton and music initialization on its current local helper,
but uses the supplied score and replay objects. `ApplicationContext` calls this
overload. Remove the old zero-argument score initializer, replay initializer,
and aggregate so they cannot retain deleted singleton lookups. Keep the
existing standalone path overloads by constructing local repositories; they
are used by profile validation. Task 5 injects music into the aggregate and
Task 6 injects chart.

- [ ] **Step 4: Bridge chart score-backed queries to the context-owned score repository**

The renamed chart implementation still calls `ScoreRepository::GetInstance()`
inside `QueryChartMeta`, `CountChartMeta`, and `FindChartMetaIndex`. Before
deleting that singleton, make the dependency explicit on the temporary
raw-chart API:

```cpp
void QueryChartMeta(sqlite3 *database, ScoreRepository &scores,
                    const ChartMetaQuery &query,
                    std::vector<ChartMetaRecord> &records);
int CountChartMeta(sqlite3 *database, ScoreRepository &scores,
                   const ChartMetaQuery &query);
int FindChartMetaIndex(sqlite3 *database, ScoreRepository &scores,
                       const ChartMetaQuery &query,
                       const std::filesystem::path &path);
```

Pass `scores` into the existing `ensureScoreQueryDatabase` helper and otherwise
leave the query bodies unchanged. `MainMenuScene` and `ChartListPageCache` pass
`context.scoreRepository`; the real score-backed profile-switch test passes its
fixture repository. This temporary signature exists only until Task 6 wraps
the same dependency in `ChartRepository::Session`.

- [ ] **Step 5: Remove global lookups from runtime consumers**

Replace each runtime lookup with the repository carried by `ApplicationContext` or an explicit parameter. Change the persistence-dependent utility signatures to:

```cpp
std::optional<ReplayData>
bestReplayForSnapshot(ReplayRepository &replays,
                      bms_parser::Chart &chart,
                      const ScoreBestSnapshot &best,
                      const std::optional<std::string> &beforeCreatedAt =
                          std::nullopt);

std::optional<ResultPreviousBestData>
previousBestForReplayChart(ScoreRepository &scores,
                           const bms_parser::ChartMeta &meta,
                           const ReplayData &replay);
```

Exporters already receive `ApplicationContext &`, so pass `context.scoreRepository` and `context.replayRepository` without adding globals.

- [ ] **Step 6: Delete score/replay singleton APIs**

Remove:

```cpp
static ScoreRepository &GetInstance();
static ReplayRepository &GetInstance();
```

Keep test-only standalone connections until Task 7; runtime code must not call them.

- [ ] **Step 7: Verify profile and durable-result behavior**

Run:

```sh
cmake --build cmake-build-debug --target app_database_initializer_tests result_persistence_coordinator_tests result_persistence_integration_tests replay_repository_tests score_provenance_db_tests profile_switch_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'app_database_initializer_tests|result_persistence_coordinator_tests|result_persistence_integration_tests|replay_repository_tests|foundation_provenance_db|foundation_profile_switch'
rg -n '(ScoreRepository|ReplayRepository)::GetInstance' src tests
```

Expected: tests pass and `rg` returns no matches.

- [ ] **Step 8: Commit context-owned score and replay repositories**

```sh
git add src tests CMakeLists.txt
git commit -m "refactor: inject score and replay repositories"
```

---

### Task 5: Make music persistence own its connection

**Files:**
- Create: `tests/music_playlist_repository_tests.cpp`
- Modify: `src/repositories/MusicPlaylistRepository.h/.cpp`
- Modify: `src/audio/MusicPlayerService.h/.cpp`
- Modify: `src/AppDatabaseInitializer.h`
- Modify: `src/context.h`
- Modify: `src/audio/CMakeLists.txt`
- Modify: `tests/app_database_initializer_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: existing playlist, track, queue, and player-state models.
- Produces: a path-injectable, non-copyable `MusicPlaylistRepository` with one retained connection and methods that take no `sqlite3*`; `MusicPlayerService(MusicPlaylistRepository &)`.

- [ ] **Step 1: Write a failing repository round-trip test**

Create a test that builds a temporary chart database using the exact
`chart_meta` DDL from the current `createChartMetaTableSchema`, inserts one row
with every column consumed by `kChartMetaSelectColumns`, then constructs:

```cpp
MusicPlaylistRepository repository(
    temporary.path() / "music_playlist.db",
    temporary.path() / "chart.db");

assert(repository.EnsureReady());
const int playlistId = repository.EnsurePlaylist("My Playlist");
assert(playlistId > 0);
assert(repository.InsertTrack(playlistId, chartMeta));
assert(repository.SelectPlaylists().front().trackCount == 1);
repository.Shutdown();
```

Also save and load a `MusicPlayerStateRecord` and a now-playing list. Add a CMake target named `music_playlist_repository_tests`.

Add compatibility cases before implementation:

- Create a version-0 playlist database with the pre-identity
  `music_playlist_items` and `music_now_playing_items` columns, migrate it,
  assert `PRAGMA user_version == 1`, all three identity columns and ten current
  indexes exist, and the original playlist/state rows are preserved.
- Create a `user_version == 2` future database containing a sentinel row,
  snapshot its main/journal/WAL/SHM files, assert `EnsureReady()` fails, and
  assert the database family is presence- and byte-identical.
- Repeat the unchanged-family assertion for a header-shaped corrupt database
  and for chart validation failure; neither case may create or mutate the
  playlist database.

Run the target. Expected: compilation fails because the constructor and connection-owning methods do not exist.

- [ ] **Step 2: Replace raw-handle public methods with a PIMPL**

The final header shape is:

```cpp
class MusicPlaylistRepository {
public:
  MusicPlaylistRepository();
  MusicPlaylistRepository(std::filesystem::path databasePath,
                          std::filesystem::path chartDatabasePath);
  ~MusicPlaylistRepository();

  MusicPlaylistRepository(const MusicPlaylistRepository &) = delete;
  MusicPlaylistRepository &
  operator=(const MusicPlaylistRepository &) = delete;

  bool EnsureReady();
  void Shutdown();
  int EnsurePlaylist(const std::string &name);
  bool RenamePlaylist(int playlistId, const std::string &name);
  std::vector<MusicPlaylistInfo> SelectPlaylists();
  bool InsertTrack(int playlistId,
                   const bms_parser::ChartMeta &chartMeta);
  bool DeleteTrack(int playlistId,
                   const bms_parser::ChartMeta &chartMeta,
                   int storedItemId = 0);
  bool MoveTrack(int playlistId,
                 const bms_parser::ChartMeta &chartMeta,
                 int delta, int storedItemId = 0);
  bool ClearPlaylist(int playlistId);
  bool DeletePlaylist(int playlistId);
  MusicPlayerStateRecord SelectPlayerState();
  bool SavePlayerState(const MusicPlayerStateRecord &state);
  bool ReplaceNowPlayingTracks(
      const std::vector<bms_parser::ChartMeta> &tracks);
  std::vector<MusicTrackRecord> SelectLibraryTracks();
  std::vector<MusicTrackRecord> SelectLibraryGroupTracks(
      const bms_parser::ChartMeta &chartMeta);
  std::vector<MusicTrackRecord> SelectNowPlayingTracks();
  std::vector<MusicTrackRecord> SelectTracks(int playlistId);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
```

`Impl` owns the existing validated connection, schema-ready flag, and paths.
`EnsureReady()` is idempotent. Every operation calls a private
`EnsureDatabase()` that reuses the retained handle; it must not reopen per
method. Preserve `MusicPlayerService::DatabaseLocked`'s current safety rule:
if the retained connection is found with autocommit disabled at the next call,
log the existing diagnostic, discard it, and open a fresh validated connection.

- [ ] **Step 3: Inject the repository into the music service**

Change the service fields from a raw connection plus owned DB helper to:

```cpp
explicit MusicPlayerService(MusicPlaylistRepository &repository);
MusicPlaylistRepository &repository;
```

Remove `playlistDatabase`, `DatabaseLocked`, `CloseDatabaseLocked`, and every
`sqlite3 *db` parameter from playlist-related private service helpers. Add:

```cpp
bool EnsureRepositoryReadyLocked(std::string &errorMessage);
```

Each current `DatabaseLocked` call site uses that idempotent readiness check and
then calls repository methods directly. Keep the existing service mutex and
cache refresh order. There is no service `Start()` method: the initializer and
first repository-using operation both call `EnsureReady()`. The service
destructor stops all workers first and then calls `repository.Shutdown()`.

- [ ] **Step 4: Make ApplicationContext own the music repository**

Declare `MusicPlaylistRepository musicPlaylistRepository;` before `musicPlayer` and initialize:

```cpp
musicPlayer(musicPlaylistRepository)
```

Change music startup initialization to `initializeMusicDatabase(MusicPlaylistRepository &)`.
Replace the Task 4 aggregate overload with the next buildable stage:

```cpp
DatabaseInitializationStatus initializeApplicationDatabases(
    ScoreRepository &scores,
    ReplayRepository &replays,
    MusicPlaylistRepository &music);
```

It keeps the renamed chart singleton temporarily and uses all three supplied
repositories. Update `ApplicationContext` and the initializer compile-time test
to call/require this exact overload; remove the two-argument aggregate and the
zero-argument music initializer, and do not construct another music repository
inside startup.

- [ ] **Step 5: Verify connection reuse and behavior**

In the isolated repository test, install a SQLite auto-extension after creating
the chart fixture and before `EnsureReady()`. Its callback increments an atomic
counter every time SQLite opens a connection:

```cpp
std::atomic<int> *connectionOpenCount = nullptr;

int countConnectionOpen(sqlite3 *, char **,
                        const sqlite3_api_routines *) {
  connectionOpenCount->fetch_add(1, std::memory_order_relaxed);
  return SQLITE_OK;
}
```

The scoped test helper sets the pointer, calls `sqlite3_reset_auto_extension()`,
registers `countConnectionOpen`, and reverses both actions in its destructor.
After the first successful `EnsureReady()`, record the counter (chart
validation may legitimately use an additional short-lived connection). Call
`EnsureReady()` again, execute every playlist/state/track operation in the
round-trip test, and assert the counter is unchanged. After `Shutdown()`, a new
`EnsureReady()` must increase it. This proves hot calls reuse the retained
connection without exposing a production test hook or virtual factory.

Run:

```sh
cmake --build cmake-build-debug --target music_playlist_repository_tests app_database_initializer_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'music_playlist_repository_tests|app_database_initializer_tests'
rg -n 'playlistDatabase|playlistDb|DatabaseLocked|CloseDatabaseLocked' src/audio/MusicPlayerService.h src/audio/MusicPlayerService.cpp
rg -n 'sqlite3|SqliteConnectionHandle' src/audio/MusicPlayerService.h
```

Expected: tests pass and both `rg` commands return no matches. Temporary local
chart-favorites SQLite use may remain in `MusicPlayerService.cpp` until the
chart-session cutover in Task 6.

- [ ] **Step 6: Commit music persistence ownership**

```sh
git add src tests CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "refactor: encapsulate music playlist persistence"
```

---

### Task 6: Introduce ChartRepository sessions and remove raw chart handles

**Files:**
- Create: `tests/chart_repository_tests.cpp`
- Create: `src/LibraryFolderClearData.h`
- Create: `src/repositories/ScoreRepositoryModels.h`
- Modify: `src/repositories/SqliteRAII.h`
- Modify: `src/repositories/ChartRepository.h/.cpp`
- Modify: `src/repositories/ScoreRepository.h/.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/MusicPlaylistRepository.cpp`
- Modify: `src/AppDatabaseInitializer.h`
- Modify: `src/context.h`
- Modify: `src/ResultPresentationUtils.h`
- Modify: `src/scene/MainMenuLibrary.h/.cpp`
- Modify: `src/scene/MainMenuScene.h/.cpp`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsSceneTables.cpp`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/audio/MusicPlayerService.h/.cpp`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `src/ResultImageExporter.cpp`
- Modify: `tests/app_database_initializer_tests.cpp`
- Modify: `tests/profile_switch_tests.cpp`
- Modify: `tests/score_provenance_db_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: existing chart query/value types, score cache types, and chart-path behavior.
- Produces: context-owned `ChartRepository`, move-only `ChartRepository::Session`, session-based score query preparation, and no chart `GetInstance()` or application-owned `sqlite3*`.

- [ ] **Step 1: Write the failing chart-session test**

Move `testChartQueryBehaviorMatrix` and
`testChartMigrationCompatibilityMatrix` from `profile_switch_tests.cpp` into
the new `chart_repository_tests.cpp` and adapt their fixtures to path-injected
`ChartRepository` and `ScoreRepository` objects; keep the concurrent
attachment/profile-switch guard test in `profile_switch_tests.cpp`. Then add a
temporary-database session case:

```cpp
ChartRepository repository(temporary.path() / "chart.db");
assert(repository.EnsureReady());

auto session = repository.OpenSession();
assert(session.has_value());
assert(session->InsertChartMeta(chartMeta));
assert(session->CountAllChartMeta() == 1);

ChartMetaQuery query;
query.limit = 64;
std::vector<ChartMetaRecord> page;
session->QueryChartMeta(query, page);
assert(page.size() == 1);
assert(session->FindChartMetaIndex(query, chartMeta.BmsPath) == 0);
```

Add a second session open at the same time and assert both remain usable. Build the target.

Add a fresh-schema assertion for `PRAGMA user_version == 3`. Keep the moved
chart query behavior matrix in this target so every session cutover rechecks
ordering, pagination, score attachment, and path indices.

Add the approved chart-open regression cases using the Task 1 raw database
family snapshot helper:

- Create a `user_version == 4` chart database with a sentinel table and row,
  snapshot `chart.db`, `chart.db-journal`, `chart.db-wal`, and `chart.db-shm`,
  assert `EnsureReady()` fails, and assert the family is byte-for-byte and
  presence-for-presence unchanged.
- Repeat that unchanged-family assertion for a header-shaped corrupt chart
  database.
- Open a supported version-3 fixture under a test-local SQLite auto-extension
  statement trace. Assert a later inspection sees `journal_mode == "wal"` and
  the traced production connection executes `PRAGMA synchronous=NORMAL`.
- After `EnsureReady()`, record a test-local auto-extension connection counter,
  call `EnsureReady()` again and assert the counter is unchanged, then open and
  close ten score-free sessions and assert the counter increases by exactly
  ten. This prevents a full snapshot/integrity preflight from being repeated
  per session.

In `score_provenance_db_tests.cpp`, add a focused helper-policy case that opens
a fresh fixture with:

```cpp
SqliteValidatedOpenPolicy policy{
    .enableForeignKeys = false,
    .disableCheckpointOnClose = false,
};
SqliteConnectionHandle connection(openValidatedSqliteDatabase(
    databasePath, 3, policy, errorMessage));
assert(connection);
int noCheckpointOnClose = -1;
assert(sqlite3_db_config(connection.get(), SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE,
                         -1, &noCheckpointOnClose) == SQLITE_OK);
assert(noCheckpointOnClose == 0);
```

This locks down the chart policy without changing the existing bool overload's
`NO_CKPT_ON_CLOSE` behavior for score, replay, and music. Open a second fresh
fixture through the existing bool overload and query the same setting, asserting
it remains `1`.

Expected: compilation fails because the path constructor, `EnsureReady`, and `Session` API are absent.

- [ ] **Step 2: Define a move-only session that hides SQLite**

Use this public shape:

```cpp
class ScoreRepository;

class ChartRepository {
public:
  class Session {
  public:
    ~Session();
    Session(Session &&) noexcept;
    Session &operator=(Session &&) noexcept;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    bool EnsureSchema();
    bool InsertChartMeta(bms_parser::ChartMeta &chartMeta);
    int CountAllChartMeta();
    int CountSolidArchives();
    void SelectAllChartMeta(
        std::vector<bms_parser::ChartMeta> &chartMetas);
    void SelectFavoriteMusicTracks(
        std::vector<MusicTrackRecord> &tracks);
    int CountFavoriteCharts();
    bool SetFavorite(const bms_parser::ChartMeta &chartMeta,
                     bool favorite);
    void QueryChartMeta(const ChartMetaQuery &query,
                        std::vector<ChartMetaRecord> &chartMetas);
    int CountChartMeta(const ChartMetaQuery &query);
    int FindChartMetaIndex(const ChartMetaQuery &query,
                           const std::filesystem::path &path);
    bool DeleteChartMeta(std::filesystem::path path);
    int DeleteChartMetaInDirectory(
        const std::filesystem::path &directory);
    bool DeleteArchiveRecords(
        const std::filesystem::path &archivePath);
    bool ClearChartMeta();
    bool InsertEntry(const std::filesystem::path &path,
                     const std::string &iosBookmark = "");
    std::vector<ChartEntry> SelectAllEntries();
    std::vector<ChartEntry> SelectEffectiveEntries();
    bool DeleteEntry(const std::filesystem::path &path);
    bool ClearEntries();
    int ScanChartRoots(
        const std::vector<std::filesystem::path> &roots,
        const std::stop_token *stopToken = nullptr,
        ChartScanProgressCallback progressCallback = nullptr,
        ChartScanPauseCallback pauseCallback = nullptr,
        ChartScanFlushRequestCallback flushRequestCallback = nullptr,
        ChartScanFlushCompleteCallback flushCompleteCallback = nullptr);
    bool ImportDifficultyTable(const std::string &headerJson,
                               const std::string &dataJson,
                               const std::string &sourceUrl = "");
    bool ImportDifficultyTableFromUrl(
        const std::string &pageUrl,
        std::string *errorMessage = nullptr,
        DifficultyTableImportProgressCallback progressCallback = nullptr);
    bool UpdateDifficultyTableFromSourceUrl(
        int tableId, std::string *errorMessage = nullptr);
    bool DeleteDifficultyTable(int tableId);
    int ImportDifficultyTablesFromDirectory(
        const std::filesystem::path &directory);
    std::vector<DifficultyTableInfo> SelectDifficultyTables();
    std::vector<DifficultyLevelInfo> SelectDifficultyLevels(int tableId);
    std::vector<DifficultyCourseTableInfo>
    SelectDifficultyCourseTables();
    std::vector<DifficultyCourseGroupInfo>
    SelectDifficultyCourseGroups(int tableId);
    std::vector<DifficultyCourseInfo>
    SelectDifficultyCourses(int tableId, const std::string &groupName);
    std::vector<course_identity::Definition>
    SelectDifficultyCourseDefinitions();
    std::string DifficultyTableLabelsForChart(
        const bms_parser::ChartMeta &meta);
    chart_library::FolderClearDataByLongNoteMode
    LoadFolderClearDataByLongNoteMode(
        const ScoreClearRankCache &scoreRanks);

  private:
    friend class ChartRepository;
    friend class ScoreRepository;
    struct Impl;
    explicit Session(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
  };

  ChartRepository();
  explicit ChartRepository(std::filesystem::path databasePath);
  ~ChartRepository();
  ChartRepository(const ChartRepository &) = delete;
  ChartRepository &operator=(const ChartRepository &) = delete;
  bool EnsureReady();
  std::optional<Session>
  OpenSession(ScoreRepository *scores = nullptr);
  const std::filesystem::path &DatabasePath() const;
  std::uint64_t GetLibraryRevision() const;

  static std::filesystem::path DefaultBmsFolderPath();
  static bool IsDefaultBmsFolderPath(const std::filesystem::path &path);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
```

Move the score/cache value declarations currently above `ScoreRepository`—from
`TransparentStringHash` through `ScoreBestCache`, including
`CourseScoreEvidence` and the long-note-mode helpers—into
`ScoreRepositoryModels.h`. `ChartRepository.h` includes that model header and
only forward-declares `ScoreRepository`; `ScoreRepository.h` can then include
`ChartRepository.h` for the nested session type without a cycle.
`ReplayRepository.h` includes the model header directly for
`CourseScoreEvidence`.

The private `Session::Impl` owns `SqliteConnectionHandle` and a non-owning
`ScoreRepository *`. Move every current `sqlite3 *db` public operation onto
`Session` without changing the SQL body. The scan and difficulty I/O methods
are explicit transitional methods so Task 6 compiles; Tasks 10 and 11 remove
them after extracting their workflows. Production sessions that perform chart
filter/sort queries open with `&context.scoreRepository`; scanner, importer,
and favorites-only sessions may omit it. A score-backed query creates the
existing `PreparedScoreQueryDatabase` lease from the stored repository pointer
for the duration of the SQL read.

The default repository constructor stores
`Utils::GetDocumentsPath("db") / "chart.db"`; the path constructor stores its
injected path. `DatabasePath()` therefore always returns the actual path used
by `EnsureReady()` and new sessions, never an empty sentinel.

`ChartRepository::Impl` owns a readiness mutex and flag. Its first
`EnsureReady()` first creates the database parent directory with the current
diagnostic behavior, then opens the chart database with
`openValidatedSqliteDatabase(path, 3, policy, error)` before running any chart
schema code, then applies only `PRAGMA synchronous=NORMAL`; the validated
opener establishes WAL after its unchanged-family preflight. Extend
`SqliteRAII.h` with a `SqliteValidatedOpenPolicy` overload while retaining the
existing bool overload for score, replay, music, and current tests. The chart
policy keeps foreign keys disabled and keeps SQLite's existing checkpoint-on-
last-close behavior; guard/preflight connections still disable close-time
checkpointing so rejected families remain untouched.

After readiness succeeds, `OpenSession()` calls a private
`openTrustedChartDatabase`: perform exactly one read-write/private-cache open,
set the current 1000 ms busy timeout, verify `user_version == 3`, and execute
the current `PRAGMA journal_mode=WAL` and `PRAGMA synchronous=NORMAL` sequence.
Keep the existing best-effort handling for those per-session pragmas: log a
failure but keep an otherwise valid session. It must not rerun the full family
snapshot or schema migration. `OpenSession()` calls `EnsureReady()` so callers
cannot bypass the preflight. Preserve the existing chart diagnostic destination
and do not delete, replace, or migrate a database that fails preflight. This is
the approved data-safety fix without adding a per-session database-copy cost.

- [ ] **Step 3: Preserve score-backed attached queries without exposing the handle**

Change `ScoreRepository::PreparedScoreQueryDatabase` to construct from a chart session:

```cpp
class [[nodiscard]] PreparedScoreQueryDatabase {
public:
  PreparedScoreQueryDatabase(const ScoreRepository &,
                             ChartRepository::Session &);
  ~PreparedScoreQueryDatabase();
  const std::optional<std::string> &error() const;
};

PreparedScoreQueryDatabase
PrepareScoreQueryDatabase(ChartRepository::Session &) const;

ScoreClearRankCache
LoadBestClearRanks(ChartRepository::Session &,
                   std::string_view schema);

ScoreBestCache
LoadBestScores(ChartRepository::Session &,
               std::string_view schema);
```

`ScoreRepository` is a friend of `Session` and accesses the native handle only in its `.cpp`. Preserve the current detach-before-recovery, attach path equality, session mutex, profile write guard, and attached schema name.

- [ ] **Step 4: Move folder-clear data out of the scene SQL boundary**

Move `ClearMarkCountMap`, `FolderClearMarkCounts`, `FolderClearRankMap`, and
`FolderClearDataByLongNoteMode` to `src/LibraryFolderClearData.h` under
`namespace chart_library`; update Main Menu callers to that namespace rather
than leaving aliases in the scene header. Add:

```cpp
chart_library::FolderClearDataByLongNoteMode
ChartRepository::Session::LoadFolderClearDataByLongNoteMode(
    const ScoreClearRankCache &scoreRanks);
```

Move the four existing streaming queries and aggregation body from `MainMenuLibrary.cpp` unchanged into repository query implementation. Keep folder-key formatting functions pure in `MainMenuLibrary.cpp`. The Task 1 four-statement budget remains exactly four.

- [ ] **Step 5: Convert Main Menu and Settings to sessions**

Replace `sqlite3 *db` in `MainMenuScene` with:

```cpp
std::optional<ChartRepository::Session> chartSession;
```

`ChartListPageCache` stores a non-owning `ChartRepository::Session *`. Main Menu opens one session in `init()`, retains it for paging, resets the page cache before destroying it, and resets the session in `cleanupScene()`.

Keep `pageSize == 128`, `maxPages == 6`, the current LRU touch order, the
leading-course-record offset, and fallback-record behavior unchanged. Only the
stored connection type and query call change.

Every Settings background operation opens its own local session from `context.chartRepository`. Preserve the current one-session-per-task pattern and do not share the Main Menu session with background threads.

- [ ] **Step 6: Make ChartRepository context-owned**

Use this repository declaration order in `ApplicationContext`, before
`resultPersistence` and before the later `musicPlayer` member:

```cpp
ChartRepository chartRepository;
ScoreRepository scoreRepository;
ReplayRepository replayRepository;
MusicPlaylistRepository musicPlaylistRepository;
result_persistence::Coordinator resultPersistence;
```

Leave the existing intervening context fields in their current declaration
order; keep `music_player::MusicPlayerService musicPlayer` at its current later
position so it is destroyed before the repositories it references.

Initialize `resultPersistence(scoreRepository, replayRepository)` and
`musicPlayer(musicPlaylistRepository, chartRepository)`. Remove
`ChartRepository::GetInstance()` and replace runtime lookups with
`context.chartRepository` or explicit references.

Add the explicit migration-path configuration that replaces the last chart
singleton lookup in score schema migration:

```cpp
void ScoreRepository::SetChartDatabasePath(
    std::filesystem::path chartDatabasePath);
```

The default remains `Utils::GetDocumentsPath("db") / "chart.db"` for existing
standalone score validators. `initializeApplicationDatabases` first calls
`charts.EnsureReady()`, then supplies `charts.DatabasePath()` to scores before
`scores.EnsureSchema()`. The migration attaches that configured path and no
longer opens or calls a chart repository itself.

Change the aggregate initializer to:

```cpp
bool initializeChartDatabase(ChartRepository &charts);

DatabaseInitializationStatus initializeApplicationDatabases(
    ChartRepository &charts,
    ScoreRepository &scores,
    ReplayRepository &replays,
    MusicPlaylistRepository &music);
```

Replace the Task 5 three-argument overload and update the initializer
compile-time test to require this final four-argument form; do not retain the
intermediate overloads. Remove the zero-argument chart initializer with the
last chart singleton lookup.

Retain `initializeApplicationDatabasesWith`'s non-short-circuit sequence: all
four readiness calls populate the status even when an earlier one fails.

Pass the chart database path into score legacy migration state instead of looking up a chart singleton. Use `chart_storage_identity` from Task 2 in music and score code.

Change the music service constructor to:

```cpp
MusicPlayerService(MusicPlaylistRepository &playlists,
                   ChartRepository &charts);
```

Its library-favorite reads and writes open short chart sessions, eliminating
the temporary local chart `sqlite3*` use left by Task 5 while retaining the
same per-refresh connection lifetime.

- [ ] **Step 7: Pass chart repositories into presentation helpers**

Use:

```cpp
std::string difficultyLabelForChart(
    ChartRepository &charts,
    const bms_parser::ChartMeta &meta);
```

Implement the no-session convenience by opening one short repository session internally, matching current behavior for exporters. Main-menu hot paths continue using their retained session and must not call this convenience repeatedly.

- [ ] **Step 8: Verify chart behavior, concurrency, and query budgets**

Run:

```sh
cmake --build cmake-build-debug --target chart_repository_tests main_menu_library_tests score_cache_query_tests score_provenance_db_tests app_database_initializer_tests profile_switch_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'chart_repository_tests|main_menu_library_tests|score_cache_query_tests|foundation_provenance_db|app_database_initializer_tests|foundation_profile_switch'
rg -n 'ChartRepository::GetInstance|sqlite3|SqliteConnectionHandle' src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp src/scene/SettingsSceneTables.cpp src/scene/MainMenuLibrary.cpp src/audio/MusicPlayerService.h src/audio/MusicPlayerService.cpp
```

Expected: tests pass, folder aggregation still executes four statements, attached-score tests retain their plan, and `rg` returns no matches.

- [ ] **Step 9: Commit chart sessions**

```sh
git add src tests CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "refactor: add chart repository sessions"
```

---

### Task 7: Remove raw score/replay compatibility APIs and split their implementations

**Files:**
- Create: `src/repositories/ScoreRepositoryInternal.h`
- Create: `src/repositories/ScoreRepositorySchema.cpp`
- Create: `src/repositories/ScoreRepositoryQueries.cpp`
- Create: `src/repositories/ReplayRepositoryInternal.h`
- Create: `src/repositories/ReplayRepositorySchema.cpp`
- Create: `src/repositories/ReplayRepositoryRecords.cpp`
- Modify: `src/repositories/ScoreRepository.h/.cpp`
- Modify: `src/repositories/ReplayRepository.h/.cpp`
- Modify: `tests/score_provenance_db_tests.cpp`
- Modify: `tests/replay_repository_tests.cpp`
- Modify: `tests/result_persistence_integration_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: the context-owned domain APIs from Tasks 4 and 6.
- Produces: repository headers with no public raw connections; focused schema/query/record implementation units.

- [ ] **Step 1: Change tests to the final domain-only API**

Replace tests that call `Connect`, `Close`, `CreateScoreTable`, `CreateCourseScoreTable`, or `CreateReplayTables` with this pattern:

```cpp
ScoreRepository repository(databasePath);
assert(repository.EnsureSchema());

auto inspection = openDatabase(databasePath);
assert(queryInt(inspection.get(), "PRAGMA user_version") ==
       ScoreRepository::kCurrentSchemaVersion);
```

For future/corrupt fixtures, construct the fixture with the test's direct SQLite helper, snapshot the database family, call `EnsureSchema()`, then compare the family unchanged. For savepoint behavior, exercise the private migration through a malformed/stale on-disk fixture rather than retaining a public caller-owned transaction API.

Build the two repository tests. Expected: compilation fails until raw methods are removed and tests are fully migrated.

- [ ] **Step 2: Remove public raw methods**

Delete these public categories:

- `ScoreRepository::Connect` and `Close`.
- `ScoreRepository::CreateScoreTable` and `CreateCourseScoreTable` overloads
  that accept a `sqlite3 *`.
- `ScoreRepository::InsertScore` and `InsertCourseScore` overloads that accept
  a `sqlite3 *` in addition to their domain arguments.
- `ReplayRepository::Connect` and `Close`.
- `ReplayRepository::CreateReplayTables` and `RecoverCourseRecords` overloads
  that accept a `sqlite3 *`.

Keep all existing application-level save, load, cache, recovery, bind, ensure, and shutdown methods.

- [ ] **Step 3: Split schema/migration code without rewriting SQL**

`ScoreRepositorySchema.cpp` owns validated open, table/index creation, version reads, migrations 1–9, and schema validation. `ScoreRepositoryQueries.cpp` owns inserts, best-score reads, cache loading, and course recovery. `ScoreRepository.cpp` owns path/session mutex state and public orchestration.

`ReplayRepositorySchema.cpp` owns validated open, schema creation, migrations 1–5, and outbox schema validation. `ReplayRepositoryRecords.cpp` owns replay serialization/hydration, summary scanning, outbox operations, and course recovery. `ReplayRepository.cpp` owns path/session mutex state and public orchestration.

Internal headers declare only the exact free functions called across those units and remain in `namespace score_repository_detail` or `namespace replay_repository_detail`. Do not expose SQL constants through public headers.

- [ ] **Step 4: Re-run schema and recovery suites**

Run:

```sh
cmake --build cmake-build-debug --target score_provenance_db_tests replay_repository_tests result_persistence_integration_tests profile_switch_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'foundation_provenance_db|replay_repository_tests|result_persistence_integration_tests|foundation_profile_switch'
rg -n 'sqlite3|SqliteConnectionHandle' src/repositories/ScoreRepository.h src/repositories/ReplayRepository.h
```

Expected: all tests pass and the public-header `rg` command returns no matches.

- [ ] **Step 5: Commit the domain-only score/replay boundary**

```sh
git add src tests CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "refactor: hide score and replay storage details"
```

---

### Task 8: Split chart query and difficulty persistence from the session shell

**Files:**
- Create: `src/repositories/ChartRepositoryInternal.h`
- Create: `src/repositories/ChartRepositoryQueries.cpp`
- Create: `src/repositories/ChartRepositoryDifficulty.cpp`
- Modify: `src/repositories/ChartRepository.cpp`
- Modify: `src/repositories/ChartRepository.h`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: `ChartRepository::Session` and every passing chart repository test.
- Produces: focused chart query/difficulty implementation units with unchanged public signatures and SQL.

- [ ] **Step 1: Add a link-level extraction gate**

Add the new `.cpp` paths to `chart_repository_tests` before the functions are moved:

```cmake
src/repositories/ChartRepository.cpp
src/repositories/ChartRepositoryQueries.cpp
src/repositories/ChartRepositoryDifficulty.cpp
```

Build. Expected: CMake fails because the two new source files do not exist.

- [ ] **Step 2: Move query responsibilities**

Move, without SQL edits, chart selection/count/index lookup, favorites, difficulty-label caching, folder-clear aggregation, and row mapping into `ChartRepositoryQueries.cpp`.

`ChartRepositoryInternal.h` declares the internal functions using `sqlite3 *` and is included only by repository `.cpp` files:

```cpp
struct ChartRepository::Impl {
  std::filesystem::path databasePath;
  std::mutex readinessMutex;
  bool ready = false;
};

struct ChartRepository::Session::Impl {
  SqliteConnectionHandle database;
  ScoreRepository *scores = nullptr;
};

namespace chart_repository_detail {

bool EnsureCoreSchema(sqlite3 *database);
bool EnsureDifficultySchema(sqlite3 *database);
void InvalidateDifficultyLabelCache();
void BumpLibraryRevision();

} // namespace chart_repository_detail
```

Define `Session::QueryChartMeta`, `CountChartMeta`, `FindChartMetaIndex`,
favorites, label population, and folder aggregation directly in
`ChartRepositoryQueries.cpp`; define the difficulty schema, delete, and select
public methods directly in `ChartRepositoryDifficulty.cpp`. Keep file-local SQL
builders and row readers in anonymous namespaces in their owning translation
unit. The four detail-namespace declarations above are the only planned
cross-unit function seams; if a compiler error reveals a missed cohesive
helper, move its caller and implementation together instead of adding a generic
execute/query wrapper.

- [ ] **Step 3: Move difficulty persistence responsibilities**

Move difficulty schema creation, deletion, and difficulty/course selection into
`ChartRepositoryDifficulty.cpp`. Keep the interwoven
`ImportDifficultyTable`, URL update/list download, directory enumeration, JSON
parsing, and replacement transaction together in `ChartRepository.cpp` until
Task 11 introduces the parsed `difficulty_table::Document` seam; splitting
that workflow before the domain value exists would require a temporary storage
API.

- [ ] **Step 4: Verify no query or schema drift**

Run:

```sh
cmake --build cmake-build-debug --target chart_repository_tests main_menu_library_tests score_cache_query_tests profile_switch_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'chart_repository_tests|main_menu_library_tests|score_cache_query_tests|foundation_profile_switch'
```

Expected: all tests pass with unchanged Task 1 budgets.

- [ ] **Step 5: Commit the focused chart implementation**

```sh
git add src CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "refactor: split chart repository responsibilities"
```

---

### Task 9: Introduce a domain scan batch inside ChartRepository

**Files:**
- Create: `src/repositories/ChartScanStore.h`
- Create: `src/repositories/ChartScanStore.cpp`
- Modify: `src/repositories/ChartRepository.h/.cpp`
- Modify: `tests/chart_repository_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: the exact SQL mutations, checkpoint behavior, and prepared inserts currently embedded in `ScanChartRoots`.
- Produces: `ChartRepository::Session::ScanBatch`, a move-only domain writer used by `ChartLibraryScanner` without exposing SQLite.

- [ ] **Step 1: Write a failing scan-batch transaction test**

Add:

```cpp
const ChartScanCheckpoint checkpoint{
    .found = true,
    .scanSignature = "repository-test",
    .phase = "individual",
    .nextIndex = 1,
    .lastPath = chartMeta.BmsPath,
};
auto batch = session->BeginScanBatch();
assert(batch.has_value());
assert(batch->UpsertChart(chartMeta, std::nullopt));
assert(batch->CheckpointAndContinue(checkpoint));
assert(batch->Commit());
assert(session->CountAllChartMeta() == 1);

auto rollback = session->BeginScanBatch();
assert(rollback.has_value());
assert(rollback->DeleteChart(chartMeta.BmsPath));
rollback.reset();
assert(session->CountAllChartMeta() == 1);
```

Build. Expected: compilation fails because `ScanBatch` is absent.

- [ ] **Step 2: Add the move-only scan batch**

Put the record, update, checkpoint, and snapshot value types in
`ChartScanStore.h`; they contain no SQLite declarations. Include that header
from `ChartRepository.h`, where the nested batch declaration remains part of
the session API. The public surface is domain-specific:

```cpp
struct ChartSourcePreference {
  int priority = 0;
  std::uint64_t archiveSize = 0;
};

struct SolidArchiveRecord {
  std::filesystem::path path;
};

struct SolidArchiveUpdate {
  std::filesystem::path path;
  std::uint64_t uncompressedSize = 0;
  int fileCount = 0;
};

struct ArchiveScanCacheRecord {
  std::filesystem::path path;
  std::int64_t archiveSize = 0;
  std::int64_t mtimeNs = 0;
  bool solid = false;
  std::uint64_t uncompressedSize = 0;
  int fileCount = 0;
  int chartCount = -1;
};

struct ArchiveScanCacheUpdate {
  std::filesystem::path path;
  bool solid = false;
  std::uint64_t uncompressedSize = 0;
  int fileCount = 0;
  int chartCount = 0;
};

struct ChartSourcePreferenceUpdate {
  std::filesystem::path path;
  int priority = 0;
  std::uint64_t archiveSize = 0;
};

struct ChartScanCheckpoint {
  bool found = false;
  std::string scanSignature;
  std::string phase;
  int nextIndex = 0;
  int subIndex = 0;
  std::filesystem::path lastPath;
  std::filesystem::path archivePath;
  std::int64_t archiveSize = 0;
  std::int64_t archiveMtimeNs = 0;
  std::string lastInnerPath;
};

// Place this complete declaration in Session's public section before
// BeginScanBatch().
class ScanBatch {
public:
  ~ScanBatch();
  ScanBatch(ScanBatch &&) noexcept;
  ScanBatch &operator=(ScanBatch &&) noexcept;
  ScanBatch(const ScanBatch &) = delete;
  ScanBatch &operator=(const ScanBatch &) = delete;

  bool UpsertChart(
      const bms_parser::ChartMeta &meta,
      std::optional<ChartSourcePreference> sourcePreference);
  bool DeleteChart(const std::filesystem::path &path);
  bool DeleteChartsInArchive(const std::filesystem::path &path);
  bool DeleteSolidArchive(const std::filesystem::path &path);
  bool DeleteArchiveCache(const std::filesystem::path &path);
  bool UpsertSolidArchive(const SolidArchiveUpdate &update);
  bool UpsertArchiveCache(const ArchiveScanCacheUpdate &update);
  bool UpdateSourcePreference(const ChartSourcePreferenceUpdate &update);
  int CountChartsInArchive(const std::filesystem::path &path);
  bool CheckpointAndContinue(const ChartScanCheckpoint &checkpoint);
  bool Commit();
  int ChangedCount() const;

private:
  friend class Session;
  struct Impl;
  explicit ScanBatch(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

std::optional<ScanBatch> BeginScanBatch();
bool ClearScanCheckpoint();
bool ClearChartMetadataRebuildRequired();
```

Move the current prepared insert statement, transaction
open/commit/rollback logic, and revision-change count into
`ChartScanStore.cpp`. `CheckpointAndContinue` preserves the current boundary:
commit pending mutations, upsert the checkpoint, then begin the next scan
transaction. Flush request/completion callbacks remain in the scanner; it
acknowledges after `CheckpointAndContinue` returns even when checkpoint
persistence reports false, matching the current log-and-acknowledge behavior.
The destructor rolls back an active batch, while the scanner explicitly
commits the same partial work on cancellation that the current implementation
commits.

Replace `Session::Impl`'s direct connection member with a shared private
`ChartSessionStorage` containing the `SqliteConnectionHandle`; `Session` and an
active `ScanBatch` each retain that storage. This does not add a connection and
keeps batch rollback safe if the session object is moved or destroyed before
the batch. Scanner code still performs no concurrent session calls while a
batch transaction is active.

Keep archive mutations separate: stale solid archives delete the solid row and
cache row independently; reindexed archives delete only their chart rows; and
solid-state changes keep their current combination and ordering. Do not replace
those calls with one aggregate archive delete. `UpsertSolidArchive` and
`UpsertArchiveCache` re-stat the archive at write time just as the current
helpers do; the snapshot's stored `archiveSize` and `mtimeNs` are comparison
data, not write input. `CountChartsInArchive` runs inside the active batch so
the cache records the rows actually inserted before a checkpoint.

Keep `ClearScanCheckpoint` and `ClearChartMetadataRebuildRequired` as session
operations outside `ScanBatch`. The scanner calls them in the same three places
as today: stale-checkpoint rejection before the write transaction, the no-work
return, and the final path after batch commit whenever the stop token itself is
not requested (including a pause-callback rejection, matching the current
condition). Do not fold either clear into a batch transaction.

- [ ] **Step 3: Add read-side scan snapshots**

Add session methods returning the current domain records needed by scanning:

```cpp
ChartScanSnapshot LoadScanSnapshot();
std::optional<ScanBatch> BeginScanBatch();
bool ClearScanCheckpoint();
bool ClearChartMetadataRebuildRequired();
```

`ChartScanSnapshot` contains
`std::vector<bms_parser::ChartMeta> charts`,
`std::vector<SolidArchiveRecord> solidArchives`,
`std::vector<ArchiveScanCacheRecord> archiveCache`, and
`std::optional<ChartScanCheckpoint> checkpoint`. It does not contain a database
handle or statement.

Refactor the transitional `Session::ScanChartRoots` in place to consume this
snapshot and batch immediately. Keep all enumeration, parsing, concurrency,
progress, pause/cancel, flush, and checkpoint-decision code in its current
file and order; replace only its direct reads/mutations and transaction calls.
This prevents a duplicate raw-SQL scan path from surviving into Task 10.

- [ ] **Step 4: Verify batching and statement reuse**

Before opening the repository session, install a test-local SQLite
auto-extension that places both an authorizer and a statement trace on each new
connection. The authorizer increments `chartMetaInsertPrepares` only for
`action == SQLITE_INSERT && first == "chart_meta"`; authorizers run when a
statement is prepared, not when it is reset. The statement trace counts SQL
whose trimmed uppercase text begins with `BEGIN` or `COMMIT`.

Capture the authorizer baseline after schema setup, then open one scan batch,
call `UpsertChart` 100 times, and commit. Assert exactly one additional
`chart_meta` insert authorization, one scan `BEGIN`, one scan `COMMIT`, and 100
traced executions of the identical chart insert SQL. This distinguishes actual
prepared-statement reuse from merely seeing the same SQL text 100 times and
does not expose a test-only native handle.

Run:

```sh
cmake --build cmake-build-debug --target chart_repository_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R chart_repository_tests
```

Expected: the scan batch tests pass.

- [ ] **Step 5: Commit the scan storage seam**

```sh
git add src tests CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "refactor: add chart scan repository batch"
```

---

### Task 10: Extract filesystem and archive scanning into ChartLibraryScanner

**Files:**
- Create: `src/ChartLibraryScanner.h`
- Create: `src/ChartLibraryScanner.cpp`
- Create: `tests/chart_library_scanner_tests.cpp`
- Modify: `src/repositories/ChartRepository.h/.cpp`
- Modify: `src/scene/MainMenuScene.h/.cpp`
- Modify: `src/scene/SettingsSceneTables.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: `ChartRepository::Session`, `ChartScanSnapshot`, and `ScanBatch`.
- Produces: `ChartLibraryScanner::Scan` with the existing progress, pause, flush, cancellation, checkpoint, parser, and archive concurrency behavior.

- [ ] **Step 1: Write failing scanner behavior tests**

Use a temporary repository and write `fixtureRoot / "sample.bms"` with this
exact ASCII fixture (no repository fixture currently exists):

```text
#PLAYER 1
#GENRE Test
#TITLE Repository Scanner
#ARTIST AsoBMaShow Test
#BPM 120
#PLAYLEVEL 1
#RANK 2
#TOTAL 100
#WAV01 sample.wav
#00111:01
```

Also create an empty `sample.wav` beside the chart so referenced-asset
discovery follows the normal path without requiring an audio decoder.

Test:

```cpp
ChartLibraryScanner scanner;
const std::uint64_t beforeRevision = repository.GetLibraryRevision();
const int changed = scanner.Scan(
    *session, {fixtureRoot}, &stopToken,
    [&](const ChartScanProgress &value) { progress.push_back(value); });

assert(changed == 1);
assert(session->CountAllChartMeta() == 1);
assert(repository.GetLibraryRevision() > beforeRevision);
assert(!progress.empty());
assert(progress.front().stage == ChartScanProgressStage::Preparing);
```

Add cases for repeated no-op scan (including an unchanged library revision),
stop requested before work, pause callback rejection, deleted file removal
(including a revision increase), and checkpoint resume. For storage failure,
install a test auto-extension authorizer that denies the single fixture's
`SQLITE_INSERT` on `chart_meta`; assert the scan returns the current zero-change
result, emits the current insert diagnostic, and leaves no chart row. Do not
invent all-or-nothing behavior for a multi-file scan: the present scanner
commits at checkpoints and commits its current partial batch on cancellation.
Build the new target.

Expected: compilation fails because `ChartLibraryScanner` does not exist.

- [ ] **Step 2: Move non-SQL scanning code**

Move filesystem enumeration, archive inspection, chart parsing, worker-count selection, streaming/prefetch concurrency, cancellation checks, progress callbacks, and checkpoint decision logic from `ChartRepository.cpp` into `ChartLibraryScanner.cpp`.

Replace each direct SQL operation with `LoadScanSnapshot` or `ScanBatch`. Keep:

```cpp
int Scan(ChartRepository::Session &session,
         const std::vector<std::filesystem::path> &roots,
         const std::stop_token *stopToken = nullptr,
         ChartScanProgressCallback progressCallback = nullptr,
         ChartScanPauseCallback pauseCallback = nullptr,
         ChartScanFlushRequestCallback flushRequestCallback = nullptr,
         ChartScanFlushCompleteCallback flushCompleteCallback = nullptr);
```

Do not change worker caps, in-flight byte budgets, ordering, checkpoint intervals, or log messages.

- [ ] **Step 3: Remove scanning from ChartRepository**

Delete `Session::ScanChartRoots` and all filesystem/archive/parser includes from repository files. Main Menu and Settings construct a scanner for each background scan and pass their independently opened session.

- [ ] **Step 4: Verify scan behavior and performance**

Run:

```sh
cmake --build cmake-build-debug --target chart_library_scanner_tests chart_repository_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'chart_library_scanner_tests|chart_repository_tests'
rg -n '(ArchiveFile|bms_parser::Parser|readArchive|std::filesystem::recursive_directory_iterator)' src/repositories/ChartRepository*.cpp
```

Expected: tests pass and `rg` returns no scanning/parser implementation matches in repository files.

- [ ] **Step 5: Commit scanner extraction**

```sh
git add src tests CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "refactor: extract chart library scanner"
```

---

### Task 11: Extract difficulty-table loading and parsing

**Files:**
- Create: `src/DifficultyTableModel.h`
- Create: `src/DifficultyTableImporter.h`
- Create: `src/DifficultyTableImporter.cpp`
- Create: `tests/difficulty_table_importer_tests.cpp`
- Modify: `src/repositories/ChartRepository.h`
- Modify: `src/repositories/ChartRepository.cpp`
- Modify: `src/repositories/ChartRepositoryDifficulty.cpp`
- Modify: `src/scene/MainMenuScene.h/.cpp`
- Modify: `src/scene/SettingsSceneTables.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: existing difficulty header/data JSON behavior and repository difficulty transaction.
- Produces: parsed `difficulty_table::Document` values, `DifficultyTableImporter` I/O methods, and `ChartRepository::Session::ReplaceDifficultyTable`.

- [ ] **Step 1: Write failing parser and rollback tests**

Use these exact in-memory fixtures:

```cpp
const std::string sha256(64, 'a');
const std::string md5(32, 'b');
const std::string sourceUrl = "https://example.test/table/header.json";
const std::string headerJson =
    "{\"name\":\"Test Table\",\"symbol\":\"*\","
    "\"data_url\":\"data.json\",\"level_order\":[\"1\"],"
    "\"course\":[{\"name\":\"Course *1\","
    "\"constraint\":[\"gauge_lr2\"],\"charts\":[{\"sha256\":\"" +
    sha256 + "\"}]}]}";
const std::string dataJson =
    "[{\"level\":\"1\",\"md5\":\"" + md5 +
    "\",\"sha256\":\"" + sha256 +
    "\",\"title\":\"Chart\",\"subtitle\":\"Sub\","
    "\"artist\":\"Artist\",\"subartist\":\"Subartist\","
    "\"url\":\"chart.zip\",\"url_diff\":\"patch.zip\"}]";
```

Test:

```cpp
const auto parsed =
    difficulty_table::Parse(headerJson, dataJson, sourceUrl, error);
assert(parsed.has_value());
assert(parsed->levelOrder == std::vector<std::string>{"1"});
assert(parsed->charts.size() == 1);
assert(parsed->charts.front().subtitle == "Sub");
assert(parsed->charts.front().subartist == "Subartist");
assert(parsed->charts.front().urlDiff == "patch.zip");
assert(parsed->courses.size() == 1);
assert(parsed->courses.front().groupName == "Course");
assert(parsed->courses.front().level == "*1");
assert(parsed->courses.front().constraintJson == "[\"gauge_lr2\"]");
assert(parsed->courses.front().charts.size() == 1);
assert(parsed->courses.front().charts.front().md5 == md5);
assert(session->ReplaceDifficultyTable(*parsed));
```

Snapshot all four difficulty tables. Install an auto-extension authorizer before
opening the session, enable it only for the second replacement, and deny
`SQLITE_INSERT` on `difficulty_course_entries`. Assert
`ReplaceDifficultyTable` returns false and all four table snapshots remain
byte-for-byte identical; this exercises rollback after earlier rows in the
transaction have changed.

Construct an importer with a fake text fetcher that returns `headerJson` for
`https://example.test/table/header.json` and `dataJson` for
`https://example.test/table/data.json`. Assert those two URLs are requested in
order and the existing single-table progress callback receives exactly
`{current = 1, total = 1, tableName = "Test Table"}` before persistence.

Expected: compilation fails because the model/parser/repository method do not exist.

- [ ] **Step 2: Define the parsed domain model**

`DifficultyTableModel.h` contains concrete values only:

```cpp
namespace difficulty_table {

struct Chart {
  std::string level;
  std::string md5;
  std::string sha256;
  std::string title;
  std::string subtitle;
  std::string artist;
  std::string subartist;
  std::string url;
  std::string urlDiff;
};

struct Course {
  std::string name;
  std::string groupName;
  std::string level;
  std::string constraintJson;
  std::vector<Chart> charts;
};

struct Document {
  std::string name;
  std::string symbol;
  std::string sourceUrl;
  std::string dataUrl;
  std::vector<std::string> levelOrder;
  std::vector<Chart> charts;
  std::vector<Course> courses;
};

std::optional<Document>
Parse(const std::string &headerJson,
      const std::string &dataJson,
      const std::string &sourceUrl,
      std::string &errorMessage);

} // namespace difficulty_table
```

These fields exactly cover the current `TableChartItem`, header level ordering,
and course persistence inputs. Parsing retains the current normalization,
stable level sorting, course-name group/level split, chart metadata fill, and
unambiguous counterpart-hash enrichment. The repository derives
`course_identity::ChartIdentity` values from each course chart and retains the
existing course ID/key when `sameDefinition` matches.

- [ ] **Step 3: Move I/O and parsing to DifficultyTableImporter**

Move page/data URL resolution, curl download, local directory enumeration, JSON parsing, normalization, and progress reporting out of repository files. Use:

```cpp
using DifficultyTableTextFetcher = std::function<std::optional<std::string>(
    const std::string &url, std::string *errorMessage)>;

class DifficultyTableImporter {
public:
  DifficultyTableImporter();
  explicit DifficultyTableImporter(DifficultyTableTextFetcher fetchText);

  bool ImportFromUrl(ChartRepository::Session &session,
                     const std::string &pageUrl,
                     std::string *errorMessage = nullptr,
                     DifficultyTableImportProgressCallback progress = nullptr);
  bool UpdateFromSourceUrl(
      ChartRepository::Session &session, int tableId,
      std::string *errorMessage = nullptr);
  int ImportFromDirectory(
      ChartRepository::Session &session,
      const std::filesystem::path &directory);
};
```

The default constructor uses the current platform-specific fetch implementation;
the injected constructor is for deterministic tests and does not alter runtime
policy, redirects, timeouts, trust-store setup, or concurrent table-list
downloads. The repository accepts a parsed `Document` and performs the current
single transaction. Main Menu and Settings construct an importer for I/O
actions and call repository selection/deletion methods directly for
persistence-only actions.

For a table-list URL, build the existing-source set once from
`session.SelectDifficultyTables()` before launching the same maximum of four
downloads. Preserve recursive-list rejection, duplicate URL removal,
completion-driven import order, skipped/imported/failed counts, first-error
selection, progress messages, and the final sanitized summary string. Add each
successful document's source URL to the set so the rest of the same import
observes it.

`UpdateFromSourceUrl` finds `tableId` in
`session.SelectDifficultyTables()` and uses that record's existing `sourceUrl`;
it returns the current "does not have an updateable source URL" error when the
record is absent or the URL is not HTTP(S). No repository method downloads or
parses JSON after this task.

- [ ] **Step 4: Verify import compatibility**

Run:

```sh
cmake --build cmake-build-debug --target difficulty_table_importer_tests chart_repository_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'difficulty_table_importer_tests|chart_repository_tests'
rg -n '(CURL|curl_|Download|nlohmann::json|ifstream)' src/repositories/ChartRepository*.cpp src/repositories/ChartRepositoryDifficulty.cpp
```

Expected: tests pass and `rg` finds no network/file/JSON parsing in repository files.

- [ ] **Step 5: Commit difficulty import extraction**

```sh
git add src tests CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "refactor: extract difficulty table importer"
```

---

### Task 12: Enforce final boundaries and run full compatibility/performance verification

**Files:**
- Create: `tests/repository_boundary_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `src/audio/CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`
- Modify: any source that violates the final audit

**Interfaces:**
- Consumes: every final repository and application-service boundary.
- Produces: a durable source-boundary test plus fresh full-suite/build/performance evidence.

- [ ] **Step 1: Add a source-boundary test**

Register the test with:

```cmake
add_executable(repository_boundary_tests
    tests/repository_boundary_tests.cpp)
target_compile_features(repository_boundary_tests PRIVATE cxx_std_23)
target_compile_definitions(repository_boundary_tests PRIVATE
    ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
```

After the existing `asobmashow_register_test` function is defined in
`CMakeLists.txt`, register it with:

```cmake
asobmashow_register_test(repository_boundary_tests)
```

The complete test algorithm is:

```cpp
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

std::string readText(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool isSource(const fs::path &path) {
  const std::string extension = path.extension().string();
  return extension == ".h" || extension == ".hpp" ||
         extension == ".c" || extension == ".cpp";
}

bool isPublicRepositoryHeader(const fs::path &relative) {
  const std::string value = relative.generic_string();
  return value == "repositories/ChartRepository.h" ||
         value == "repositories/ScoreRepository.h" ||
         value == "repositories/ReplayRepository.h" ||
         value == "repositories/MusicPlaylistRepository.h";
}

bool mayOwnSqlite(const fs::path &relative) {
  const std::string value = relative.generic_string();
  return value.starts_with("repositories/") ||
         value == "ProfileDatabaseTools.cpp" ||
         value == "ProfileDatabaseTools.h" ||
         value == "sqlite3.c" || value == "sqlite3.h";
}

int main() {
  const fs::path sourceRoot = fs::path(ASOBMASHOW_SOURCE_DIR) / "src";
  const std::array<std::string_view, 4> obsolete{
      "ChartDBHelper", "ScoreDBHelper",
      "ReplayDBHelper", "MusicPlaylistDB"};
  const std::array<std::string_view, 4> sqliteTokens{
      "sqlite3", "SqliteConnectionHandle",
      "SqliteStatementHandle", "SqliteTransactionHandle"};
  std::vector<std::string> failures;

  for (const fs::directory_entry &entry :
       fs::recursive_directory_iterator(sourceRoot)) {
    if (!entry.is_regular_file() || !isSource(entry.path())) {
      continue;
    }
    const fs::path relative = fs::relative(entry.path(), sourceRoot);
    const std::string text = readText(entry.path());
    for (std::string_view symbol : obsolete) {
      if (text.find(symbol) != std::string::npos) {
        failures.push_back(relative.generic_string() +
                           ": obsolete " + std::string(symbol));
      }
    }
    if (relative.generic_string().starts_with("repositories/") &&
        (relative.extension() == ".h" ||
         relative.extension() == ".hpp") &&
        text.find("GetInstance") != std::string::npos) {
      failures.push_back(relative.generic_string() +
                         ": repository singleton");
    }
    if (!mayOwnSqlite(relative) || isPublicRepositoryHeader(relative)) {
      for (std::string_view token : sqliteTokens) {
        if (text.find(token) != std::string::npos) {
          failures.push_back(relative.generic_string() +
                             ": raw SQLite token " + std::string(token));
        }
      }
    }
  }

  for (const std::string &failure : failures) {
    std::cerr << failure << '\n';
  }
  return failures.empty() ? 0 : 1;
}
```

This allows only repository implementation/support files, the existing profile
backup/validation tool, and the bundled SQLite source to own raw SQLite under
`src`. The four public repository headers remain raw-SQLite-free. Tests are
outside `src` and may continue using direct SQLite fixtures and inspection.

- [ ] **Step 2: Run the boundary test red, then remove every violation**

Run:

```sh
cmake --build cmake-build-debug --target repository_boundary_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R repository_boundary_tests
```

Expected on the first run: failure listing any leftover boundary violations. Remove those exact violations, rerun, and expect a pass.

- [ ] **Step 3: Verify schemas and migrations**

Run:

```sh
cmake --build cmake-build-debug --target chart_repository_tests music_playlist_repository_tests replay_repository_tests score_provenance_db_tests result_persistence_integration_tests profile_switch_tests profile_archive_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'chart_repository_tests|music_playlist_repository_tests|replay_repository_tests|foundation_provenance_db|result_persistence_integration_tests|foundation_profile_switch|foundation_profile_archive'
```

Expected: all tests pass, including score/replay/music byte-family
preservation, the new chart future/corrupt unchanged-family cases, supported
chart-schema compatibility, and schema snapshots.

- [ ] **Step 4: Verify deterministic performance contracts**

Run:

```sh
ctest --test-dir cmake-build-debug --output-on-failure -R 'main_menu_library_tests|score_cache_query_tests|replay_repository_tests|chart_repository_tests|chart_library_scanner_tests|music_playlist_repository_tests'
```

Expected: four folder-clear statements, unchanged attached-score index plans, bounded replay scans, one retained connection per hot owner, and prepared scan inserts.

Run the focused set three times with `/usr/bin/time -p`. Compare medians with Task 1. Investigate any repeatable slowdown above 10%; do not fail based on a single timing sample.

- [ ] **Step 5: Run the complete CTest suite**

Run:

```sh
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: 100% tests passed, 0 failed.

- [ ] **Step 6: Run the prescribed desktop build**

Run:

```sh
cmake --build cmake-build-debug --target main -j 6
```

Expected: exit code 0.

- [ ] **Step 7: Audit names, paths, and iOS membership**

Run:

```sh
rg -n 'ChartDBHelper|ScoreDBHelper|ReplayDBHelper|MusicPlaylistDB' src CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
rg -n 'GetInstance' src/repositories
find src/repositories -maxdepth 1 -type f | sort
git diff --check
```

Expected: the obsolete-name and repository-`GetInstance` searches return no source matches; repository files match the File Structure section; `git diff --check` is clean. Verify every new `.cpp` appears once in CMake and once in the Xcode `membershipExceptions` list.

- [ ] **Step 8: Commit final enforcement and fixes**

```sh
git add src tests CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "test: enforce repository architecture"
```

## Execution Notes

- Execute tasks in order. Task 6 is the dependency hinge: do not remove score raw-attachment APIs before `ChartRepository::Session` exists.
- After every production edit, run the smallest named target first, then the task's complete focused set.
- Keep commits limited to the listed task. The approved chart-open fix belongs
  in Task 6. If another major, straightforward correctness or data-safety bug
  appears, first add its deterministic regression test, isolate the change in
  the current task's commit, and report it explicitly; otherwise record it for
  separate work.
- Preserve unrelated working-tree changes. Before each commit, inspect `git status --short` and stage only task files.
- Use `scripts/ios_firebase_deploy.sh --build-only` only if a later explicit request asks for iOS verification beyond the required desktop build. Never run the upload path without explicit deployment authorization.
