#include "../src/AppSettingsStore.h"
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
  testProfileCrudConstraintsAndDataIsolation();
  testFutureVersionsFailClosed();

  if (failures != 0) {
    std::cerr << failures << " player profile test(s) failed.\n";
    return 1;
  }
  std::cout << "Player profile manager tests passed.\n";
  return 0;
}
