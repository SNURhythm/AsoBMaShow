#include "../src/AppSettingsStore.h"
#include "../src/AtomicFile.h"
#include "../src/PlayerProfileManager.h"
#include "../src/ProfileDatabaseTools.h"
#include "../src/ProfileDatabaseActivity.h"
#include "../src/Utils.h"
#include "../src/repositories/ReplayRepository.h"
#include "../src/repositories/ScoreRepository.h"
#include "../src/input/InputProfileStore.h"
#include "../src/sqlite3.h"
#include "../yoga/lib/nlohmann/json.hpp"
#include "support/ReplaySchema10Fixture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

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

class ScopedDocumentsRoot {
public:
  explicit ScopedDocumentsRoot(const std::filesystem::path &root)
      : previousGameName_(Utils::GameName) {
    Utils::GameName = root.string();
  }

  ~ScopedDocumentsRoot() { Utils::GameName = previousGameName_; }

private:
  std::string previousGameName_;
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

std::string validPracticePresetJson(std::string_view hash) {
  const nlohmann::json configuration = {
      {"chartSha256", hash},
      {"startMicros", 0},
      {"endMicros", 1'000'000},
      {"loop", false},
      {"countInBeats", 4},
      {"gaugeType", "normal"},
      {"gaugeAutoShift", false},
      {"startingGaugePercent", nullptr},
      {"judge", {{"kind", 0}, {"scalePercent", 100}}},
      {"playback", {{"percent", 100}, {"mode", 0}}},
  };
  return nlohmann::json{{"schemaVersion", 1},
                        {"chartSha256", hash},
                        {"lastUsed", configuration},
                        {"named", nlohmann::json::array()}}
             .dump() +
         "\n";
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

void seedReplayMigrationChartMetadata(const std::filesystem::path &path) {
  std::filesystem::create_directories(path.parent_path());
  Database database = openDatabase(path);
  expect(database != nullptr, "replay migration chart database opens");
  if (!database) {
    return;
  }
  expect(
      execute(
          database.get(),
          "CREATE TABLE chart_meta(path TEXT PRIMARY KEY,md5 TEXT NOT NULL,"
          "sha256 TEXT NOT NULL,keys INTEGER,ln_mode INTEGER,"
          "total_long_notes INTEGER,total_backspin_notes INTEGER,"
          "total_notes INTEGER);"
          "INSERT INTO chart_meta(path,md5,sha256,keys,ln_mode,"
          "total_long_notes,total_backspin_notes,total_notes) VALUES("
          "'chart.bms','0123456789abcdef0123456789abcdef',"
          "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
          "7,0,0,0,1)"),
      "replay migration chart metadata is authoritative");
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
  ScoreRepository scoreHelper(scorePath);
  expect(scoreHelper.EnsureSchema(), "legacy score schema initializes");

  LegacyData result{openDatabase(scorePath), openDatabase(replayPath)};
  expect(result.scoreConnection != nullptr, "legacy score database opens");
  expect(result.replayConnection != nullptr, "legacy replay database opens");
  if (!result.scoreConnection || !result.replayConnection) {
    return result;
  }
  try {
    replay_schema10_fixture::createExactSchema(result.replayConnection.get());
  } catch (const std::exception &exception) {
    expect(false, "legacy replay schema initializes: " +
                      std::string(exception.what()));
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
  try {
    constexpr std::string_view replayAttempt =
        "123e4567-e89b-42d3-a456-426614174001";
    const std::string replayProvenance =
        serializeScoreProvenance(ScoreProvenance::Legacy());
    replay_schema10_fixture::insertSimpleChart(
        result.replayConnection.get(), 1,
        "0123456789abcdef0123456789abcdef",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "2026-07-10 12:34:56", 2, replayProvenance, replayAttempt, 0);
    replay_schema10_fixture::insertSimplePendingChart(
        result.replayConnection.get(), 1,
        "0123456789abcdef0123456789abcdef",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "2026-07-10 12:34:56", 2, replayProvenance, replayAttempt, 0);
  } catch (const std::exception &exception) {
    expect(false, "WAL-backed replay rows insert: " +
                      std::string(exception.what()));
  }
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

std::int64_t matchingRowCount(const std::filesystem::path &database,
                              std::string_view query) {
  Database connection = openDatabase(database);
  expect(connection != nullptr, "matching row count database opens");
  if (!connection) {
    return -1;
  }
  sqlite3_stmt *raw = nullptr;
  const std::string sql(query);
  if (sqlite3_prepare_v2(connection.get(), sql.c_str(), -1, &raw, nullptr) !=
      SQLITE_OK) {
    expect(false, "matching row count query prepares");
    return -1;
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      raw, sqlite3_finalize);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    expect(false, "matching row count query returns a row");
    return -1;
  }
  return sqlite3_column_int64(statement.get(), 0);
}

void seedIrOperationalState(const std::filesystem::path &path,
                            std::string_view label) {
  ReplayRepository repository(path);
  expect(repository.EnsureSchema(),
         std::string(label) + " IR outbox schema initializes");
  repository.Shutdown();
  Database database = openDatabase(path);
  expect(database != nullptr,
         std::string(label) + " IR outbox database opens");
  if (!database) {
    return;
  }
  expect(execute(
             database.get(),
             "INSERT INTO ir_outbox(provider_id,attempt_id,chart_md5,"
             "chart_sha256,payload_json,ruleset_id,ruleset_revision,"
             "validation_fingerprint,state,local_result_ready,"
             "next_attempt_at_ms,remote_job_id,remote_origin,created_at_ms,"
             "updated_at_ms,completed_at_ms) VALUES"
             "('tachi','10000000-0000-4000-8000-000000000001',NULL,"
             "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
             "'{\"score\":1}','test-rules',1,lower(hex(zeroblob(32))),"
             "0,1,NULL,NULL,NULL,1000,1000,NULL),"
             "('archive_readonly','10000000-0000-4000-8000-000000000002',NULL,"
             "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',"
             "'{\"score\":2}','test-rules',1,lower(hex(zeroblob(32))),"
             "2,1,3000,'job-2','https://example.invalid',"
             "2000,2000,NULL),"
             "('tachi_backup','10000000-0000-4000-8000-000000000003',NULL,"
             "'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',"
             "'{\"score\":3}','test-rules',1,lower(hex(zeroblob(32))),"
             "5,1,NULL,NULL,NULL,3000,3000,3000)"),
         std::string(label) + " pending, deferred, and succeeded IR rows seed");
  expect(
      execute(
          database.get(),
          "INSERT INTO chart_results(attempt_id,chart_path,chart_md5,"
          "chart_sha256,chart_title,chart_artist,key_mode,long_note_mode,score,"
          "max_score,max_combo,combo_break,p_great,great,good,bad,poor,k_poor,"
          "fast,slow,final_gauge,clear_type,adopted_gauge_type,"
          "gauge_history_json,"
          "judgement_timing_json,provenance_json,result_fingerprint,"
          "played_at_unix_ms) VALUES("
          "'10000000-0000-4000-8000-000000000004','remote.bms',"
          "'dddddddddddddddddddddddddddddddd',"
          "'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',"
          "'Remote Song','Remote Artist',7,0,1,2,1,0,0,1,0,0,0,0,0,0,0.0,0,"
          "2,'[]',NULL,'{\"ruleset\":{\"version\":0},\"eligibility\":2,"
          "\"source\":{\"kind\":\"legacy\"}}',lower(hex(zeroblob(32))),"
          "4000);"
          "INSERT INTO ir_submission_receipts(provider_id,server_origin,"
          "result_id,attempt_id,chart_sha256,confirmation_source,"
          "confirmed_at_ms) VALUES('tachi','https://boku.tachi.ac',"
          "last_insert_rowid(),'10000000-0000-4000-8000-000000000004',"
          "'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',"
          "1,4000);"
          "INSERT INTO ir_remote_scores(provider_id,server_origin,"
          "remote_score_id,remote_user_id,game,remote_chart_id,chart_md5,"
          "chart_sha256,title,artist,note_count,score,lamp_rank,service,"
          "time_added_ms,sync_generation) VALUES('tachi',"
          "'https://boku.tachi.ac','remote-score',42,'bms-7k','remote-chart',"
          "'dddddddddddddddddddddddddddddddd',"
          "'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',"
          "'Remote Song','Remote Artist',1,1,0,'Bokutachi',4000,1)"),
      std::string(label) + " account-scoped IR evidence and mirror seed");
}

void seedImportedIrScore(const std::filesystem::path &path,
                         std::string_view label) {
  Database database = openDatabase(path);
  expect(database != nullptr,
         std::string(label) + " imported IR score database opens");
  if (!database) {
    return;
  }
  expect(
      execute(
          database.get(),
          "INSERT INTO scores(chart_sha256,score,max_score,max_combo,"
          "combo_break,pgreat,great,good,bad,poor,kpoor,fast,slow,"
          "final_gauge,clear_type,score_source,source_provider_id,"
          "source_server_origin,source_remote_score_id,"
          "source_sync_generation) VALUES("
          "'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',"
          "1,2,1,0,0,1,0,0,0,0,0,0,0.0,0,1,'tachi',"
          "'https://boku.tachi.ac','remote-score',1)"),
      std::string(label) + " imported IR score projection seeds");
}

constexpr std::string_view kReferencedReplayFilename =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.brd";

void seedReferencedReplay(const PlayerProfilePaths &paths,
                          std::string_view label) {
  Database database = openDatabase(paths.replaysDb);
  expect(database != nullptr, std::string(label) + " replay database opens");
  if (!database) {
    return;
  }
  expect(
      execute(
          database.get(),
          "INSERT INTO chart_results(attempt_id,chart_path,chart_md5,"
          "chart_sha256,chart_title,chart_artist,key_mode,long_note_mode,score,"
          "max_score,max_combo,combo_break,p_great,great,good,bad,poor,k_poor,"
          "fast,slow,final_gauge,clear_type,adopted_gauge_type,"
          "gauge_history_json,judgement_timing_json,provenance_json,"
          "result_fingerprint,played_at_unix_ms) VALUES("
          "'10000000-0000-4000-8000-000000000005','owned.bms',"
          "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',"
          "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
          "'Owned Replay','Artist',7,0,1,2,1,0,0,1,0,0,0,0,0,0,0.0,0,2,'[]',"
          "NULL,'{\"ruleset\":{\"version\":0},\"eligibility\":2,"
          "\"source\":{\"kind\":\"legacy\"}}',lower(hex(zeroblob(32))),"
          "5000);"
          "INSERT INTO replay_files(chart_result_id,course_result_id,stem,"
          "history_index,relative_path,content_sha256,compressed_size,"
          "codec_version) VALUES(last_insert_rowid(),NULL,"
          "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
          "0,'replay/"
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
          "brd',"
          "'32e5fa06f7003f7aff6ffe68451a942cad17c55c64d6c671ea2cc2a1181ab987',"
          "19,2);"
          "INSERT INTO replay_stem_sequences(stem,last_history_index) VALUES("
          "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
          "0)"),
      std::string(label) + " replay result and file reference seed");
}

bool directoryContains(const std::filesystem::path &root,
                       std::string_view needle) {
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->is_regular_file(error) && !error &&
        readFile(iterator->path()).find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
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

void seedSupportedOlderProfile(const PlayerProfilePaths &paths,
                               std::string_view label) {
  Database scores = openDatabase(paths.scoresDb);
  expect(scores != nullptr, std::string(label) + " score marker database opens");
  if (!scores) {
    return;
  }
  expect(execute(scores.get(),
                 "CREATE TABLE validation_policy_marker(value TEXT);"
                 "INSERT INTO validation_policy_marker VALUES('score')"),
         std::string(label) + " score marker is written");
  scores.reset();
  setDatabaseVersion(paths.scoresDb, 5, std::string(label) + " score");

  std::error_code removeError;
  std::filesystem::remove(paths.replaysDb, removeError);
  expect(!removeError, std::string(label) + " current replay database removes");
  if (removeError) {
    return;
  }
  Database replays = openDatabase(paths.replaysDb);
  expect(replays != nullptr,
         std::string(label) + " replay marker database opens");
  if (!replays) {
    return;
  }
  try {
    replay_schema10_fixture::createExactSchema(replays.get());
    const std::string provenance =
        serializeScoreProvenance(ScoreProvenance::Legacy());
    replay_schema10_fixture::insertSimpleChart(
        replays.get(), 1, "0123456789abcdef0123456789abcdef",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "2026-07-10 12:34:56", 2, provenance);
    replay_schema10_fixture::execute(
        replays.get(),
        "INSERT INTO course_replays(id,course_id,course_key,course_name,"
        "course_group_name,constraint_json,gauge_type,gauge_profile,"
        "gauge_auto_shift,ln_mode,requested_play_option,assist_option,"
        "final_score,max_combo,final_gauge,clear_type,completed_charts,"
        "total_charts,created_at,ruleset_version,eligibility,provenance_json) "
        "VALUES(7,7,'','Course','Group','[]',0,0,0,0,'NORMAL','OFF',2,1,"
        "75.0,300,1,1,'2026-07-10 12:34:56',0,2," +
            replay_schema10_fixture::quote(provenance) +
            ");"
            "INSERT INTO course_replay_stages(id,course_replay_id,stage_index,"
            "replay_id,rest_micros_after_stage) VALUES(8,7,0,1,0)");
  } catch (const std::exception &exception) {
    expect(false, std::string(label) + " replay v10 schema creates: " +
                      exception.what());
    return;
  }
  expect(execute(replays.get(),
                 "CREATE TABLE validation_policy_marker(value TEXT);"
                 "INSERT INTO validation_policy_marker VALUES('replay')"),
         std::string(label) + " replay marker is written");
}

void expectSupportedOlderPayload(const PlayerProfilePaths &paths,
                                 std::string_view label) {
  std::string error;
  expect(sqliteDatabaseUserVersion(paths.scoresDb, error) == 5,
         std::string(label) + " keeps score schema version 5: " + error);
  error.clear();
  expect(sqliteDatabaseUserVersion(paths.replaysDb, error) == 10,
         std::string(label) + " keeps replay schema version 10: " + error);
  expect(rowCount(paths.scoresDb, "validation_policy_marker") == 1,
         std::string(label) + " keeps the score marker row");
  expect(rowCount(paths.replaysDb, "validation_policy_marker") == 1,
         std::string(label) + " keeps the replay marker row");
  expect(rowCount(paths.replaysDb, "replays") == 1 &&
             rowCount(paths.replaysDb, "course_replays") == 1 &&
             rowCount(paths.replaysDb, "course_replay_stages") == 1,
         std::string(label) + " keeps the course-only stage replay");
}

std::vector<std::filesystem::path>
profileTransactionArtifacts(const std::filesystem::path &root) {
  std::vector<std::filesystem::path> result;
  const auto profiles = root / "profiles";
  std::error_code error;
  if (!std::filesystem::exists(profiles, error)) {
    expect(!error, "profile transaction artifact root inspection succeeds");
    return result;
  }
  std::filesystem::directory_iterator iterator(profiles, error);
  expect(!error, "profile transaction artifact enumeration begins");
  if (error) {
    return result;
  }
  for (const auto &entry : iterator) {
    const std::string filename = entry.path().filename().string();
    if (filename.starts_with(".staging-") ||
        filename.starts_with(".deleting-") ||
        filename.starts_with(".backup-")) {
      result.push_back(entry.path());
    }
  }
  std::ranges::sort(result);
  return result;
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
  ScopedDocumentsRoot documents(temp.path());
  seedReplayMigrationChartMetadata(Utils::GetDocumentsPath("db") / "chart.db");
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
             paths.irCredentialsJson == paths.root / "ir-credentials.json" &&
             paths.bokutachiCacheJson ==
                 paths.root / "bokutachi-cache.json" &&
             paths.scoresDb == paths.root / "scores.db" &&
             paths.replaysDb == paths.root / "replays.db" &&
             paths.practiceDirectory == paths.root / "practice" &&
             paths.replayDirectory == paths.root / "replay" &&
             std::filesystem::is_directory(paths.practiceDirectory) &&
             std::filesystem::is_directory(paths.replayDirectory),
         "all profile paths follow fixed layout");
  expect(!std::filesystem::exists(paths.irCredentialsJson),
         "migration starts without device-local IR credentials");
  expect(!std::filesystem::exists(paths.bokutachiCacheJson),
         "migration starts without a Bokutachi lookup cache");

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
  expect(rowCount(paths.replaysDb, "chart_results") == 1,
         "replay result survives migration");
  expect(rowCount(paths.replaysDb, "replay_files") == 1,
         "replay events migrate into one file reference");
  expect(std::filesystem::exists(
             paths.root / "replay" /
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.brd"),
         "legacy replay events migrate into a Beatoraja-layout file");

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

void testFailedCreateCommitSyncLeavesNoVisibleProfile() {
  TempDirectory temp("profile-create-parent-sync");
  const std::string activeId = "11111111-1111-4111-8111-111111111111";
  const std::string createdId = "22222222-2222-4222-8222-222222222222";
  std::vector<std::string> uuids{activeId, createdId};
  std::size_t uuidIndex = 0;
  bool inject = false;
  bool finalRenameCompleted = false;
  bool commitSyncFailed = false;
  const auto profiles = temp.path() / "profiles";
  const auto staging = profiles / (".staging-" + createdId);
  const auto destination = profiles / createdId;

  auto dependencies = dependenciesFor();
  dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
  dependencies.filesystem.durableRename = [&](const std::filesystem::path &from,
                                              const std::filesystem::path &to,
                                              std::string &error) {
    const bool renamed = atomic_file::renameDurably(from, to, error);
    if (inject && renamed && from == staging && to == destination) {
      finalRenameCompleted = true;
    }
    return renamed;
  };
  dependencies.filesystem.syncDirectory = [&](const std::filesystem::path &path,
                                              std::string &error) {
    if (inject && finalRenameCompleted && !commitSyncFailed &&
        path == profiles) {
      commitSyncFailed = true;
      error = "injected create commit sync failure";
      return false;
    }
    return atomic_file::syncDirectory(path, error);
  };

  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(), "create parent-sync fixture initializes");
  const std::size_t countBefore = manager.listProfiles().size();

  inject = true;
  const ProfileResult result = manager.createProfile("Create Sync Failure");
  inject = false;
  expect(result.error == ProfileError::IoFailure,
         "failed commit sync reports create failure");
  expect(!std::filesystem::exists(manager.pathsFor(createdId).root),
         "failed create commit leaves no visible destination");
  expect(stagingDirectories(temp.path()).empty(),
         "successful create rollback cleans hidden staging");
  expect(manager.listProfiles().size() == countBefore,
         "reported create failure does not change the visible catalog");

  PlayerProfileManager recovered(
      temp.path(), dependenciesFor("33333333-3333-4333-8333-333333333333"));
  expect(recovered.Initialize().ok(),
         "create parent-sync failure reinitializes cleanly");
  expect(!std::filesystem::exists(recovered.pathsFor(createdId).root) &&
             recovered.listProfiles().size() == countBefore,
         "reinitialization preserves failed create outcome");
}

void testFailedDuplicateCommitSyncLeavesNoVisibleProfile() {
  TempDirectory temp("profile-duplicate-parent-sync");
  const std::string activeId = "11111111-1111-4111-8111-111111111111";
  const std::string duplicateId = "22222222-2222-4222-8222-222222222222";
  std::vector<std::string> uuids{activeId, duplicateId};
  std::size_t uuidIndex = 0;
  bool inject = false;
  bool finalRenameCompleted = false;
  bool commitSyncFailed = false;
  const auto profiles = temp.path() / "profiles";
  const auto staging = profiles / (".staging-" + duplicateId);
  const auto destination = profiles / duplicateId;

  auto dependencies = dependenciesFor();
  dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
  dependencies.filesystem.durableRename = [&](const std::filesystem::path &from,
                                              const std::filesystem::path &to,
                                              std::string &error) {
    const bool renamed = atomic_file::renameDurably(from, to, error);
    if (inject && renamed && from == staging && to == destination) {
      finalRenameCompleted = true;
    }
    return renamed;
  };
  dependencies.filesystem.syncDirectory = [&](const std::filesystem::path &path,
                                              std::string &error) {
    if (inject && finalRenameCompleted && !commitSyncFailed &&
        path == profiles) {
      commitSyncFailed = true;
      error = "injected duplicate commit sync failure";
      return false;
    }
    return atomic_file::syncDirectory(path, error);
  };

  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(),
         "duplicate parent-sync fixture initializes");
  const std::string sourceId = manager.activeProfile().id;
  const std::size_t countBefore = manager.listProfiles().size();

  inject = true;
  const ProfileResult result =
      manager.duplicateProfile(sourceId, "Duplicate Sync Failure");
  inject = false;
  expect(result.error == ProfileError::IoFailure,
         "failed commit sync reports duplicate failure");
  expect(!std::filesystem::exists(manager.pathsFor(duplicateId).root),
         "failed duplicate commit leaves no visible destination");
  expect(stagingDirectories(temp.path()).empty(),
         "successful duplicate rollback cleans hidden staging");
  expect(manager.listProfiles().size() == countBefore,
         "reported duplicate failure does not change the visible catalog");

  PlayerProfileManager recovered(
      temp.path(), dependenciesFor("33333333-3333-4333-8333-333333333333"));
  expect(recovered.Initialize().ok(),
         "duplicate parent-sync failure reinitializes cleanly");
  expect(!std::filesystem::exists(recovered.pathsFor(duplicateId).root) &&
             recovered.listProfiles().size() == countBefore,
         "reinitialization preserves failed duplicate outcome");
}

enum class NewProfileOperation { Create, Duplicate };

enum class NewProfileFault {
  FinalRenameFails,
  FinalRenameMovesThenFails,
  CommitSyncRollbackSucceeds,
  CommitSyncRollbackUnavailable,
  RollbackMovesThenFails,
  RollbackCleanupFails,
  RollbackParentSyncFails,
  RollbackUnavailableRetrySyncFails,
  AmbiguousFinalLeavesBoth,
  AmbiguousFinalRetrySyncFails,
  AmbiguousFinalLeavesInvalidDestination,
  AmbiguousFinalLeavesNeither,
  RollbackCleanupSyncFails,
  UnsafeDestinationInspection,
};

struct NewProfileFaultCase {
  NewProfileFault fault;
  std::string_view name;
  bool expectedSuccess;
  bool expectedWarning = false;
  bool expectedStagingEvidence = false;
  bool safelyInspected = true;
};

constexpr std::array kNewProfileFaultCases{
    NewProfileFaultCase{NewProfileFault::FinalRenameFails,
                        "point 0 final rename fails", false},
    NewProfileFaultCase{NewProfileFault::FinalRenameMovesThenFails,
                        "point 1 final rename moves then fails", true},
    NewProfileFaultCase{NewProfileFault::CommitSyncRollbackSucceeds,
                        "point 2 commit sync rollback succeeds", false},
    NewProfileFaultCase{NewProfileFault::CommitSyncRollbackUnavailable,
                        "point 3 commit sync rollback unavailable", true},
    NewProfileFaultCase{NewProfileFault::RollbackMovesThenFails,
                        "point 4 rollback moves then fails", false},
    NewProfileFaultCase{NewProfileFault::RollbackCleanupFails,
                        "point 5 rollback staging cleanup fails", false, false,
                        true},
    NewProfileFaultCase{NewProfileFault::RollbackParentSyncFails,
                        "point 6 rollback parent sync fails", false, false,
                        true},
    NewProfileFaultCase{NewProfileFault::RollbackUnavailableRetrySyncFails,
                        "point 7 rollback unavailable retry sync fails", true,
                        true},
    NewProfileFaultCase{NewProfileFault::AmbiguousFinalLeavesBoth,
                        "ambiguous final rename leaves destination and staging",
                        true},
    NewProfileFaultCase{NewProfileFault::AmbiguousFinalRetrySyncFails,
                        "ambiguous final rename retry sync fails", true, true},
    NewProfileFaultCase{NewProfileFault::AmbiguousFinalLeavesInvalidDestination,
                        "ambiguous final rename leaves invalid destination",
                        false, false, true},
    NewProfileFaultCase{NewProfileFault::AmbiguousFinalLeavesNeither,
                        "ambiguous final rename leaves neither path", false},
    NewProfileFaultCase{NewProfileFault::RollbackCleanupSyncFails,
                        "rollback staging cleanup sync fails", false},
    NewProfileFaultCase{NewProfileFault::UnsafeDestinationInspection,
                        "unsafe destination inspection", false, false, true,
                        false},
};

std::string faultCaseLabel(NewProfileOperation operation,
                           std::string_view name) {
  return std::string(operation == NewProfileOperation::Create ? "create "
                                                              : "duplicate ") +
         std::string(name);
}

bool hasProfile(const PlayerProfileManager &manager, std::string_view id) {
  const auto catalog = manager.listProfiles();
  return std::ranges::find(catalog, id, &PlayerProfile::id) != catalog.end();
}

void seedDuplicateFaultPayload(const PlayerProfilePaths &source) {
  auto loaded = AppSettingsStore::Load(source.settingsJson);
  expect(loaded.status == AppSettingsLoadStatus::Loaded,
         "duplicate fault source settings load");
  if (loaded.status == AppSettingsLoadStatus::Loaded) {
    loaded.settings.audioOffsetMs = 137;
    loaded.settings.selectedPlayOption = "R-RANDOM";
    std::string error;
    expect(AppSettingsStore::Save(source.settingsJson, loaded.settings, error),
           "duplicate fault source settings save: " + error);
  }

  Database scores = openDatabase(source.scoresDb);
  expect(scores != nullptr, "duplicate fault source database opens");
  if (scores) {
    expect(execute(scores.get(),
                   "CREATE TABLE profile_fault_marker(value TEXT);"
                   "INSERT INTO profile_fault_marker VALUES('source-data')"),
           "duplicate fault source database marker seeds");
  }
}

bool isCommitSyncFault(NewProfileFault fault) {
  switch (fault) {
  case NewProfileFault::CommitSyncRollbackSucceeds:
  case NewProfileFault::CommitSyncRollbackUnavailable:
  case NewProfileFault::RollbackMovesThenFails:
  case NewProfileFault::RollbackCleanupFails:
  case NewProfileFault::RollbackParentSyncFails:
  case NewProfileFault::RollbackUnavailableRetrySyncFails:
  case NewProfileFault::RollbackCleanupSyncFails:
    return true;
  default:
    return false;
  }
}

bool expectsRollbackAttempt(NewProfileFault fault) {
  return isCommitSyncFault(fault);
}

bool copyTreeForFault(const std::filesystem::path &source,
                      const std::filesystem::path &destination,
                      std::string &errorMessage) {
  std::error_code error;
  std::filesystem::copy(source, destination,
                        std::filesystem::copy_options::recursive, error);
  if (error) {
    errorMessage = "unable to copy fault tree: " + error.message();
    return false;
  }
  return true;
}

void runNewProfileFaultCase(NewProfileOperation operation,
                            const NewProfileFaultCase &testCase) {
  const std::string label = faultCaseLabel(operation, testCase.name);
  TempDirectory temp(operation == NewProfileOperation::Create
                         ? "profile-create-fault"
                         : "profile-duplicate-fault");
  TempDirectory external("profile-new-fault-external");
  const std::string activeId = "11111111-1111-4111-8111-111111111111";
  const std::string generatedId = "22222222-2222-4222-8222-222222222222";
  std::vector<std::string> uuids{activeId, generatedId};
  std::size_t uuidIndex = 0;
  bool inject = false;
  int finalRenameCalls = 0;
  int rollbackRenameCalls = 0;
  int profilesSyncCalls = 0;
  bool rollbackCleanupFailureInjected = false;
  bool rollbackCleanupSyncAttempted = false;
  bool destinationRemovalAttempted = false;
  const auto profiles = temp.path() / "profiles";
  const auto staging = profiles / (".staging-" + generatedId);
  const auto destination = profiles / generatedId;
  const auto externalSentinel = external.path() / "sentinel.txt";
  writeFile(externalSentinel, "external-data");

  auto dependencies = dependenciesFor();
  dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
  dependencies.filesystem.durableRename = [&](const std::filesystem::path &from,
                                              const std::filesystem::path &to,
                                              std::string &error) {
    if (!inject) {
      return atomic_file::renameDurably(from, to, error);
    }
    if (from == staging && to == destination) {
      ++finalRenameCalls;
      switch (testCase.fault) {
      case NewProfileFault::FinalRenameFails:
        error = "injected final rename failure";
        return false;
      case NewProfileFault::FinalRenameMovesThenFails:
      case NewProfileFault::AmbiguousFinalRetrySyncFails:
        if (!atomic_file::renameDurably(from, to, error)) {
          return false;
        }
        error = "injected ambiguous final rename";
        return false;
      case NewProfileFault::AmbiguousFinalLeavesBoth:
        if (!copyTreeForFault(from, to, error)) {
          return false;
        }
        error = "injected ambiguous final rename with both paths";
        return false;
      case NewProfileFault::AmbiguousFinalLeavesInvalidDestination:
        if (!copyTreeForFault(from, to, error)) {
          return false;
        }
        {
          std::error_code removeError;
          std::filesystem::remove(destination / "settings.json", removeError);
          if (removeError) {
            error =
                "unable to invalidate destination: " + removeError.message();
            return false;
          }
        }
        error = "injected ambiguous invalid destination";
        return false;
      case NewProfileFault::AmbiguousFinalLeavesNeither: {
        std::error_code removeError;
        std::filesystem::remove_all(staging, removeError);
        if (removeError) {
          error =
              "unable to remove ambiguous staging: " + removeError.message();
          return false;
        }
      }
        error = "injected ambiguous rename with neither path";
        return false;
      case NewProfileFault::UnsafeDestinationInspection: {
        std::error_code linkError;
        std::filesystem::create_directory_symlink(external.path(), destination,
                                                  linkError);
        if (linkError) {
          error = "unable to create unsafe destination: " + linkError.message();
          return false;
        }
      }
        error = "injected unsafe destination";
        return false;
      default:
        return atomic_file::renameDurably(from, to, error);
      }
    }
    if (from == destination && to == staging) {
      ++rollbackRenameCalls;
      switch (testCase.fault) {
      case NewProfileFault::CommitSyncRollbackUnavailable:
      case NewProfileFault::RollbackUnavailableRetrySyncFails:
        error = "injected rollback rename failure";
        return false;
      case NewProfileFault::RollbackMovesThenFails:
        if (!atomic_file::renameDurably(from, to, error)) {
          return false;
        }
        error = "injected ambiguous rollback rename";
        return false;
      default:
        return atomic_file::renameDurably(from, to, error);
      }
    }
    return atomic_file::renameDurably(from, to, error);
  };
  dependencies.filesystem.syncDirectory = [&](const std::filesystem::path &path,
                                              std::string &error) {
    if (!inject || path != profiles) {
      return atomic_file::syncDirectory(path, error);
    }
    ++profilesSyncCalls;
    if (isCommitSyncFault(testCase.fault) && profilesSyncCalls == 1) {
      error = "injected commit sync failure";
      return false;
    }
    if (testCase.fault == NewProfileFault::RollbackParentSyncFails &&
        profilesSyncCalls == 2) {
      error = "injected rollback parent sync failure";
      return false;
    }
    if ((testCase.fault == NewProfileFault::RollbackUnavailableRetrySyncFails &&
         profilesSyncCalls == 2) ||
        (testCase.fault == NewProfileFault::AmbiguousFinalRetrySyncFails &&
         profilesSyncCalls == 1)) {
      error = "injected reconciliation retry sync failure";
      return false;
    }
    if (testCase.fault == NewProfileFault::RollbackCleanupSyncFails &&
        profilesSyncCalls == 3) {
      rollbackCleanupSyncAttempted = true;
      error = "injected rollback cleanup sync failure";
      return false;
    }
    return atomic_file::syncDirectory(path, error);
  };
  dependencies.filesystem.removeTree = [&](const std::filesystem::path &path,
                                           std::string &error) {
    if (inject && path == destination) {
      destinationRemovalAttempted = true;
    }
    if (inject && testCase.fault == NewProfileFault::RollbackCleanupFails &&
        path == staging && rollbackRenameCalls > 0) {
      rollbackCleanupFailureInjected = true;
      error = "injected rollback staging cleanup failure";
      return false;
    }
    return removeTree(path, error);
  };

  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  const ProfileResult initialized = manager.Initialize();
  expect(initialized.ok(),
         label + " fixture initializes: " + initialized.message);
  if (!initialized.ok()) {
    return;
  }
  const PlayerProfilePaths source = manager.pathsFor(activeId);
  seedDuplicateFaultPayload(source);
  const std::string sourceSettingsBefore = readFile(source.settingsJson);
  const std::int64_t sourceMarkerRowsBefore =
      rowCount(source.scoresDb, "profile_fault_marker");
  const std::size_t countBefore = manager.listProfiles().size();

  inject = true;
  const ProfileResult result =
      operation == NewProfileOperation::Create
          ? manager.createProfile("Fault Target")
          : manager.duplicateProfile(activeId, "Fault Target");
  inject = false;

  expect(finalRenameCalls == 1, label + " reaches final rename exactly once");
  if (expectsRollbackAttempt(testCase.fault)) {
    expect(rollbackRenameCalls == 1,
           label + " reaches rollback reconciliation");
  }
  if (testCase.fault == NewProfileFault::RollbackCleanupFails) {
    expect(rollbackCleanupFailureInjected,
           label + " reaches rollback staging cleanup");
  }
  if (testCase.fault == NewProfileFault::RollbackCleanupSyncFails) {
    expect(rollbackCleanupSyncAttempted,
           label + " reaches rollback staging cleanup sync");
  }
  expect(result.ok() == testCase.expectedSuccess,
         label + " reports the canonical outcome: " + result.message);
  if (!testCase.expectedSuccess) {
    const ProfileError expectedError =
        testCase.fault ==
                NewProfileFault::AmbiguousFinalLeavesInvalidDestination
            ? ProfileError::IntegrityFailure
            : ProfileError::IoFailure;
    expect(result.error == expectedError,
           label + " reports the classified failure kind");
  }
  if (testCase.expectedWarning) {
    expect(!result.message.empty(), label + " reports a warning");
  }

  if (testCase.safelyInspected) {
    expect(result.ok() == manager.validateProfile(generatedId).ok(),
           label + " result matches canonical profile validity");
    expect(result.ok() == hasProfile(manager, generatedId),
           label + " result matches canonical profile catalog membership");
  } else {
    expect(result.error == ProfileError::IoFailure,
           label + " fails closed on unsafe inspection");
    expect(!hasProfile(manager, generatedId),
           label + " unsafe destination is not cataloged");
  }
  expect(manager.listProfiles().size() ==
             countBefore + (testCase.expectedSuccess ? 1U : 0U),
         label + " catalog count matches the reported outcome");
  expect(stagingDirectories(temp.path()).empty() ==
             !testCase.expectedStagingEvidence,
         label + " preserves only required hidden staging evidence");

  if (testCase.fault ==
          NewProfileFault::AmbiguousFinalLeavesInvalidDestination ||
      testCase.fault == NewProfileFault::UnsafeDestinationInspection) {
    expect(!destinationRemovalAttempted,
           label + " never removes an unsafe or invalid destination");
    expect(std::filesystem::symlink_status(destination).type() !=
               std::filesystem::file_type::not_found,
           label + " preserves destination evidence");
  }
  if (testCase.fault == NewProfileFault::UnsafeDestinationInspection) {
    expect(readFile(externalSentinel) == "external-data",
           label + " leaves the external target untouched");
  }

  expect(readFile(source.settingsJson) == sourceSettingsBefore &&
             rowCount(source.scoresDb, "profile_fault_marker") ==
                 sourceMarkerRowsBefore,
         label + " does not mutate source settings or database data");
  if (operation == NewProfileOperation::Duplicate && result.ok()) {
    const PlayerProfilePaths duplicate = manager.pathsFor(generatedId);
    expect(readFile(duplicate.settingsJson) == sourceSettingsBefore &&
               rowCount(duplicate.scoresDb, "profile_fault_marker") ==
                   sourceMarkerRowsBefore,
           label + " preserves duplicate settings and database data");
  }

  PlayerProfileManager recovered(
      temp.path(), dependenciesFor("33333333-3333-4333-8333-333333333333"));
  const ProfileResult reinitialized = recovered.Initialize();
  expect(reinitialized.ok(),
         label + " clean reinitialization succeeds: " + reinitialized.message);
  expect(recovered.validateProfile(generatedId).ok() ==
             testCase.expectedSuccess,
         label + " clean reinitialization preserves canonical validity");
  expect(hasProfile(recovered, generatedId) == testCase.expectedSuccess,
         label + " clean reinitialization preserves catalog outcome");
  expect(stagingDirectories(temp.path()).empty(),
         label + " clean reinitialization recovers hidden staging");
  expect(readFile(externalSentinel) == "external-data",
         label + " recovery leaves external data untouched");
}

void testCreateAndDuplicateFaultMatrixHasFilesystemDerivedOutcomes() {
  for (const NewProfileOperation operation :
       {NewProfileOperation::Create, NewProfileOperation::Duplicate}) {
    for (const NewProfileFaultCase &testCase : kNewProfileFaultCases) {
      runNewProfileFaultCase(operation, testCase);
    }
  }
}

enum class ProfileDeletionFault {
  SourceRenameFails,
  SourceRenameMovesThenFails,
  CommitSyncRollbackSucceeds,
  RollbackUnavailableRetrySyncSucceeds,
  RollbackMovesThenFails,
  PostCommitCleanupFails,
  PostCommitCleanupPartiallyMutates,
  FinalCleanupSyncFails,
  RollbackUnavailableRetrySyncFails,
  RollbackUnavailableLeavesInvalidTombstone,
  StaleTombstoneRemovalFails,
  StaleTombstoneCleanupSyncFails,
  RollbackParentSyncFails,
  AmbiguousRenameLeavesBoth,
  SourceAbsentWithInvalidTombstone,
  PresentInvalidSource,
  BothPathsAbsent,
  UnsafeSourceInspection,
};

struct ProfileDeletionFaultCase {
  ProfileDeletionFault fault;
  std::string_view name;
  bool expectedSuccess;
  bool expectedWarning = false;
  bool expectedTombstoneEvidence = false;
  bool safelyClassified = true;
};

constexpr std::array kProfileDeletionFaultCases{
    ProfileDeletionFaultCase{ProfileDeletionFault::SourceRenameFails,
                             "point 0 source rename fails", false},
    ProfileDeletionFaultCase{ProfileDeletionFault::SourceRenameMovesThenFails,
                             "point 1 source rename moves then fails", true},
    ProfileDeletionFaultCase{ProfileDeletionFault::CommitSyncRollbackSucceeds,
                             "point 2 commit sync rollback succeeds", false},
    ProfileDeletionFaultCase{
        ProfileDeletionFault::RollbackUnavailableRetrySyncSucceeds,
        "point 3 rollback unavailable retry sync succeeds", true},
    ProfileDeletionFaultCase{ProfileDeletionFault::RollbackMovesThenFails,
                             "point 4 rollback moves then fails", false},
    ProfileDeletionFaultCase{ProfileDeletionFault::PostCommitCleanupFails,
                             "point 5 post-commit cleanup fails", true, true,
                             true},
    ProfileDeletionFaultCase{
        ProfileDeletionFault::PostCommitCleanupPartiallyMutates,
        "point 6 post-commit cleanup partially mutates", true, true, true},
    ProfileDeletionFaultCase{ProfileDeletionFault::FinalCleanupSyncFails,
                             "point 7 final cleanup sync fails", true, true},
    ProfileDeletionFaultCase{
        ProfileDeletionFault::RollbackUnavailableRetrySyncFails,
        "point 8 rollback unavailable retry sync fails", true, true, true},
    ProfileDeletionFaultCase{
        ProfileDeletionFault::RollbackUnavailableLeavesInvalidTombstone,
        "rollback unavailable leaves invalid tombstone", true, true, true},
    ProfileDeletionFaultCase{ProfileDeletionFault::StaleTombstoneRemovalFails,
                             "stale tombstone removal fails before mutation",
                             false, false, true},
    ProfileDeletionFaultCase{
        ProfileDeletionFault::StaleTombstoneCleanupSyncFails,
        "stale tombstone cleanup sync fails before mutation", false},
    ProfileDeletionFaultCase{ProfileDeletionFault::RollbackParentSyncFails,
                             "rollback parent sync fails", false},
    ProfileDeletionFaultCase{ProfileDeletionFault::AmbiguousRenameLeavesBoth,
                             "ambiguous rename leaves source and tombstone",
                             false, false, true},
    ProfileDeletionFaultCase{
        ProfileDeletionFault::SourceAbsentWithInvalidTombstone,
        "source absent with invalid tombstone", true, true, true},
    ProfileDeletionFaultCase{ProfileDeletionFault::PresentInvalidSource,
                             "present source cannot be deeply validated", false,
                             false, false, false},
    ProfileDeletionFaultCase{ProfileDeletionFault::BothPathsAbsent,
                             "source and tombstone are both absent", true,
                             true},
    ProfileDeletionFaultCase{ProfileDeletionFault::UnsafeSourceInspection,
                             "unsafe source inspection", false, false, true,
                             false},
};

bool isDeletionCommitSyncFault(ProfileDeletionFault fault) {
  switch (fault) {
  case ProfileDeletionFault::CommitSyncRollbackSucceeds:
  case ProfileDeletionFault::RollbackUnavailableRetrySyncSucceeds:
  case ProfileDeletionFault::RollbackMovesThenFails:
  case ProfileDeletionFault::RollbackUnavailableRetrySyncFails:
  case ProfileDeletionFault::RollbackUnavailableLeavesInvalidTombstone:
  case ProfileDeletionFault::RollbackParentSyncFails:
    return true;
  default:
    return false;
  }
}

bool expectsDeletionRollback(ProfileDeletionFault fault) {
  return isDeletionCommitSyncFault(fault);
}

bool expectsRollbackParentSync(ProfileDeletionFault fault) {
  switch (fault) {
  case ProfileDeletionFault::CommitSyncRollbackSucceeds:
  case ProfileDeletionFault::RollbackMovesThenFails:
  case ProfileDeletionFault::RollbackParentSyncFails:
    return true;
  default:
    return false;
  }
}

bool expectsRetryCommitSync(ProfileDeletionFault fault) {
  switch (fault) {
  case ProfileDeletionFault::RollbackUnavailableRetrySyncSucceeds:
  case ProfileDeletionFault::RollbackUnavailableRetrySyncFails:
  case ProfileDeletionFault::RollbackUnavailableLeavesInvalidTombstone:
    return true;
  default:
    return false;
  }
}

bool expectsDurabilityRecheckWarning(ProfileDeletionFault fault) {
  switch (fault) {
  case ProfileDeletionFault::SourceAbsentWithInvalidTombstone:
  case ProfileDeletionFault::RollbackUnavailableLeavesInvalidTombstone:
  case ProfileDeletionFault::BothPathsAbsent:
    return true;
  default:
    return false;
  }
}

bool hasPhysicalPath(const std::filesystem::path &path) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  expect(!error, "physical path inspection succeeds: " + error.message());
  return !error && exists;
}

void runProfileDeletionFaultCase(const ProfileDeletionFaultCase &testCase) {
  const std::string label = "delete " + std::string(testCase.name);
  TempDirectory temp("profile-delete-fault");
  TempDirectory external("profile-delete-fault-external");
  const std::string activeId = "11111111-1111-4111-8111-111111111111";
  const std::string deletedId = "22222222-2222-4222-8222-222222222222";
  std::vector<std::string> uuids{activeId, deletedId};
  std::size_t uuidIndex = 0;
  bool inject = false;
  int sourceRenameCalls = 0;
  int rollbackRenameCalls = 0;
  int tombstoneRemovalCalls = 0;
  bool initialCommitSyncAttempted = false;
  bool rollbackParentSyncAttempted = false;
  bool retryCommitSyncAttempted = false;
  bool cleanupSyncAttempted = false;
  bool staleCleanupSyncAttempted = false;
  bool staleTombstoneRemoved = false;
  bool partialCleanupInjected = false;
  bool sourceRemovalAttempted = false;
  const auto profiles = temp.path() / "profiles";
  const auto source = profiles / deletedId;
  const auto tombstone = profiles / (".deleting-" + deletedId);
  const auto externalSentinel = external.path() / "sentinel.txt";
  writeFile(externalSentinel, "external-data");

  auto dependencies = dependenciesFor();
  dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
  dependencies.filesystem.durableRename = [&](const std::filesystem::path &from,
                                              const std::filesystem::path &to,
                                              std::string &error) {
    if (!inject) {
      return atomic_file::renameDurably(from, to, error);
    }
    if (from == source && to == tombstone) {
      ++sourceRenameCalls;
      switch (testCase.fault) {
      case ProfileDeletionFault::SourceRenameFails:
        error = "injected source rename failure";
        return false;
      case ProfileDeletionFault::SourceRenameMovesThenFails:
        if (!atomic_file::renameDurably(from, to, error)) {
          return false;
        }
        error = "injected ambiguous source rename";
        return false;
      case ProfileDeletionFault::AmbiguousRenameLeavesBoth:
        if (!copyTreeForFault(from, to, error)) {
          return false;
        }
        error = "injected ambiguous source rename with both paths";
        return false;
      case ProfileDeletionFault::SourceAbsentWithInvalidTombstone: {
        if (!atomic_file::renameDurably(from, to, error)) {
          return false;
        }
        std::error_code removeError;
        std::filesystem::remove(tombstone / "profile.json", removeError);
        if (removeError) {
          error = "unable to invalidate tombstone: " + removeError.message();
          return false;
        }
        error = "injected ambiguous rename with invalid tombstone";
        return false;
      }
      case ProfileDeletionFault::PresentInvalidSource: {
        std::error_code removeError;
        std::filesystem::remove(source / "settings.json", removeError);
        if (removeError) {
          error = "unable to invalidate source: " + removeError.message();
          return false;
        }
        error = "injected invalid canonical source";
        return false;
      }
      case ProfileDeletionFault::BothPathsAbsent: {
        std::error_code removeError;
        std::filesystem::remove_all(source, removeError);
        if (removeError) {
          error = "unable to remove source: " + removeError.message();
          return false;
        }
        error = "injected ambiguous rename with neither path";
        return false;
      }
      case ProfileDeletionFault::UnsafeSourceInspection: {
        if (!atomic_file::renameDurably(from, to, error)) {
          return false;
        }
        std::error_code linkError;
        std::filesystem::create_directory_symlink(external.path(), source,
                                                  linkError);
        if (linkError) {
          error = "unable to create unsafe source: " + linkError.message();
          return false;
        }
        error = "injected unsafe canonical source";
        return false;
      }
      default:
        return atomic_file::renameDurably(from, to, error);
      }
    }
    if (from == tombstone && to == source) {
      ++rollbackRenameCalls;
      switch (testCase.fault) {
      case ProfileDeletionFault::RollbackUnavailableRetrySyncSucceeds:
      case ProfileDeletionFault::RollbackUnavailableRetrySyncFails:
        error = "injected rollback rename failure";
        return false;
      case ProfileDeletionFault::RollbackUnavailableLeavesInvalidTombstone: {
        std::error_code removeError;
        std::filesystem::remove(tombstone / "profile.json", removeError);
        if (removeError) {
          error = "unable to invalidate rollback tombstone: " +
                  removeError.message();
          return false;
        }
        error = "injected rollback failure with invalid tombstone";
        return false;
      }
      case ProfileDeletionFault::RollbackMovesThenFails:
        if (!atomic_file::renameDurably(from, to, error)) {
          return false;
        }
        error = "injected ambiguous rollback rename";
        return false;
      default:
        return atomic_file::renameDurably(from, to, error);
      }
    }
    return atomic_file::renameDurably(from, to, error);
  };
  dependencies.filesystem.syncDirectory = [&](const std::filesystem::path &path,
                                              std::string &error) {
    if (!inject || path != profiles) {
      return atomic_file::syncDirectory(path, error);
    }
    if (sourceRenameCalls == 0) {
      staleCleanupSyncAttempted = true;
      if (testCase.fault ==
          ProfileDeletionFault::StaleTombstoneCleanupSyncFails) {
        error = "injected stale tombstone cleanup sync failure";
        return false;
      }
      return atomic_file::syncDirectory(path, error);
    }
    if (tombstoneRemovalCalls > 0) {
      cleanupSyncAttempted = true;
      if (testCase.fault == ProfileDeletionFault::FinalCleanupSyncFails) {
        error = "injected final cleanup sync failure";
        return false;
      }
      return atomic_file::syncDirectory(path, error);
    }
    if (rollbackRenameCalls == 0) {
      initialCommitSyncAttempted = true;
      if (isDeletionCommitSyncFault(testCase.fault)) {
        error = "injected delete commit sync failure";
        return false;
      }
      return atomic_file::syncDirectory(path, error);
    }

    std::error_code sourceError;
    const bool sourceExists = std::filesystem::exists(source, sourceError);
    expect(!sourceError, label + " sync-state source inspection succeeds: " +
                             sourceError.message());
    if (sourceExists) {
      rollbackParentSyncAttempted = true;
      if (testCase.fault == ProfileDeletionFault::RollbackParentSyncFails) {
        error = "injected rollback parent sync failure";
        return false;
      }
      return atomic_file::syncDirectory(path, error);
    }
    retryCommitSyncAttempted = true;
    if (testCase.fault ==
        ProfileDeletionFault::RollbackUnavailableRetrySyncFails) {
      error = "injected retry commit sync failure";
      return false;
    }
    return atomic_file::syncDirectory(path, error);
  };
  dependencies.filesystem.removeTree = [&](const std::filesystem::path &path,
                                           std::string &error) {
    if (inject && path == source) {
      sourceRemovalAttempted = true;
    }
    if (!inject || path != tombstone) {
      return removeTree(path, error);
    }
    ++tombstoneRemovalCalls;
    if (testCase.fault == ProfileDeletionFault::StaleTombstoneRemovalFails &&
        sourceRenameCalls == 0) {
      error = "injected stale tombstone removal failure";
      return false;
    }
    if (testCase.fault ==
            ProfileDeletionFault::StaleTombstoneCleanupSyncFails &&
        sourceRenameCalls == 0) {
      const bool removed = removeTree(path, error);
      staleTombstoneRemoved = removed;
      return removed;
    }
    if (testCase.fault == ProfileDeletionFault::PostCommitCleanupFails) {
      error = "injected post-commit tombstone cleanup failure";
      return false;
    }
    if (testCase.fault ==
        ProfileDeletionFault::PostCommitCleanupPartiallyMutates) {
      std::error_code removeError;
      std::filesystem::remove(path / "profile.json", removeError);
      if (removeError) {
        error = "unable to partially clean tombstone: " + removeError.message();
        return false;
      }
      partialCleanupInjected = true;
      error = "injected partial tombstone cleanup failure";
      return false;
    }
    return removeTree(path, error);
  };

  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  const ProfileResult initialized = manager.Initialize();
  expect(initialized.ok(),
         label + " fixture initializes: " + initialized.message);
  if (!initialized.ok()) {
    return;
  }
  const ProfileResult created = manager.createProfile("Delete Fault Target");
  expect(created.ok() && created.profile,
         label + " fixture creates an inactive profile");
  if (!created.profile) {
    return;
  }
  const std::size_t countBefore = manager.listProfiles().size();

  if (testCase.fault == ProfileDeletionFault::StaleTombstoneRemovalFails ||
      testCase.fault == ProfileDeletionFault::StaleTombstoneCleanupSyncFails) {
    std::string copyError;
    expect(copyTreeForFault(source, tombstone, copyError),
           label + " creates a stale tombstone: " + copyError);
  }

  inject = true;
  const ProfileResult result = manager.deleteProfile(deletedId);
  inject = false;

  const bool stalePrecommitFault =
      testCase.fault == ProfileDeletionFault::StaleTombstoneRemovalFails ||
      testCase.fault == ProfileDeletionFault::StaleTombstoneCleanupSyncFails;
  expect(sourceRenameCalls == (stalePrecommitFault ? 0 : 1),
         label + " reaches only the expected source rename");
  if (expectsDeletionRollback(testCase.fault)) {
    expect(rollbackRenameCalls == 1,
           label + " reaches rollback reconciliation");
    expect(initialCommitSyncAttempted,
           label + " reaches the initial deletion commit sync");
  }
  if (expectsRollbackParentSync(testCase.fault)) {
    expect(rollbackParentSyncAttempted,
           label + " attempts the restored-source parent sync");
    expect(!retryCommitSyncAttempted,
           label + " does not use the absent-source retry sync");
  }
  if (expectsRetryCommitSync(testCase.fault)) {
    expect(retryCommitSyncAttempted,
           label + " attempts the absent-source retry commit sync");
    expect(!rollbackParentSyncAttempted,
           label + " does not use the restored-source parent sync");
  }
  if (testCase.fault == ProfileDeletionFault::StaleTombstoneCleanupSyncFails) {
    expect(staleTombstoneRemoved,
           label + " removes stale tombstone before failed cleanup sync");
    expect(staleCleanupSyncAttempted,
           label + " attempts the stale-tombstone cleanup sync");
  }
  if (testCase.fault == ProfileDeletionFault::FinalCleanupSyncFails) {
    expect(cleanupSyncAttempted,
           label + " attempts the post-commit cleanup sync");
  }
  if (testCase.fault ==
      ProfileDeletionFault::PostCommitCleanupPartiallyMutates) {
    expect(partialCleanupInjected,
           label + " reaches partial post-commit cleanup");
  }
  expect(result.ok() == testCase.expectedSuccess,
         label + " reports the canonical outcome: " + result.message);
  if (testCase.expectedWarning) {
    expect(!result.message.empty(), label + " reports a warning");
  }
  if (expectsDurabilityRecheckWarning(testCase.fault)) {
    expect(result.message.find("durability should be rechecked") !=
               std::string::npos,
           label +
               " explicitly warns that directory durability is unconfirmed");
  }

  std::error_code sourceError;
  const bool sourceExists = std::filesystem::exists(source, sourceError);
  expect(!sourceError, label + " canonical source inspection succeeds");
  if (testCase.safelyClassified) {
    expect(!sourceError && result.ok() == !sourceExists,
           label + " result matches physical canonical source presence");
    expect(result.ok() == !hasProfile(manager, deletedId),
           label + " result matches catalog membership");
    expect(manager.listProfiles().size() ==
               countBefore - (testCase.expectedSuccess ? 1U : 0U),
           label + " catalog count matches the reported outcome");
  } else {
    expect(!result.ok() && sourceExists,
           label + " fails closed while preserving the canonical path");
    expect(!manager.validateProfile(deletedId).ok(),
           label + " canonical path remains unverifiable");
    expect(!sourceRemovalAttempted,
           label + " validation failure never authorizes source cleanup");
  }
  if (testCase.expectedTombstoneEvidence) {
    expect(hasPhysicalPath(tombstone),
           label + " preserves required tombstone evidence");
  }
  if (testCase.fault ==
          ProfileDeletionFault::SourceAbsentWithInvalidTombstone ||
      testCase.fault ==
          ProfileDeletionFault::RollbackUnavailableLeavesInvalidTombstone) {
    expect(!manager.validateProfile(deletedId).ok() &&
               tombstoneRemovalCalls == 0,
           label + " preserves invalid tombstone evidence without cleanup");
  }
  if (testCase.fault == ProfileDeletionFault::UnsafeSourceInspection) {
    expect(result.error == ProfileError::IoFailure &&
               tombstoneRemovalCalls == 0,
           label + " rejects unsafe inspection without cleanup");
  }
  expect(readFile(externalSentinel) == "external-data",
         label + " leaves external data untouched");

  PlayerProfileManager recovered(
      temp.path(), dependenciesFor("33333333-3333-4333-8333-333333333333"));
  const ProfileResult reinitialized = recovered.Initialize();
  expect(reinitialized.ok(),
         label + " clean reinitialization succeeds: " + reinitialized.message);
  expect(!hasPhysicalPath(tombstone),
         label + " clean reinitialization removes recoverable tombstones");
  if (testCase.safelyClassified) {
    expect(recovered.validateProfile(deletedId).ok() ==
               !testCase.expectedSuccess,
           label + " clean reinitialization preserves source validity");
    expect(hasProfile(recovered, deletedId) == !testCase.expectedSuccess,
           label + " clean reinitialization preserves catalog outcome");
  } else {
    expect(hasPhysicalPath(source) &&
               !recovered.validateProfile(deletedId).ok(),
           label + " clean reinitialization preserves unsafe source evidence");
  }
  expect(readFile(externalSentinel) == "external-data",
         label + " recovery leaves external data untouched");
}

void testProfileDeletionFaultMatrixHasFilesystemDerivedOutcomes() {
  for (const ProfileDeletionFaultCase &testCase : kProfileDeletionFaultCases) {
    runProfileDeletionFaultCase(testCase);
  }
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
  const auto deleted = manager.deleteProfile(deletedId);
  expect(deleted.ok() && !deleted.message.empty(),
         "post-commit tombstone cleanup failure reports success warning");
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

void testFailedDeleteCommitSyncRestoresProfile() {
  TempDirectory temp("profile-delete-parent-sync");
  const std::string activeId = "11111111-1111-4111-8111-111111111111";
  const std::string deletedId = "22222222-2222-4222-8222-222222222222";
  std::vector<std::string> uuids{activeId, deletedId};
  std::size_t uuidIndex = 0;
  bool inject = false;
  bool deletionRenameCompleted = false;
  bool commitSyncFailed = false;
  const auto profiles = temp.path() / "profiles";
  const auto source = profiles / deletedId;
  const auto tombstone = profiles / (".deleting-" + deletedId);

  auto dependencies = dependenciesFor();
  dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
  dependencies.filesystem.durableRename = [&](const std::filesystem::path &from,
                                              const std::filesystem::path &to,
                                              std::string &error) {
    const bool renamed = atomic_file::renameDurably(from, to, error);
    if (inject && renamed && from == source && to == tombstone) {
      deletionRenameCompleted = true;
    }
    return renamed;
  };
  dependencies.filesystem.syncDirectory = [&](const std::filesystem::path &path,
                                              std::string &error) {
    if (inject && deletionRenameCompleted && !commitSyncFailed &&
        path == profiles) {
      commitSyncFailed = true;
      error = "injected delete commit sync failure";
      return false;
    }
    return atomic_file::syncDirectory(path, error);
  };

  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(), "delete parent-sync fixture initializes");
  const auto created = manager.createProfile("Delete Sync Failure");
  expect(created.ok() && created.profile,
         "delete parent-sync fixture creates an inactive profile");
  if (!created.profile) {
    return;
  }
  const std::size_t countBefore = manager.listProfiles().size();

  inject = true;
  const ProfileResult result = manager.deleteProfile(deletedId);
  inject = false;
  expect(result.error == ProfileError::IoFailure,
         "failed delete commit sync reports failure");
  expect(manager.validateProfile(deletedId).ok(),
         "failed delete commit sync restores the valid canonical source");
  expect(hasProfile(manager, deletedId) &&
             manager.listProfiles().size() == countBefore,
         "failed delete commit sync leaves the visible catalog unchanged");

  PlayerProfileManager recovered(
      temp.path(), dependenciesFor("33333333-3333-4333-8333-333333333333"));
  expect(recovered.Initialize().ok(),
         "delete parent-sync failure reinitializes cleanly");
  expect(recovered.validateProfile(deletedId).ok() &&
             hasProfile(recovered, deletedId) &&
             recovered.listProfiles().size() == countBefore,
         "reinitialization preserves the restored delete outcome");
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

void testSupportedOlderInactiveProfileUsesManagePolicy() {
  TempDirectory temp("profile-supported-older-manage");
  const std::string activeId = "11111111-1111-4111-8111-111111111111";
  const std::string olderId = "22222222-2222-4222-8222-222222222222";
  const std::string duplicateId = "33333333-3333-4333-8333-333333333333";
  std::vector<std::string> uuids{activeId, olderId, duplicateId};
  std::size_t uuidIndex = 0;
  auto dependencies = dependenciesFor();
  dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(),
         "supported-older management fixture initializes");
  const ProfileResult created = manager.createProfile("Older Inactive");
  expect(created.ok() && created.profile,
         "supported-older management target is created");
  if (!created.profile) {
    return;
  }

  const PlayerProfilePaths olderPaths = manager.pathsFor(olderId);
  seedSupportedOlderProfile(olderPaths, "supported-older management target");
  seedReplayMigrationChartMetadata(temp.path() / "db" / "chart.db");
  const std::string settingsBefore = readFile(olderPaths.settingsJson);
  const std::string inputBefore = readFile(olderPaths.inputJson);
  const std::string metadataBeforeCommit = readFile(olderPaths.profileJson);
  const auto bootstrap = temp.path() / "active-profile.json";
  const std::string bootstrapBefore = readFile(bootstrap);

  expect(hasProfile(manager, olderId),
         "supported-older inactive profile remains cataloged");
  expect(manager.validateProfileForActivation(olderId).ok(),
         "activation preflight admits the supported-older profile");
  expect(manager.validateProfile(olderId).error == ProfileError::IntegrityFailure,
         "strict public validation rejects the supported-older profile");
  expect(manager.commitActiveProfile(olderId).error ==
             ProfileError::IntegrityFailure,
         "runtime commit rejects the supported-older profile before binding");
  expect(manager.activeProfile().id == activeId &&
             readFile(bootstrap) == bootstrapBefore &&
             readFile(olderPaths.profileJson) == metadataBeforeCommit,
         "rejected runtime commit leaves bootstrap and metadata unchanged");

  const ProfileResult renamed = manager.renameProfile(olderId, "Older Renamed");
  expect(renamed.ok() && renamed.profile &&
             renamed.profile->displayName == "Older Renamed",
         "supported-older inactive profile can be renamed");
  expect(hasProfile(manager, olderId),
         "renamed supported-older profile remains cataloged");
  expect(readFile(olderPaths.settingsJson) == settingsBefore &&
             readFile(olderPaths.inputJson) == inputBefore,
         "supported-older rename changes metadata only");
  expectSupportedOlderPayload(olderPaths,
                              "supported-older profile after rename");

  const ProfileResult duplicated =
      manager.duplicateProfile(olderId, "Older Copy");
  expect(duplicated.ok() && duplicated.profile &&
             duplicated.profile->id == duplicateId,
         "supported-older inactive profile can be duplicated");
  expectSupportedOlderPayload(olderPaths,
                              "supported-older source after duplication");
  if (duplicated.profile) {
    const PlayerProfilePaths copyPaths = manager.pathsFor(duplicateId);
    std::string versionError;
    expect(sqliteDatabaseUserVersion(copyPaths.scoresDb, versionError) ==
                   ScoreRepository::kCurrentSchemaVersion &&
               sqliteDatabaseUserVersion(copyPaths.replaysDb, versionError) ==
                   ReplayRepository::kCurrentSchemaVersion,
           "normal duplicate database owners migrate the copy to current");
    expect(rowCount(copyPaths.scoresDb, "validation_policy_marker") == 1 &&
               rowCount(copyPaths.replaysDb, "validation_policy_marker") == 1,
           "supported-older duplicate preserves both marker rows");
    expect(rowCount(copyPaths.replaysDb, "chart_results") == 0 &&
               rowCount(copyPaths.replaysDb, "course_results") == 1 &&
               rowCount(copyPaths.replaysDb, "course_result_stages") == 1 &&
               rowCount(copyPaths.replaysDb, "replay_files") == 1,
           "course-only stage replay migrates into one course result and one "
           "course BRD without a standalone chart result");
    expect(manager.validateProfile(duplicateId).ok(),
           "migrated supported-older duplicate is runtime ready");
    expect(manager.deleteProfile(duplicateId).ok(),
           "current duplicate can be deleted before the older source");
  }

  expect(manager.listProfiles().size() == 2 && hasProfile(manager, activeId) &&
             hasProfile(manager, olderId),
         "mixed current and supported-older profiles remain manageable");
  const ProfileResult deleted = manager.deleteProfile(olderId);
  expect(deleted.ok() && deleted.message.empty(),
         "supported-older inactive profile deletes without a warning");
  expect(!std::filesystem::exists(olderPaths.root) &&
             !std::filesystem::exists(temp.path() / "profiles" /
                                      (".deleting-" + olderId)),
         "supported-older deletion removes canonical and tombstone paths");
  expect(manager.listProfiles().size() == 1 && hasProfile(manager, activeId) &&
             profileTransactionArtifacts(temp.path()).empty(),
         "supported-older deletion leaves only the active catalog profile");
}

void testSupportedOlderDeleteRollbackRestoresManageableSource() {
  TempDirectory temp("profile-supported-older-delete-rollback");
  const std::string activeId = "11111111-1111-4111-8111-111111111111";
  const std::string olderId = "22222222-2222-4222-8222-222222222222";
  std::vector<std::string> uuids{activeId, olderId};
  std::size_t uuidIndex = 0;
  bool inject = false;
  bool sourceRenameReached = false;
  bool commitSyncFailed = false;
  bool rollbackRenameReached = false;
  bool rollbackSyncReached = false;
  const auto profiles = temp.path() / "profiles";
  const auto source = profiles / olderId;
  const auto tombstone = profiles / (".deleting-" + olderId);

  auto dependencies = dependenciesFor();
  dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
  dependencies.filesystem.durableRename = [&](const std::filesystem::path &from,
                                              const std::filesystem::path &to,
                                              std::string &error) {
    if (inject && from == source && to == tombstone) {
      sourceRenameReached = true;
    } else if (inject && from == tombstone && to == source) {
      rollbackRenameReached = true;
    }
    return atomic_file::renameDurably(from, to, error);
  };
  dependencies.filesystem.syncDirectory = [&](const std::filesystem::path &path,
                                              std::string &error) {
    if (inject && path == profiles && sourceRenameReached &&
        !rollbackRenameReached && !commitSyncFailed) {
      commitSyncFailed = true;
      error = "injected supported-older deletion commit sync failure";
      return false;
    }
    if (inject && path == profiles && rollbackRenameReached) {
      rollbackSyncReached = true;
    }
    return atomic_file::syncDirectory(path, error);
  };

  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(),
         "supported-older rollback fixture initializes");
  const ProfileResult created = manager.createProfile("Older Rollback");
  expect(created.ok() && created.profile,
         "supported-older rollback target is created");
  if (!created.profile) {
    return;
  }
  const PlayerProfilePaths olderPaths = manager.pathsFor(olderId);
  seedSupportedOlderProfile(olderPaths, "supported-older rollback target");
  const std::string settingsBefore = readFile(olderPaths.settingsJson);
  const std::string inputBefore = readFile(olderPaths.inputJson);

  inject = true;
  const ProfileResult deleted = manager.deleteProfile(olderId);
  inject = false;
  expect(deleted.error == ProfileError::IoFailure,
         "supported-older commit-sync failure reports deletion failure");
  expect(sourceRenameReached && commitSyncFailed && rollbackRenameReached &&
             rollbackSyncReached,
         "supported-older deletion reaches commit failure and durable rollback");
  expect(std::filesystem::exists(source) &&
             !std::filesystem::exists(tombstone),
         "supported-older deletion rollback restores only the source path");
  expect(manager.validateProfileForActivation(olderId).ok() &&
             hasProfile(manager, olderId) && manager.listProfiles().size() == 2,
         "restored supported-older source is manageable and cataloged");
  expect(!manager.validateProfile(olderId).ok(),
         "restored supported-older source remains intentionally runtime strict");
  expect(readFile(olderPaths.settingsJson) == settingsBefore &&
             readFile(olderPaths.inputJson) == inputBefore,
         "supported-older rollback preserves non-database profile data");
  expectSupportedOlderPayload(olderPaths,
                              "supported-older source after rollback");

  PlayerProfileManager recovered(
      temp.path(), dependenciesFor("33333333-3333-4333-8333-333333333333"));
  const ProfileResult initialized = recovered.Initialize();
  expect(initialized.ok(),
         "supported-older rollback state reinitializes: " + initialized.message);
  expect(recovered.activeProfile().id == activeId &&
             recovered.validateProfileForActivation(olderId).ok() &&
             hasProfile(recovered, olderId) &&
             recovered.listProfiles().size() == 2 &&
             profileTransactionArtifacts(temp.path()).empty(),
         "clean reinitialization preserves the restored manageable source");
  expectSupportedOlderPayload(recovered.pathsFor(olderId),
                              "reinitialized supported-older source");
}

void testFutureDatabaseProfileIsNeverManageable() {
  for (const bool futureScoreDatabase : {true, false}) {
    TempDirectory temp(futureScoreDatabase ? "profile-future-managed-score"
                                            : "profile-future-managed-replay");
    const std::string activeId = "11111111-1111-4111-8111-111111111111";
    const std::string futureId = "22222222-2222-4222-8222-222222222222";
    const std::string duplicateId = "33333333-3333-4333-8333-333333333333";
    std::vector<std::string> uuids{activeId, futureId, duplicateId};
    std::size_t uuidIndex = 0;
    auto dependencies = dependenciesFor();
    dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
    PlayerProfileManager manager(temp.path(), std::move(dependencies));
    expect(manager.Initialize().ok(), "future managed fixture initializes");
    const ProfileResult created = manager.createProfile("Future Inactive");
    expect(created.ok() && created.profile,
           "future managed target is created");
    if (!created.profile) {
      continue;
    }

    const PlayerProfilePaths paths = manager.pathsFor(futureId);
    const auto futureDatabase =
        futureScoreDatabase ? paths.scoresDb : paths.replaysDb;
    const int futureVersion =
        futureScoreDatabase ? ScoreRepository::kCurrentSchemaVersion + 1
                            : ReplayRepository::kCurrentSchemaVersion + 1;
    setDatabaseVersion(futureDatabase, futureVersion,
                       futureScoreDatabase ? "future score"
                                           : "future replay");
    const std::string metadataBefore = readFile(paths.profileJson);
    const auto bootstrap = temp.path() / "active-profile.json";
    const std::string bootstrapBefore = readFile(bootstrap);
    const auto artifactsBefore = profileTransactionArtifacts(temp.path());

    expect(!hasProfile(manager, futureId),
           "future database profile is omitted from the catalog");
    expect(manager.renameProfile(futureId, "Rejected Future").error ==
               ProfileError::FutureVersion,
           "rename rejects a future database profile");
    expect(manager.duplicateProfile(futureId, "Rejected Future Copy").error ==
               ProfileError::FutureVersion,
           "duplicate rejects a future database profile");
    expect(manager.deleteProfile(futureId).error == ProfileError::FutureVersion,
           "delete rejects a future database profile");
    expect(manager.validateProfileForActivation(futureId).error ==
               ProfileError::FutureVersion,
           "activation preflight rejects a future database profile");
    expect(manager.validateProfile(futureId).error ==
               ProfileError::FutureVersion,
           "runtime validation rejects a future database profile");
    expect(manager.commitActiveProfile(futureId).error ==
               ProfileError::FutureVersion,
           "runtime commit rejects a future database profile");

    std::string versionError;
    expect(std::filesystem::exists(paths.root) &&
               sqliteDatabaseUserVersion(futureDatabase, versionError) ==
                   futureVersion,
           "future database path and version remain unchanged: " +
               versionError);
    expect(readFile(paths.profileJson) == metadataBefore &&
               readFile(bootstrap) == bootstrapBefore,
           "future database rejection preserves metadata and bootstrap");
    expect(profileTransactionArtifacts(temp.path()) == artifactsBefore,
           "future database rejection creates no transaction artifacts");
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
    expect(scores != nullptr, "supported-older score marker database opens");
    if (!scores) {
      return;
    }
    expect(execute(scores.get(),
                   "CREATE TABLE migration_marker(value TEXT);"
                   "INSERT INTO migration_marker VALUES('score');"
                   "PRAGMA user_version=5"),
           "score v5 fixture and marker are written");
  }
  {
    std::error_code removeError;
    std::filesystem::remove(paths.replaysDb, removeError);
    expect(!removeError, "current replay database removes for v10 fixture");
    if (removeError) {
      return;
    }
    Database replays = openDatabase(paths.replaysDb);
    expect(replays != nullptr, "supported-older replay marker database opens");
    if (!replays) {
      return;
    }
    try {
      replay_schema10_fixture::createExactSchema(replays.get());
    } catch (const std::exception &exception) {
      expect(false, "replay v10 fixture creates: " +
                        std::string(exception.what()));
      return;
    }
    expect(execute(replays.get(),
                   "CREATE TABLE migration_marker(value TEXT);"
                   "INSERT INTO migration_marker VALUES('replay')"),
           "replay v10 fixture and marker are written");
  }

  std::string versionError;
  expect(!creator.validateProfile(profileId).ok(),
         "strict public validation rejects a profile awaiting migration");
  expect(sqliteDatabaseUserVersion(paths.scoresDb, versionError) == 5 &&
             sqliteDatabaseUserVersion(paths.replaysDb, versionError) == 10,
         "strict validation does not mutate supported older databases");

  PlayerProfileManager reopened(
      temp.path(), dependenciesFor("22222222-2222-4222-8222-222222222222"));
  const ProfileResult initialized = reopened.Initialize();
  expect(initialized.ok() && reopened.activeProfile().id == profileId,
         "startup admits the active v5/v10 profile for its schema owners: " +
             initialized.message);
  expect(reopened.listProfiles().size() == 1,
         "profile discovery keeps a supported older profile selectable");
  expect(sqliteDatabaseUserVersion(paths.scoresDb, versionError) == 5 &&
             sqliteDatabaseUserVersion(paths.replaysDb, versionError) == 10,
         "profile startup preflight remains read-only");

  ScoreRepository score(paths.scoresDb);
  ReplayRepository replay(paths.replaysDb);
  expect(score.EnsureSchema() && replay.EnsureSchema(),
         "normal database owners migrate the admitted profile");
  score.Shutdown();
  replay.Shutdown();
  expect(sqliteDatabaseUserVersion(paths.scoresDb, versionError) ==
                 ScoreRepository::kCurrentSchemaVersion &&
             sqliteDatabaseUserVersion(paths.replaysDb, versionError) ==
                 ReplayRepository::kCurrentSchemaVersion,
         "database owners advance v5/v10 to the current schemas");
  expect(rowCount(paths.scoresDb, "migration_marker") == 1 &&
             rowCount(paths.replaysDb, "migration_marker") == 1,
         "database migrations preserve profile rows");
  expect(reopened.validateProfile(profileId).ok(),
         "strict validation succeeds after owned migration");
}

void testDuplicateMigratesCompactSchema13AndPreservesRows() {
  TempDirectory temp("profile-duplicate-schema13");
  auto dependencies = dependenciesFor();
  int uuidIndex = 0;
  dependencies.generateUuid = [&] {
    return uuidIndex++ == 0 ? "11111111-1111-4111-8111-111111111111"
                            : "22222222-2222-4222-8222-222222222222";
  };
  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(), "schema-13 duplicate fixture initializes");
  const auto sourcePaths = manager.activePaths();
  {
    Database database = openDatabase(sourcePaths.replaysDb);
    expect(database != nullptr &&
               execute(database.get(),
                       "ALTER TABLE course_results DROP COLUMN "
                       "entry_facts_json;PRAGMA user_version=13"),
           "duplicate source is downgraded to compact schema 13");
  }

  const auto duplicated =
      manager.duplicateProfile(manager.activeProfile().id, "Schema 13 Copy");
  expect(duplicated.ok() && duplicated.profile.has_value(),
         "supported compact schema 13 duplicates through migration: " +
             duplicated.message);
  if (!duplicated.profile) {
    return;
  }
  std::string versionError;
  const auto duplicatePaths = manager.pathsFor(duplicated.profile->id);
  expect(sqliteDatabaseUserVersion(duplicatePaths.replaysDb, versionError) ==
             ReplayRepository::kCurrentSchemaVersion,
         "schema-13 duplicate is stored at the current replay schema");
}

void testDuplicateHoldsProfileActivityExclusionAcrossSnapshotAndFiles() {
  TempDirectory temp("profile-duplicate-guard");
  auto dependencies = dependenciesFor();
  int uuidIndex = 0;
  dependencies.generateUuid = [&] {
    return uuidIndex++ == 0 ? "11111111-1111-4111-8111-111111111111"
                            : "22222222-2222-4222-8222-222222222222";
  };
  std::filesystem::path replaySource;
  std::mutex mutex;
  std::condition_variable condition;
  bool replaySnapshotReached = false;
  bool releaseDuplicate = false;
  dependencies.snapshotDatabase = [&](const std::filesystem::path &source,
                                      const std::filesystem::path &destination,
                                      std::string &errorMessage) {
    if (!replaySource.empty() && source == replaySource) {
      std::unique_lock lock(mutex);
      replaySnapshotReached = true;
      condition.notify_all();
      condition.wait(lock, [&] { return releaseDuplicate; });
    }
    return snapshotSqliteDatabase(source, destination, errorMessage);
  };
  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(), "guarded duplicate fixture initializes");
  replaySource = manager.activePaths().replaysDb;

  ProfileResult duplicated;
  std::thread worker([&] {
    duplicated =
        manager.duplicateProfile(manager.activeProfile().id, "Guarded Copy");
  });
  {
    std::unique_lock lock(mutex);
    condition.wait(lock, [&] { return replaySnapshotReached; });
  }
  bool competingOperationEntered = false;
  {
    profile_database_activity::SwitchGuard competingOperation;
    competingOperationEntered = competingOperation.ownsLock();
  }
  expect(!competingOperationEntered,
         "duplicate excludes profile mutations across replay file and database "
         "copying");
  {
    std::lock_guard lock(mutex);
    releaseDuplicate = true;
  }
  condition.notify_all();
  worker.join();
  expect(duplicated.ok(), "guarded duplicate still succeeds");
}

void testDuplicateRejectsReferencedReplayChecksumMismatch() {
  TempDirectory temp("profile-duplicate-replay-checksum");
  auto dependencies = dependenciesFor();
  int uuidIndex = 0;
  dependencies.generateUuid = [&] {
    return uuidIndex++ == 0 ? "11111111-1111-4111-8111-111111111111"
                            : "22222222-2222-4222-8222-222222222222";
  };
  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(), "checksum duplicate fixture initializes");
  const auto source = manager.activePaths();
  seedReferencedReplay(source, "checksum duplicate source");
  writeFile(source.replayDirectory / kReferencedReplayFilename,
            "OWNED replay bytes\n");

  const auto duplicated = manager.duplicateProfile(manager.activeProfile().id,
                                                   "Checksum Mismatch Copy");
  expect(!duplicated.ok() && duplicated.error == ProfileError::IntegrityFailure,
         "duplicate rejects referenced replay bytes that differ from the "
         "database checksum: " +
             duplicated.message);
}

void testDuplicatePreservesDeletedReferencedReplayAsMissing() {
  TempDirectory temp("profile-duplicate-missing-replay");
  auto dependencies = dependenciesFor();
  int uuidIndex = 0;
  dependencies.generateUuid = [&] {
    return uuidIndex++ == 0 ? "11111111-1111-4111-8111-111111111111"
                            : "22222222-2222-4222-8222-222222222222";
  };
  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(), "missing-replay duplicate initializes");
  const auto source = manager.activePaths();
  seedReferencedReplay(source, "missing-replay duplicate source");

  const auto duplicated = manager.duplicateProfile(manager.activeProfile().id,
                                                   "Missing Replay Copy");
  expect(duplicated.ok() && duplicated.profile.has_value(),
         "duplicate accepts a replay reference whose user-deleted file is "
         "missing: " +
             duplicated.message);
  if (duplicated.profile) {
    expect(!std::filesystem::exists(
               manager.pathsFor(duplicated.profile->id).replayDirectory /
               kReferencedReplayFilename),
           "duplicate keeps the user-deleted replay unavailable");
  }
}

void testUnsupportedPreV10ReplayProfileFailsActivationPreflight() {
  TempDirectory temp("profile-unsupported-pre-v10-replay");
  PlayerProfileManager manager(temp.path(), dependenciesFor());
  expect(manager.Initialize().ok(), "pre-v10 replay fixture initializes");
  const auto paths = manager.activePaths();
  setDatabaseVersion(paths.replaysDb, 9, "pre-v10 replay");

  expect(manager.validateProfileForActivation(manager.activeProfile().id).error ==
             ProfileError::IntegrityFailure,
         "activation preflight rejects replay schemas older than v10");
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
             std::filesystem::exists(manager.pathsFor(secondId).replaysDb) &&
             std::filesystem::is_directory(
                 manager.pathsFor(secondId).practiceDirectory) &&
             std::filesystem::is_directory(
                 manager.pathsFor(secondId).replayDirectory),
         "created profile owns all isolated data files");
  expect(!std::filesystem::exists(
             manager.pathsFor(secondId).irCredentialsJson),
         "created profile does not create a credential file");

  auto sourceSettings =
      AppSettingsStore::Load(manager.pathsFor(firstId).settingsJson).settings;
  sourceSettings.selectedGameplayRuleset = "beatoraja";
  std::string settingsError;
  expect(AppSettingsStore::Save(manager.pathsFor(firstId).settingsJson,
                                sourceSettings, settingsError),
         "duplicate source ruleset saves: " + settingsError);

  writeFile(manager.pathsFor(firstId).irCredentialsJson,
            R"({"schemaVersion":1,"providers":{"tachi":{"apiKey":"sentinel-api-key"}}})");
  constexpr std::string_view replayBytes = "duplicate-replay-bytes\n";
  writeFile(manager.pathsFor(firstId).replayDirectory / "copy-test.brd",
            replayBytes);
  seedIrOperationalState(manager.pathsFor(firstId).replaysDb,
                         "duplicate source");
  seedImportedIrScore(manager.pathsFor(firstId).scoresDb, "duplicate source");
  expect(rowCount(manager.pathsFor(firstId).replaysDb, "ir_outbox") == 3,
         "duplicate source starts with three IR operational rows");
  expect(rowCount(manager.pathsFor(firstId).replaysDb,
                  "ir_submission_receipts") == 1 &&
             rowCount(manager.pathsFor(firstId).replaysDb,
                      "ir_remote_scores") == 1 &&
             matchingRowCount(manager.pathsFor(firstId).scoresDb,
                              "SELECT COUNT(*) FROM scores WHERE "
                              "score_source=1") == 1,
         "duplicate source starts with account-scoped IR data");

  const auto duplicate = manager.duplicateProfile(firstId, "First Copy");
  expect(duplicate.ok() && duplicate.profile.has_value(),
         "profile duplication succeeds");
  const std::string copyId = duplicate.profile ? duplicate.profile->id : "";
  expect(readFile(manager.pathsFor(copyId).settingsJson) ==
             readFile(manager.pathsFor(firstId).settingsJson),
         "duplicate preserves settings bytes");
  expect(AppSettingsStore::Load(manager.pathsFor(copyId).settingsJson)
                 .settings.selectedGameplayRuleset == "beatoraja",
         "duplicate preserves the per-profile ruleset selection");
  expect(readFile(manager.pathsFor(copyId).replayDirectory / "copy-test.brd") ==
             replayBytes,
         "duplicate preserves replay paths and exact file bytes");
  expect(matchingRowCount(manager.pathsFor(copyId).scoresDb,
                          "SELECT COUNT(*) FROM scores WHERE score_source=0") ==
             matchingRowCount(manager.pathsFor(firstId).scoresDb,
                              "SELECT COUNT(*) FROM scores WHERE "
                              "score_source=0"),
         "duplicate snapshots local score data");
  expect(!std::filesystem::exists(
             manager.pathsFor(copyId).irCredentialsJson),
         "duplicate does not copy device-local IR credentials");
  expect(rowCount(manager.pathsFor(firstId).replaysDb, "ir_outbox") == 3,
         "duplication leaves source IR operational rows unchanged");
  expect(rowCount(manager.pathsFor(firstId).replaysDb,
                  "ir_submission_receipts") == 1 &&
             rowCount(manager.pathsFor(firstId).replaysDb,
                      "ir_remote_scores") == 1 &&
             matchingRowCount(manager.pathsFor(firstId).scoresDb,
                              "SELECT COUNT(*) FROM scores WHERE "
                              "score_source=1") == 1,
         "duplication leaves source account-scoped IR data unchanged");
  expect(rowCount(manager.pathsFor(copyId).replaysDb, "ir_outbox") == 0,
         "duplicate starts with no device-local IR operational rows");
  expect(rowCount(manager.pathsFor(copyId).replaysDb,
                  "ir_submission_receipts") == 0 &&
             rowCount(manager.pathsFor(copyId).replaysDb, "ir_remote_scores") ==
                 0 &&
             matchingRowCount(manager.pathsFor(copyId).scoresDb,
                              "SELECT COUNT(*) FROM scores WHERE "
                              "score_source=1") == 0,
         "duplicate starts with no account-scoped IR data");
  expect(!directoryContains(manager.pathsFor(copyId).root,
                            "sentinel-api-key"),
         "duplicate contains no credential bytes in any profile file");

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

void testOptionalOperationalFilesRejectLinksWithoutTouchingTargets() {
  TempDirectory temp("profile-operational-file-safety");
  TempDirectory external("profile-operational-file-external");
  PlayerProfileManager manager(temp.path(), dependenciesFor());
  expect(manager.Initialize().ok(),
         "operational-file safety fixture initializes");
  const std::string profileId = manager.activeProfile().id;
  const PlayerProfilePaths paths = manager.activePaths();

  const std::array<std::pair<std::filesystem::path, std::string_view>, 2>
      operationalFiles = {{{paths.irCredentialsJson, "IR credentials"},
                           {paths.bokutachiCacheJson, "Bokutachi cache"}}};
  for (const auto &[profilePath, label] : operationalFiles) {
    const auto externalTarget = external.path() / profilePath.filename();
    const std::string sentinel = std::string(label) + " external data";
    writeFile(externalTarget, sentinel);

    std::error_code linkError;
    std::filesystem::create_symlink(externalTarget, profilePath, linkError);
    expect(!linkError, std::string(label) + " symlink fixture creates");
    if (!linkError) {
      expect(manager.validateProfile(profileId).error ==
                 ProfileError::IntegrityFailure,
             std::string(label) + " validation rejects a linked file");
      expect(readFile(externalTarget) == sentinel,
             std::string(label) +
                 " rejection leaves its external target untouched");
      std::filesystem::remove(profilePath, linkError);
      expect(!linkError, std::string(label) + " symlink fixture removes");
    }
  }
}

void testPracticeDirectoryLifecycleAndValidation() {
  constexpr std::string_view hash = "0123456789abcdef0123456789abcdef"
                                    "0123456789abcdef0123456789abcdef";
  TempDirectory temp("profile-practice");
  std::vector<std::string> uuids = {
      "11111111-1111-4111-8111-111111111111",
      "22222222-2222-4222-8222-222222222222",
  };
  std::size_t uuidIndex = 0;
  auto dependencies = dependenciesFor();
  dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(), "practice-directory fixture initializes");
  const std::string sourceId = manager.activeProfile().id;
  const PlayerProfilePaths source = manager.activePaths();
  expect(source.practiceDirectory == source.root / "practice" &&
             std::filesystem::is_directory(source.practiceDirectory),
         "profile creation owns an empty practice directory");

  const auto presetPath =
      source.practiceDirectory / (std::string(hash) + ".json");
  writeFile(presetPath, validPracticePresetJson(hash));
  expect(manager.validateProfile(sourceId).ok(),
         "routine validation admits semantic hash-matched practice JSON");
  writeFile(presetPath, "{not-json");
  expect(manager.validateProfile(sourceId).ok() &&
             manager.validateProfileForActivation(sourceId).ok(),
         "malformed optional practice JSON remains structurally valid and "
         "activatable");
  const auto malformedVisibleProfiles = manager.listProfiles();
  expect(std::ranges::find(malformedVisibleProfiles, sourceId,
                           &PlayerProfile::id) !=
             malformedVisibleProfiles.end(),
         "malformed optional practice JSON does not hide its profile");
  writeFile(presetPath, validPracticePresetJson(hash));
  const std::array<std::string_view, 4> sidecarSuffixes = {
      ".tmp", ".bak", ".bak.pending", ".bak.previous"};
  for (const std::string_view suffix : sidecarSuffixes) {
    writeFile(std::filesystem::path(presetPath.string() + std::string(suffix)),
              "bounded transient bytes\n");
  }
  expect(manager.validateProfile(sourceId).ok(),
         "routine validation admits every exact bounded atomic sidecar");
  const auto visibleProfiles = manager.listProfiles();
  expect(std::ranges::find(visibleProfiles, sourceId, &PlayerProfile::id) !=
             visibleProfiles.end(),
         "a profile remains visible while atomic crash sidecars exist");
  const auto unknownSidecar =
      std::filesystem::path(presetPath.string() + ".bak.tmp");
  writeFile(unknownSidecar, "{}\n");
  expect(!manager.validateProfile(sourceId).ok(),
         "practice validation rejects unknown compound sidecar names");
  std::filesystem::remove(unknownSidecar);
  const auto backupPath = std::filesystem::path(presetPath.string() + ".bak");
  writeFile(backupPath, std::string((1U * 1024U * 1024U) + 1U, 'x'));
  expect(!manager.validateProfile(sourceId).ok(),
         "practice validation applies the one MiB bound to backup sidecars");
  writeFile(backupPath, "bounded transient bytes\n");

  const auto duplicate = manager.duplicateProfile(sourceId, "Practice Copy");
  expect(duplicate.ok() && duplicate.profile,
         "profile with practice data duplicates");
  if (duplicate.profile) {
    const auto duplicatePractice =
        manager.pathsFor(duplicate.profile->id).practiceDirectory;
    const auto copied = duplicatePractice / presetPath.filename();
    expect(readFile(copied) == readFile(presetPath),
           "duplication copies validated practice JSON bytes");
    std::vector<std::string> duplicateEntries;
    for (const auto &entry :
         std::filesystem::directory_iterator(duplicatePractice)) {
      duplicateEntries.push_back(entry.path().filename().string());
    }
    expect(duplicateEntries ==
               std::vector<std::string>{presetPath.filename().string()},
           "duplication includes only primary practice JSON, not sidecars");
  }

  std::filesystem::remove(presetPath);
  for (const std::string_view suffix : sidecarSuffixes) {
    std::filesystem::remove(
        std::filesystem::path(presetPath.string() + std::string(suffix)));
  }
  std::filesystem::remove(source.practiceDirectory);
  expect(manager.validateProfile(sourceId).ok(),
         "legacy profile without a practice directory remains valid");

  std::filesystem::create_directory(source.practiceDirectory);
  writeFile(source.practiceDirectory / "not-a-hash.json", "{}\n");
  expect(!manager.validateProfile(sourceId).ok(),
         "practice validation rejects non-hash filenames");
  std::filesystem::remove_all(source.practiceDirectory);

  std::filesystem::create_directory(source.practiceDirectory);
  std::filesystem::create_directory(
      std::filesystem::path(presetPath.string() + ".bak.pending"));
  expect(!manager.validateProfile(sourceId).ok(),
         "practice validation rejects an exact sidecar that is not regular");
  std::filesystem::remove_all(source.practiceDirectory);

  std::filesystem::create_directory(source.practiceDirectory);
  std::filesystem::create_directory(source.practiceDirectory /
                                    (std::string(hash) + ".json"));
  expect(!manager.validateProfile(sourceId).ok(),
         "practice validation rejects non-regular entries");
  std::filesystem::remove_all(source.practiceDirectory);

  std::filesystem::create_directory(source.practiceDirectory);
  writeFile(source.practiceDirectory / (std::string(hash) + ".json"),
            std::string((1U * 1024U * 1024U) + 1U, 'x'));
  expect(!manager.validateProfile(sourceId).ok(),
         "practice validation rejects files larger than one MiB");
  std::filesystem::remove_all(source.practiceDirectory);

  TempDirectory external("profile-practice-external");
  std::error_code linkError;
  std::filesystem::create_directory_symlink(
      external.path(), source.practiceDirectory, linkError);
  expect(!linkError, "practice-directory symlink fixture creates");
  if (!linkError) {
    expect(!manager.validateProfile(sourceId).ok(),
           "practice validation rejects a linked directory");
  }
}

void testReplayDirectoryLifecycleAndValidation() {
  TempDirectory temp("profile-replay-validation");
  TempDirectory external("profile-replay-validation-external");
  PlayerProfileManager manager(temp.path(), dependenciesFor());
  expect(manager.Initialize().ok(), "replay validation test initializes");

  const std::string profileId = manager.activeProfile().id;
  const PlayerProfilePaths paths = manager.activePaths();
  std::error_code error;
  std::filesystem::remove(paths.replayDirectory, error);
  expect(!error, "replay directory is removed for missing-directory fixture");
  expect(manager.validateProfile(profileId).ok() &&
             manager.validateProfileForActivation(profileId).ok(),
         "missing optional replay directory remains valid and activatable");

  error.clear();
  std::filesystem::create_directory_symlink(external.path(),
                                            paths.replayDirectory, error);
  expect(!error, "replay-directory symlink fixture creates");
  if (!error) {
    expect(manager.validateProfile(profileId).error ==
                   ProfileError::IntegrityFailure &&
               manager.validateProfileForActivation(profileId).error ==
                   ProfileError::IntegrityFailure,
           "public validation and activation reject a linked replay "
           "directory");
    error.clear();
    std::filesystem::remove(paths.replayDirectory, error);
    expect(!error, "replay-directory symlink fixture is removed");
  }

  error.clear();
  std::filesystem::create_directory(paths.replayDirectory, error);
  expect(!error, "replay directory is recreated for child-link fixture");
  const auto externalReplay = external.path() / "outside.brd";
  writeFile(externalReplay, "external replay\n");
  error.clear();
  std::filesystem::create_symlink(externalReplay,
                                  paths.replayDirectory / "linked.brd", error);
  expect(!error, "replay child symlink fixture creates");
  if (!error) {
    expect(manager.validateProfile(profileId).error ==
                   ProfileError::IntegrityFailure &&
               manager.validateProfileForActivation(profileId).error ==
                   ProfileError::IntegrityFailure,
           "public validation and activation reject a linked replay child");
    expect(readFile(externalReplay) == "external replay\n",
           "replay child validation leaves the link target untouched");
  }
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
  testFailedCreateCommitSyncLeavesNoVisibleProfile();
  testFailedDuplicateCommitSyncLeavesNoVisibleProfile();
  testCreateAndDuplicateFaultMatrixHasFilesystemDerivedOutcomes();
  testProfileDeletionFaultMatrixHasFilesystemDerivedOutcomes();
  testProfilesRootSymlinkNeverEscapesApplicationRoot();
  testPartialTombstoneCleanupNeverRestoresOrExposesProfile();
  testFailedDeleteCommitSyncRestoresProfile();
  testFutureLegacyDatabaseVersionFailsClosedBeforeMigration();
  testSupportedOlderInactiveProfileUsesManagePolicy();
  testSupportedOlderDeleteRollbackRestoresManageableSource();
  testFutureDatabaseProfileIsNeverManageable();
  testSupportedOlderActiveProfileWaitsForSchemaOwners();
  testDuplicateMigratesCompactSchema13AndPreservesRows();
  testDuplicateHoldsProfileActivityExclusionAcrossSnapshotAndFiles();
  testDuplicateRejectsReferencedReplayChecksumMismatch();
  testDuplicatePreservesDeletedReferencedReplayAsMissing();
  testUnsupportedPreV10ReplayProfileFailsActivationPreflight();
  testProfileCrudConstraintsAndDataIsolation();
  testOptionalOperationalFilesRejectLinksWithoutTouchingTargets();
  testPracticeDirectoryLifecycleAndValidation();
  testReplayDirectoryLifecycleAndValidation();
  testFutureVersionsFailClosed();

  if (failures != 0) {
    std::cerr << failures << " player profile test(s) failed.\n";
    return 1;
  }
  std::cout << "Player profile manager tests passed.\n";
  return 0;
}
