#include "../src/AppSettingsStore.h"
#include "../src/AtomicFile.h"
#include "../src/ChartDBHelper.h"
#include "../src/PlayerProfileManager.h"
#include "../src/ProfileDatabaseTools.h"
#include "../src/ReplayDBHelper.h"
#include "../src/ScoreDBHelper.h"
#include "../src/input/InputProfileStore.h"
#include "../src/sqlite3.h"
#include "../yoga/lib/nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

ChartDBHelper::ChartDBHelper() = default;
sqlite3 *ChartDBHelper::Connect() { return nullptr; }
bool ChartDBHelper::CreateChartMetaTable(sqlite3 *) { return false; }

namespace {
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  explicit TempDirectory(std::string_view label = "profile") {
    static std::atomic<unsigned long long> sequence{0};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-" + std::string(label) + "-" + std::to_string(nonce) +
             "-" + std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path(__FILE__).parent_path() / "fixtures" /
         "profiles" / name;
}

void writeFile(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool execute(sqlite3 *database, const char *sql) {
  char *error = nullptr;
  const int rc = sqlite3_exec(database, sql, nullptr, nullptr, &error);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL failure: " << (error == nullptr ? "unknown" : error)
              << '\n';
    sqlite3_free(error);
    return false;
  }
  return true;
}

struct DatabaseCloser {
  void operator()(sqlite3 *database) const {
    if (database != nullptr) {
      sqlite3_close(database);
    }
  }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

Database openDatabase(const std::filesystem::path &path) {
  sqlite3 *raw = nullptr;
  if (sqlite3_open(path.string().c_str(), &raw) != SQLITE_OK) {
    if (raw != nullptr) {
      std::cerr << "Unable to open test database: " << sqlite3_errmsg(raw)
                << '\n';
      sqlite3_close(raw);
    }
    return {};
  }
  return Database(raw);
}

PlayerProfileManagerDependencies
dependenciesFor(std::string uuid = "11111111-1111-4111-8111-111111111111",
                std::string timestamp = "2026-07-10T12:34:56Z") {
  PlayerProfileManagerDependencies dependencies;
  dependencies.generateUuid = [uuid = std::move(uuid)] { return uuid; };
  dependencies.utcNow = [timestamp = std::move(timestamp)] {
    return timestamp;
  };
  return dependencies;
}

std::vector<std::filesystem::path>
stagingDirectories(const std::filesystem::path &root) {
  std::vector<std::filesystem::path> result;
  const auto profiles = root / "profiles";
  std::error_code error;
  if (!std::filesystem::exists(profiles, error)) {
    return result;
  }
  for (const auto &entry : std::filesystem::directory_iterator(profiles)) {
    if (entry.path().filename().string().starts_with(".staging-")) {
      result.push_back(entry.path());
    }
  }
  return result;
}

bool removeTree(const std::filesystem::path &path, std::string &errorMessage) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error) {
    errorMessage = "unable to remove test tree: " + error.message();
    return false;
  }
  return true;
}

struct LegacyData {
  Database scoreConnection;
  Database replayConnection;
};

LegacyData seedLegacyData(const std::filesystem::path &root) {
  std::filesystem::create_directories(root / "db");
  std::filesystem::copy_file(fixture("legacy-settings.cfg"),
                             root / "settings.cfg",
                             std::filesystem::copy_options::overwrite_existing);

  const auto scorePath = root / "db" / "score.db";
  const auto replayPath = root / "db" / "replay.db";
  ScoreDBHelper scoreHelper(scorePath);
  ReplayDBHelper replayHelper(replayPath);
  expect(scoreHelper.EnsureSchema(), "legacy score schema initializes");
  expect(replayHelper.EnsureSchema(), "legacy replay schema initializes");

  LegacyData result{openDatabase(scorePath), openDatabase(replayPath)};
  expect(result.scoreConnection != nullptr, "legacy score database opens");
  expect(result.replayConnection != nullptr, "legacy replay database opens");
  if (!result.scoreConnection || !result.replayConnection) {
    return result;
  }

  expect(execute(result.scoreConnection.get(), "PRAGMA journal_mode=WAL"),
         "score database enters WAL mode");
  expect(execute(result.scoreConnection.get(), "PRAGMA wal_autocheckpoint=0"),
         "score auto-checkpoint is disabled");
  expect(
      execute(
          result.scoreConnection.get(),
          "INSERT INTO scores (chart_path,chart_md5,chart_sha256,ln_mode,"
          "chart_title,chart_artist,score,max_score,max_combo,combo_break,"
          "pgreat,great,good,bad,poor,kpoor,fast,slow,final_gauge,clear_type) "
          "VALUES ('legacy.bms','0123456789abcdef0123456789abcdef',"
          "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
          "0,'Legacy Song','Legacy Artist',100,200,3,4,5,6,7,8,9,10,11,12,"
          "0.75,2)"),
      "WAL-backed score row inserts");
  expect(
      execute(result.scoreConnection.get(),
              "INSERT INTO course_scores (course_id,course_key,course_name,"
              "course_group_name,constraint_json,gauge_type,gauge_profile,"
              "gauge_auto_shift,play_option,assist_option,completed_charts,"
              "total_charts,score,max_score,max_combo,combo_break,pgreat,great,"
              "good,bad,poor,kpoor,fast,slow,final_gauge,clear_type) VALUES "
              "(7,'course-key','Legacy Course','Group','[]',0,0,0,'OFF','OFF',"
              "1,1,100,200,3,4,5,6,7,8,9,10,11,12,0.5,2)"),
      "WAL-backed course score row inserts");

  expect(execute(result.replayConnection.get(), "PRAGMA journal_mode=WAL"),
         "replay database enters WAL mode");
  expect(execute(result.replayConnection.get(), "PRAGMA wal_autocheckpoint=0"),
         "replay auto-checkpoint is disabled");
  expect(
      execute(
          result.replayConnection.get(),
          "INSERT INTO replays (chart_path,chart_md5,chart_sha256,"
          "chart_title,chart_artist,ln_mode,gauge_type,gauge_auto_shift,"
          "final_score,max_combo,final_gauge,clear_type,assist_option) VALUES "
          "('legacy.bms','0123456789abcdef0123456789abcdef',"
          "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
          "'Legacy Song','Legacy Artist',0,0,0,100,3,0.75,2,'OFF')"),
      "WAL-backed replay row inserts");
  expect(
      execute(result.replayConnection.get(),
              "INSERT INTO replay_events (replay_id,event_index,action,lane,"
              "note_time_micros,song_time_micros,judge_time_micros,judgement,"
              "diff_micros,gauge,gauge_type,combo,score) VALUES "
              "(1,0,1,2,1000,1000,1000,0,0,0.75,0,1,2)"),
      "WAL-backed replay event row inserts");
  return result;
}

std::int64_t rowCount(const std::filesystem::path &database,
                      std::string_view table) {
  std::string error;
  const auto count = sqliteTableRowCount(database, table, error);
  expect(count.has_value(),
         "row count for " + std::string(table) + " succeeds: " + error);
  return count.value_or(-1);
}

void setDatabaseVersion(const std::filesystem::path &path, int version,
                        std::string_view label) {
  Database database = openDatabase(path);
  expect(database != nullptr, std::string(label) + " database opens");
  if (!database) {
    return;
  }
  expect(execute(database.get(),
                 ("PRAGMA user_version=" + std::to_string(version)).c_str()),
         std::string(label) + " database version updates");
}

void testSqliteSnapshotIncludesWalAndValidatesIdentifiers() {
  TempDirectory temp("sqlite-snapshot");
  const auto source = temp.path() / "source.db";
  const auto destination = temp.path() / "destination.db";
  Database database = openDatabase(source);
  expect(database != nullptr, "snapshot source opens");
  if (!database) {
    return;
  }
  expect(execute(database.get(), "PRAGMA journal_mode=WAL"),
         "snapshot source enables WAL");
  expect(execute(database.get(), "PRAGMA wal_autocheckpoint=0"),
         "snapshot source disables auto-checkpoint");
  expect(
      execute(database.get(),
              "CREATE TABLE rows_from_wal(id INTEGER PRIMARY KEY, value TEXT)"),
      "snapshot test table creates");
  expect(execute(database.get(),
                 "INSERT INTO rows_from_wal(value) VALUES ('uncheckpointed')"),
         "snapshot WAL row inserts");

  std::string error;
  expect(snapshotSqliteDatabase(source, destination, error),
         "SQLite backup snapshot succeeds: " + error);
  expect(rowCount(destination, "rows_from_wal") == 1,
         "SQLite backup includes uncheckpointed WAL rows");
  expect(sqliteIntegrityCheck(destination, error),
         "snapshot passes integrity check: " + error);
  expect(!sqliteTableRowCount(destination, "rows_from_wal; DROP TABLE x", error)
              .has_value(),
         "unsafe table identifiers are rejected");
}

void testFirstRunMigrationIsLosslessAndIdempotent() {
  TempDirectory temp("profile-migration");
  LegacyData legacy = seedLegacyData(temp.path());
  std::filesystem::create_directories(temp.path() / "profiles" /
                                      ".staging-abandoned");
  writeFile(temp.path() / "profiles" / ".staging-abandoned" / "partial",
            "partial");
  std::filesystem::create_directories(temp.path() / "profiles" / ".keep-me");

  PlayerProfileManager manager(temp.path(), dependenciesFor());
  const ProfileResult initialized = manager.Initialize();
  expect(initialized.ok(),
         "first-run profile migration succeeds: " + initialized.message);
  if (!initialized.ok()) {
    return;
  }

  const auto expectedId = "11111111-1111-4111-8111-111111111111";
  expect(manager.activeProfile().id == expectedId,
         "generated UUID becomes active profile ID");
  const auto paths = manager.activePaths();
  expect(paths.root == temp.path() / "profiles" / expectedId,
         "profile root follows fixed layout");
  expect(paths.profileJson == paths.root / "profile.json" &&
             paths.settingsJson == paths.root / "settings.json" &&
             paths.inputJson == paths.root / "input.json" &&
             paths.scoresDb == paths.root / "scores.db" &&
             paths.replaysDb == paths.root / "replays.db",
         "all profile paths follow fixed layout");

  const auto settings = AppSettingsStore::Load(paths.settingsJson);
  expect(settings.status == AppSettingsLoadStatus::Loaded,
         "migrated settings JSON loads");
  expect(settings.settings.audioOffsetMs == -23 &&
             settings.settings.visibleTimeGreenNumber == 777 &&
             settings.settings.selectedPlayOption == "R-RANDOM" &&
             settings.settings.selectedPacemakerTarget == "AAA",
         "legacy settings convert exactly into versioned settings");
  const auto input = InputProfileStore::load(paths.inputJson);
  expect(input.status == InputProfileLoadStatus::Loaded,
         "default input profile is persisted");
  expect(!input.profile.bindings.empty(),
         "persisted input profile contains keyboard defaults");

  expect(rowCount(paths.scoresDb, "scores") == 1,
         "score rows survive migration");
  expect(rowCount(paths.scoresDb, "course_scores") == 1,
         "course score rows survive migration");
  expect(rowCount(paths.replaysDb, "replays") == 1,
         "replay rows survive migration");
  expect(rowCount(paths.replaysDb, "replay_events") == 1,
         "replay event rows survive migration");

  std::string integrityError;
  expect(sqliteIntegrityCheck(paths.scoresDb, integrityError),
         "migrated score database is integral: " + integrityError);
  expect(sqliteIntegrityCheck(paths.replaysDb, integrityError),
         "migrated replay database is integral: " + integrityError);
  expect(std::filesystem::exists(temp.path() / "settings.cfg") &&
             std::filesystem::exists(temp.path() / "db" / "score.db") &&
             std::filesystem::exists(temp.path() / "db" / "replay.db"),
         "legacy source files are retained");
  expect(stagingDirectories(temp.path()).empty(),
         "abandoned and completed staging directories are removed");
  expect(std::filesystem::exists(temp.path() / "profiles" / ".keep-me"),
         "cleanup does not remove unrelated hidden directories");

  const std::string bootstrapBefore =
      readFile(temp.path() / "active-profile.json");
  PlayerProfileManager second(
      temp.path(), dependenciesFor("22222222-2222-4222-8222-222222222222",
                                   "2026-07-11T00:00:00Z"));
  const auto secondResult = second.Initialize();
  expect(secondResult.ok(), "second initialization succeeds");
  expect(second.activeProfile().id == expectedId,
         "second initialization reuses the active profile");
  expect(second.listProfiles().size() == 1,
         "second initialization does not duplicate profiles");
  expect(readFile(temp.path() / "active-profile.json") == bootstrapBefore,
         "idempotent initialization does not rewrite bootstrap");
}

void testEveryPreFinalizeFailureCleansStaging() {
  const std::array phases = {
      ProfileMigrationPhase::PrepareStaging,
      ProfileMigrationPhase::WriteSettings,
      ProfileMigrationPhase::WriteInput,
      ProfileMigrationPhase::SnapshotScores,
      ProfileMigrationPhase::SnapshotReplays,
      ProfileMigrationPhase::EnsureScoreSchema,
      ProfileMigrationPhase::EnsureReplaySchema,
      ProfileMigrationPhase::ValidateIntegrity,
      ProfileMigrationPhase::CompareRows,
      ProfileMigrationPhase::WriteMetadata,
      ProfileMigrationPhase::FinalizeProfile,
  };

  for (const ProfileMigrationPhase phase : phases) {
    TempDirectory temp("profile-failure");
    writeFile(temp.path() / "settings.cfg", "audio_offset_ms=-17\n");
    const std::string sourceBefore = readFile(temp.path() / "settings.cfg");
    auto dependencies = dependenciesFor();
    dependencies.beforeMigrationPhase = [phase](ProfileMigrationPhase current,
                                                std::string &error) {
      if (current == phase) {
        error = "injected migration phase failure";
        return false;
      }
      return true;
    };
    PlayerProfileManager manager(temp.path(), std::move(dependencies));
    const auto result = manager.Initialize();
    expect(result.error == ProfileError::MigrationFailure,
           "injected phase returns MigrationFailure");
    expect(stagingDirectories(temp.path()).empty(),
           "injected phase cleans staging directory");
    expect(!std::filesystem::exists(temp.path() / "active-profile.json"),
           "injected phase leaves bootstrap untouched");
    expect(readFile(temp.path() / "settings.cfg") == sourceBefore,
           "injected phase leaves legacy source untouched");
  }
}

void testFinalizedOrphanRecoversAfterBootstrapFailure() {
  TempDirectory temp("profile-orphan");
  auto failingDependencies = dependenciesFor();
  failingDependencies.beforeMigrationPhase = [](ProfileMigrationPhase phase,
                                                std::string &error) {
    if (phase == ProfileMigrationPhase::WriteBootstrap) {
      error = "injected bootstrap failure";
      return false;
    }
    return true;
  };
  PlayerProfileManager first(temp.path(), std::move(failingDependencies));
  const auto failed = first.Initialize();
  expect(failed.error == ProfileError::MigrationFailure,
         "bootstrap failure reports migration failure");
  expect(!std::filesystem::exists(temp.path() / "active-profile.json"),
         "failed bootstrap is absent");
  expect(std::filesystem::exists(temp.path() / "profiles" /
                                 "11111111-1111-4111-8111-111111111111" /
                                 "profile.json"),
         "finalized profile remains available for recovery");

  PlayerProfileManager recovered(
      temp.path(), dependenciesFor("22222222-2222-4222-8222-222222222222"));
  const auto recovery = recovered.Initialize();
  expect(recovery.ok(), "orphan finalized profile is recovered");
  expect(recovered.activeProfile().id == "11111111-1111-4111-8111-111111111111",
         "orphan recovery does not create a replacement UUID");
  expect(recovered.listProfiles().size() == 1,
         "orphan recovery keeps one finalized profile");
  expect(std::filesystem::exists(temp.path() / "active-profile.json"),
         "orphan recovery recreates bootstrap");
}

void testDurableFinalizePrecedesBootstrapAndRecoversAfterSyncFailure() {
  TempDirectory temp("profile-durability");
  std::vector<std::string> operations;
  auto dependencies = dependenciesFor();
  dependencies.filesystem.syncFile = [&](const std::filesystem::path &path,
                                         std::string &error) {
    operations.push_back("file:" + path.filename().string());
    return atomic_file::syncFile(path, error);
  };
  dependencies.filesystem.syncDirectory = [&](const std::filesystem::path &path,
                                              std::string &error) {
    if (path.filename().string().starts_with(".staging-")) {
      operations.emplace_back("staging-directory");
      return atomic_file::syncDirectory(path, error);
    }
    if (path == temp.path() / "profiles") {
      operations.emplace_back("profiles-directory");
      error = "injected profiles directory sync failure";
      return false;
    }
    return atomic_file::syncDirectory(path, error);
  };
  dependencies.filesystem.durableRename = [&](const std::filesystem::path &from,
                                              const std::filesystem::path &to,
                                              std::string &error) {
    operations.emplace_back("rename-profile");
    return atomic_file::renameDurably(from, to, error);
  };
  dependencies.beforeMigrationPhase = [&](ProfileMigrationPhase phase,
                                          std::string &) {
    if (phase == ProfileMigrationPhase::WriteBootstrap) {
      operations.emplace_back("bootstrap");
    }
    return true;
  };

  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  const ProfileResult failed = manager.Initialize();
  expect(failed.error == ProfileError::MigrationFailure,
         "parent sync failure reports migration failure");
  expect(!std::filesystem::exists(temp.path() / "active-profile.json"),
         "bootstrap is not written after parent sync failure");
  expect(std::ranges::find(operations, "bootstrap") == operations.end(),
         "bootstrap phase is never entered before durable finalize succeeds");

  const auto stagingSync = std::ranges::find(operations, "staging-directory");
  const auto rename = std::ranges::find(operations, "rename-profile");
  const auto parentSync = std::ranges::find(operations, "profiles-directory");
  expect(
      stagingSync != operations.end() && rename != operations.end() &&
          parentSync != operations.end() && stagingSync < rename &&
          rename < parentSync,
      "staging directory sync, rename, and profiles parent sync are ordered");
  for (const std::string_view filename :
       {"settings.json", "input.json", "scores.db", "replays.db",
        "profile.json"}) {
    const auto synced =
        std::ranges::find(operations, "file:" + std::string(filename));
    expect(synced != operations.end() && synced < stagingSync,
           std::string(filename) + " is synced before the staging directory");
  }

  const auto finalized =
      temp.path() / "profiles" / "11111111-1111-4111-8111-111111111111";
  expect(std::filesystem::exists(finalized / "profile.json"),
         "parent sync failure leaves a finalized orphan for recovery");
  PlayerProfileManager recovered(
      temp.path(), dependenciesFor("22222222-2222-4222-8222-222222222222"));
  expect(recovered.Initialize().ok(),
         "next initialization safely recovers finalized sync-failure orphan");
  expect(recovered.activeProfile().id == "11111111-1111-4111-8111-111111111111",
         "sync-failure recovery preserves the finalized profile ID");
}

void testProfilesRootSymlinkNeverEscapesApplicationRoot() {
  TempDirectory temp("profile-confinement");
  TempDirectory external("profile-external");
  const auto sentinel = external.path() / "sentinel.txt";
  writeFile(sentinel, "external-data");
  std::filesystem::create_directories(external.path() / ".staging-malicious");
  writeFile(external.path() / ".staging-malicious" / "keep.txt", "keep");

  TempDirectory applicationParent("profile-root-ancestor");
  const auto linkedApplicationRoot = applicationParent.path() / "linked-root";
  std::error_code error;
  std::filesystem::create_directory_symlink(external.path(),
                                            linkedApplicationRoot, error);
  expect(!error, "application-root symlink fixture creates");
  if (!error) {
    PlayerProfileManager linkedRoot(linkedApplicationRoot, dependenciesFor());
    expect(linkedRoot.Initialize().error == ProfileError::IoFailure,
           "Initialize rejects a symlinked application data root");
    expect(readFile(sentinel) == "external-data",
           "application-root rejection leaves external data untouched");
  }

  error.clear();
  std::filesystem::create_directory_symlink(external.path(),
                                            temp.path() / "profiles", error);
  expect(!error, "profiles-root symlink fixture creates");
  if (error) {
    return;
  }

  PlayerProfileManager blocked(temp.path(), dependenciesFor());
  expect(blocked.Initialize().error == ProfileError::IoFailure,
         "Initialize rejects a symlinked profiles root");
  expect(blocked.listProfiles().empty(),
         "listProfiles does not traverse a symlinked profiles root");
  expect(!blocked.validateProfile("11111111-1111-4111-8111-111111111111").ok(),
         "validateProfile rejects a symlinked profiles root");
  expect(readFile(sentinel) == "external-data" &&
             readFile(external.path() / ".staging-malicious" / "keep.txt") ==
                 "keep",
         "Initialize and staging cleanup leave external data untouched");

  std::filesystem::remove(temp.path() / "profiles", error);
  PlayerProfileManager live(temp.path(), dependenciesFor());
  expect(live.Initialize().ok(), "confinement CRUD fixture initializes");

  const auto hostileStaging =
      temp.path() / "profiles" / ".staging-external-target";
  std::filesystem::create_directory_symlink(external.path(), hostileStaging,
                                            error);
  expect(!error, "staging symlink fixture creates");
  PlayerProfileManager cleanupBlocked(temp.path(), dependenciesFor());
  expect(cleanupBlocked.Initialize().error == ProfileError::IoFailure,
         "initialization refuses a symlinked staging transaction");
  expect(readFile(sentinel) == "external-data",
         "staging cleanup never traverses an external symlink target");
  std::filesystem::remove(hostileStaging, error);

  const auto originalProfiles = temp.path() / "profiles-real";
  std::filesystem::rename(temp.path() / "profiles", originalProfiles, error);
  expect(!error, "real profiles directory moves aside");
  std::filesystem::create_directory_symlink(external.path(),
                                            temp.path() / "profiles", error);
  expect(!error, "CRUD profiles-root symlink fixture creates");
  if (error) {
    return;
  }

  expect(live.createProfile("Escaped").error == ProfileError::IoFailure,
         "createProfile rejects a symlinked profiles root");
  expect(live.listProfiles().empty(),
         "listProfiles remains fail-closed after profiles-root replacement");
  expect(!live.validateProfile(live.activeProfile().id).ok(),
         "validateProfile rejects profiles-root replacement");
  expect(live.deleteProfile(live.activeProfile().id).error ==
             ProfileError::IoFailure,
         "deleteProfile rejects profiles-root replacement before traversal");
  expect(readFile(sentinel) == "external-data" &&
             readFile(external.path() / ".staging-malicious" / "keep.txt") ==
                 "keep",
         "CRUD operations never mutate external symlink targets");
}

void testPartialTombstoneCleanupNeverRestoresOrExposesProfile() {
  TempDirectory temp("profile-delete-tombstone");
  std::vector<std::string> uuids = {
      "11111111-1111-4111-8111-111111111111",
      "22222222-2222-4222-8222-222222222222",
  };
  std::size_t uuidIndex = 0;
  auto dependencies = dependenciesFor();
  dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
  bool failDeletionCleanup = true;
  dependencies.filesystem.removeTree = [&](const std::filesystem::path &path,
                                           std::string &error) {
    if (failDeletionCleanup &&
        path.filename().string().starts_with(".deleting-")) {
      failDeletionCleanup = false;
      std::error_code removeError;
      std::filesystem::remove(path / "profile.json", removeError);
      error = "injected partial tombstone cleanup failure";
      return false;
    }
    return removeTree(path, error);
  };

  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(), "tombstone fixture initializes");
  const auto created = manager.createProfile("Delete Me");
  expect(created.ok() && created.profile,
         "tombstone fixture creates an inactive profile");
  if (!created.profile) {
    return;
  }
  const std::string deletedId = created.profile->id;
  const auto source = manager.pathsFor(deletedId).root;
  const auto tombstone = temp.path() / "profiles" / (".deleting-" + deletedId);
  expect(manager.deleteProfile(deletedId).error == ProfileError::IoFailure,
         "partial physical cleanup reports an I/O failure");
  expect(!std::filesystem::exists(source) && std::filesystem::exists(tombstone),
         "logical deletion is never rolled back after partial cleanup");
  expect(manager.listProfiles().size() == 1,
         "partially deleted tombstone is not exposed as a profile");

  auto blockedDependencies = dependenciesFor();
  blockedDependencies.filesystem.removeTree =
      [](const std::filesystem::path &path, std::string &error) {
        if (path.filename().string().starts_with(".deleting-")) {
          error = "injected restart tombstone cleanup failure";
          return false;
        }
        return removeTree(path, error);
      };
  PlayerProfileManager blockedRestart(temp.path(),
                                      std::move(blockedDependencies));
  expect(blockedRestart.Initialize().error == ProfileError::IoFailure,
         "restart fails safely when tombstone cleanup still cannot finish");
  expect(!std::filesystem::exists(source) && std::filesystem::exists(tombstone),
         "failed restart cleanup does not restore a partial profile");

  PlayerProfileManager recovered(temp.path(), dependenciesFor());
  expect(recovered.Initialize().ok(),
         "later restart retries and completes tombstone cleanup");
  expect(!std::filesystem::exists(tombstone) &&
             recovered.listProfiles().size() == 1,
         "successful restart removes tombstone without exposing deleted data");
}

void testFutureLegacyDatabaseVersionFailsClosedBeforeMigration() {
  for (const bool scoreDatabase : {true, false}) {
    TempDirectory temp(scoreDatabase ? "profile-future-score"
                                     : "profile-future-replay");
    std::filesystem::create_directories(temp.path() / "db");
    const auto source =
        temp.path() / "db" / (scoreDatabase ? "score.db" : "replay.db");
    Database database = openDatabase(source);
    expect(database != nullptr, "future legacy database fixture opens");
    if (!database) {
      continue;
    }
    expect(execute(database.get(), "PRAGMA user_version=999"),
           "future legacy user_version is written");
    database.reset();
    const std::string sourceBefore = readFile(source);

    PlayerProfileManager manager(temp.path(), dependenciesFor());
    expect(manager.Initialize().error == ProfileError::FutureVersion,
           "future legacy database version returns FutureVersion");
    expect(readFile(source) == sourceBefore,
           "future legacy database remains byte-for-byte untouched");
    expect(stagingDirectories(temp.path()).empty() &&
               !std::filesystem::exists(temp.path() / "active-profile.json"),
           "future legacy preflight creates no staged or bootstrap state");
  }
}

void testSupportedOlderActiveProfileWaitsForSchemaOwners() {
  TempDirectory temp("profile-supported-older-active");
  PlayerProfileManager creator(temp.path(), dependenciesFor());
  expect(creator.Initialize().ok(), "supported-older fixture initializes");
  const PlayerProfilePaths paths = creator.activePaths();
  const std::string profileId = creator.activeProfile().id;

  {
    Database scores = openDatabase(paths.scoresDb);
    Database replays = openDatabase(paths.replaysDb);
    expect(scores != nullptr && replays != nullptr,
           "supported-older marker databases open");
    if (!scores || !replays) {
      return;
    }
    expect(execute(scores.get(),
                   "CREATE TABLE migration_marker(value TEXT);"
                   "INSERT INTO migration_marker VALUES('score');"
                   "PRAGMA user_version=5"),
           "score v5 fixture and marker are written");
    expect(execute(replays.get(),
                   "CREATE TABLE migration_marker(value TEXT);"
                   "INSERT INTO migration_marker VALUES('replay');"
                   "PRAGMA user_version=3"),
           "replay v3 fixture and marker are written");
  }

  std::string versionError;
  expect(!creator.validateProfile(profileId).ok(),
         "strict public validation rejects a profile awaiting migration");
  expect(sqliteDatabaseUserVersion(paths.scoresDb, versionError) == 5 &&
             sqliteDatabaseUserVersion(paths.replaysDb, versionError) == 3,
         "strict validation does not mutate supported older databases");

  PlayerProfileManager reopened(
      temp.path(), dependenciesFor("22222222-2222-4222-8222-222222222222"));
  const ProfileResult initialized = reopened.Initialize();
  expect(initialized.ok() && reopened.activeProfile().id == profileId,
         "startup admits the active v5/v3 profile for its schema owners: " +
             initialized.message);
  expect(reopened.listProfiles().size() == 1,
         "profile discovery keeps a supported older profile selectable");
  expect(sqliteDatabaseUserVersion(paths.scoresDb, versionError) == 5 &&
             sqliteDatabaseUserVersion(paths.replaysDb, versionError) == 3,
         "profile startup preflight remains read-only");

  ScoreDBHelper score(paths.scoresDb);
  ReplayDBHelper replay(paths.replaysDb);
  expect(score.EnsureSchema() && replay.EnsureSchema(),
         "normal database owners migrate the admitted profile");
  score.Shutdown();
  replay.Shutdown();
  expect(sqliteDatabaseUserVersion(paths.scoresDb, versionError) ==
                 ScoreDBHelper::kCurrentSchemaVersion &&
             sqliteDatabaseUserVersion(paths.replaysDb, versionError) ==
                 ReplayDBHelper::kCurrentSchemaVersion,
         "database owners advance v5/v3 to the current schemas");
  expect(rowCount(paths.scoresDb, "migration_marker") == 1 &&
             rowCount(paths.replaysDb, "migration_marker") == 1,
         "database migrations preserve profile rows");
  expect(reopened.validateProfile(profileId).ok(),
         "strict validation succeeds after owned migration");
}

void testProfileCrudConstraintsAndDataIsolation() {
  TempDirectory temp("profile-crud");
  std::vector<std::string> uuids = {
      "11111111-1111-4111-8111-111111111111",
      "22222222-2222-4222-8222-222222222222",
      "33333333-3333-4333-8333-333333333333",
  };
  std::size_t uuidIndex = 0;
  auto dependencies = dependenciesFor();
  dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(), "CRUD manager initializes");
  const std::string firstId = manager.activeProfile().id;

  expect(manager.createProfile("   ").error == ProfileError::InvalidName,
         "blank profile names are rejected");
  expect(manager.createProfile(std::string("bad\0name", 8)).error ==
             ProfileError::InvalidName,
         "control characters in profile names are rejected");
  expect(manager.createProfile(std::string("bad\xC2\x85name", 9)).error ==
             ProfileError::InvalidName,
         "Unicode C1 controls in profile names are rejected");

  const auto created = manager.createProfile("Second Player");
  expect(created.ok() && created.profile.has_value(),
         "profile creation succeeds");
  const std::string secondId = created.profile ? created.profile->id : "";
  expect(std::filesystem::exists(manager.pathsFor(secondId).settingsJson) &&
             std::filesystem::exists(manager.pathsFor(secondId).inputJson) &&
             std::filesystem::exists(manager.pathsFor(secondId).scoresDb) &&
             std::filesystem::exists(manager.pathsFor(secondId).replaysDb),
         "created profile owns all isolated data files");

  const auto duplicate = manager.duplicateProfile(firstId, "First Copy");
  expect(duplicate.ok() && duplicate.profile.has_value(),
         "profile duplication succeeds");
  const std::string copyId = duplicate.profile ? duplicate.profile->id : "";
  expect(readFile(manager.pathsFor(copyId).settingsJson) ==
             readFile(manager.pathsFor(firstId).settingsJson),
         "duplicate preserves settings bytes");
  expect(rowCount(manager.pathsFor(copyId).scoresDb, "scores") ==
             rowCount(manager.pathsFor(firstId).scoresDb, "scores"),
         "duplicate snapshots score data");

  const auto renamed = manager.renameProfile(copyId, "Renamed Copy");
  expect(renamed.ok() && renamed.profile &&
             renamed.profile->displayName == "Renamed Copy",
         "profile rename persists new display name");
  expect(manager.deleteProfile(firstId).error ==
             ProfileError::ActiveProfileDeletion,
         "active profile cannot be deleted while another profile exists");

  expect(manager.commitActiveProfile(secondId).ok(),
         "active profile commit succeeds");
  expect(manager.activeProfile().id == secondId,
         "active profile changes only after commit");
  expect(manager.deleteProfile(copyId).ok(), "inactive copy can be deleted");
  expect(manager.deleteProfile(firstId).ok(),
         "former active profile can be deleted after switching");
  expect(manager.deleteProfile(secondId).error ==
             ProfileError::LastProfileDeletion,
         "the last profile cannot be deleted");
}

void testFutureVersionsFailClosed() {
  TempDirectory temp("profile-future");
  PlayerProfileManager manager(temp.path(), dependenciesFor());
  expect(manager.Initialize().ok(), "future-version test initializes");
  const auto bootstrap = temp.path() / "active-profile.json";
  const std::string bootstrapBefore = readFile(bootstrap);
  writeFile(manager.activePaths().profileJson,
            "{\"schemaVersion\":99,\"id\":\"" + manager.activeProfile().id +
                "\",\"displayName\":\"Future\",\"createdAt\":\"x\","
                "\"lastUsedAt\":\"x\"}\n");

  PlayerProfileManager reopened(temp.path(), dependenciesFor());
  const auto result = reopened.Initialize();
  expect(result.error == ProfileError::FutureVersion,
         "future profile version fails closed");
  expect(readFile(bootstrap) == bootstrapBefore,
         "future profile version leaves bootstrap untouched");

  writeFile(bootstrap, "{\"schemaVersion\":99,\"activeProfileId\":\"" +
                           manager.activeProfile().id + "\"}\n");
  const std::string futureBootstrap = readFile(bootstrap);
  PlayerProfileManager futureBootstrapManager(temp.path(), dependenciesFor());
  expect(futureBootstrapManager.Initialize().error ==
             ProfileError::FutureVersion,
         "future bootstrap version fails closed");
  expect(readFile(bootstrap) == futureBootstrap,
         "future bootstrap remains byte-for-byte unchanged");

  TempDirectory orphanTemp("profile-future-orphan");
  PlayerProfileManager orphanCreator(orphanTemp.path(), dependenciesFor());
  expect(orphanCreator.Initialize().ok(), "future orphan fixture initializes");
  const auto orphanProfile = orphanCreator.activePaths().profileJson;
  const std::string orphanId = orphanCreator.activeProfile().id;
  std::error_code removeError;
  std::filesystem::remove(orphanTemp.path() / "active-profile.json",
                          removeError);
  expect(!removeError, "future orphan bootstrap is removed for test setup");
  writeFile(orphanProfile,
            "{\"schemaVersion\":99,\"id\":\"" + orphanId +
                "\",\"displayName\":\"Future\",\"createdAt\":\"x\","
                "\"lastUsedAt\":\"x\"}\n");
  PlayerProfileManager orphanReopen(
      orphanTemp.path(),
      dependenciesFor("22222222-2222-4222-8222-222222222222"));
  expect(orphanReopen.Initialize().error == ProfileError::FutureVersion,
         "future orphan profile blocks replacement migration");
  expect(!std::filesystem::exists(orphanTemp.path() / "active-profile.json"),
         "future orphan profile does not create a replacement bootstrap");
}
} // namespace

int main() {
  testSqliteSnapshotIncludesWalAndValidatesIdentifiers();
  testFirstRunMigrationIsLosslessAndIdempotent();
  testEveryPreFinalizeFailureCleansStaging();
  testFinalizedOrphanRecoversAfterBootstrapFailure();
  testDurableFinalizePrecedesBootstrapAndRecoversAfterSyncFailure();
  testProfilesRootSymlinkNeverEscapesApplicationRoot();
  testPartialTombstoneCleanupNeverRestoresOrExposesProfile();
  testFutureLegacyDatabaseVersionFailsClosedBeforeMigration();
  testSupportedOlderActiveProfileWaitsForSchemaOwners();
  testProfileCrudConstraintsAndDataIsolation();
  testFutureVersionsFailClosed();

  if (failures != 0) {
    std::cerr << failures << " player profile test(s) failed.\n";
    return 1;
  }
  std::cout << "Player profile manager tests passed.\n";
  return 0;
}
