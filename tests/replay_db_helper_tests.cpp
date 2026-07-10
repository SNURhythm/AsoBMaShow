#include "../src/ReplayDBHelper.h"
#include "../src/ScoreProvenance.h"
#include "../src/SqliteRAII.h"
#include "../src/Utils.h"
#include "../src/targets.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#if !TARGET_OS_WINDOWS
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr const char *kLegacyProvenanceJson =
    "{\"schemaVersion\":1,\"ruleset\":{\"version\":0},\"stages\":[],"
    "\"eligibility\":\"legacy-unverified\"}";

void execOrAbort(sqlite3 *db, const std::string &sql) {
  char *error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    std::cerr << "exec failed: " << (error != nullptr ? error : "") << "\n"
              << sql << std::endl;
    sqlite3_free(error);
    std::abort();
  }
}

SqliteConnectionHandle openDatabase(const std::filesystem::path &path) {
  std::filesystem::create_directories(path.parent_path());
  sqlite3 *db = nullptr;
  assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
  return SqliteConnectionHandle(db);
}

int queryInt(sqlite3 *db, const std::string &sql) {
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(db, sql, stmt) == SQLITE_OK);
  assert(sqlite3_step(stmt.get()) == SQLITE_ROW);
  return sqlite3_column_int(stmt.get(), 0);
}

std::string queryText(sqlite3 *db, const std::string &sql) {
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(db, sql, stmt) == SQLITE_OK);
  assert(sqlite3_step(stmt.get()) == SQLITE_ROW);
  const auto *text = sqlite3_column_text(stmt.get(), 0);
  return text == nullptr ? std::string{}
                         : std::string(reinterpret_cast<const char *>(text));
}

bool tableExists(sqlite3 *db, const std::string &table) {
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(
             db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
             stmt) == SQLITE_OK);
  sqlite3_bind_text(stmt.get(), 1, table.c_str(), -1, SQLITE_TRANSIENT);
  return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

bool columnExists(sqlite3 *db, const std::string &table,
                  const std::string &column) {
  SqliteStatementHandle stmt;
  const std::string sql = "PRAGMA table_info(\"" + table + "\")";
  assert(prepareSqliteStatement(db, sql, stmt) == SQLITE_OK);
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *name = sqlite3_column_text(stmt.get(), 1);
    if (name != nullptr && column == reinterpret_cast<const char *>(name)) {
      return true;
    }
  }
  return false;
}

std::string schemaSnapshot(sqlite3 *db) {
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(
             db,
             "SELECT type || ':' || name || ':' || COALESCE(sql, '') "
             "FROM sqlite_master ORDER BY type, name",
             stmt) == SQLITE_OK);
  std::string result;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *text = sqlite3_column_text(stmt.get(), 0);
    if (text != nullptr) {
      result += reinterpret_cast<const char *>(text);
    }
    result.push_back('\n');
  }
  return result;
}

std::string readFileBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

struct PersistentDatabaseSnapshot {
  int userVersion = 0;
  std::string journalMode;
  std::string schema;
  std::string sentinel;
  std::string bytes;
  bool journalSidecar = false;
  bool walSidecar = false;
  bool shmSidecar = false;

  bool operator==(const PersistentDatabaseSnapshot &) const = default;
};

PersistentDatabaseSnapshot
persistentDatabaseSnapshot(const std::filesystem::path &path) {
  auto db = openDatabase(path);
  PersistentDatabaseSnapshot result;
  result.userVersion = queryInt(db.get(), "PRAGMA user_version");
  result.journalMode = queryText(db.get(), "PRAGMA journal_mode");
  result.schema = schemaSnapshot(db.get());
  result.sentinel = queryText(db.get(), "SELECT value FROM sentinel");
  db.reset();
  result.bytes = readFileBytes(path);
  result.journalSidecar = std::filesystem::exists(path.string() + "-journal");
  result.walSidecar = std::filesystem::exists(path.string() + "-wal");
  result.shmSidecar = std::filesystem::exists(path.string() + "-shm");
  return result;
}

void createFutureSentinelDatabase(const std::filesystem::path &path) {
  auto db = openDatabase(path);
  execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT)");
  execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('unchanged')");
  execOrAbort(db.get(), "PRAGMA user_version = 99");
}

struct RawDatabaseFamilySnapshot {
  std::array<std::optional<std::string>, 4> files;

  bool operator==(const RawDatabaseFamilySnapshot &) const = default;
};

RawDatabaseFamilySnapshot
rawDatabaseFamilySnapshot(const std::filesystem::path &path) {
  constexpr std::array<const char *, 4> suffixes = {"", "-journal", "-wal",
                                                    "-shm"};
  RawDatabaseFamilySnapshot snapshot;
  for (std::size_t i = 0; i < suffixes.size(); ++i) {
    const std::filesystem::path familyPath = path.string() + suffixes[i];
    std::error_code existsError;
    const bool exists = std::filesystem::exists(familyPath, existsError);
    assert(!existsError);
    if (exists) {
      snapshot.files[i] = readFileBytes(familyPath);
    }
  }
  return snapshot;
}

enum class FutureDatabaseState { Delete, WalAfterExit, HotJournal };

const char *futureDatabaseStateName(FutureDatabaseState state) {
  switch (state) {
  case FutureDatabaseState::Delete:
    return "delete";
  case FutureDatabaseState::WalAfterExit:
    return "wal-after-exit";
  case FutureDatabaseState::HotJournal:
    return "hot-journal";
  }
  return "unknown";
}

#if !TARGET_OS_WINDOWS
[[noreturn]] void executeChildSqlAndExit(const std::filesystem::path &path,
                                         const char *sql,
                                         bool flushCache = false) {
  sqlite3 *db = nullptr;
  if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK || db == nullptr) {
    _exit(10);
  }
  char *error = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    sqlite3_free(error);
    _exit(11);
  }
  if (flushCache && sqlite3_db_cacheflush(db) != SQLITE_OK) {
    _exit(12);
  }
  _exit(0);
}

void waitForSuccessfulChild(pid_t child) {
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status));
  assert(WEXITSTATUS(status) == 0);
}

void createFutureDatabaseState(const std::filesystem::path &path,
                               FutureDatabaseState state) {
  std::filesystem::create_directories(path.parent_path());
  if (state == FutureDatabaseState::Delete) {
    createFutureSentinelDatabase(path);
    return;
  }
  if (state == FutureDatabaseState::HotJournal) {
    createFutureSentinelDatabase(path);
  }

  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    if (state == FutureDatabaseState::WalAfterExit) {
      executeChildSqlAndExit(
          path, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
                "CREATE TABLE sentinel(value TEXT);"
                "INSERT INTO sentinel VALUES ('unchanged');"
                "PRAGMA user_version=99;");
    }
    executeChildSqlAndExit(
        path,
        "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
        "PRAGMA cache_size=1; BEGIN IMMEDIATE; PRAGMA user_version=98;"
        "CREATE TABLE uncommitted(payload BLOB);"
        "WITH RECURSIVE counter(value) AS (VALUES(1) UNION ALL "
        "SELECT value + 1 FROM counter WHERE value < 200) "
        "INSERT INTO uncommitted SELECT randomblob(4096) FROM counter;",
        true);
  }
  waitForSuccessfulChild(child);

  if (state == FutureDatabaseState::WalAfterExit) {
    assert(std::filesystem::exists(path.string() + "-wal"));
    assert(std::filesystem::exists(path.string() + "-shm"));
  } else {
    assert(std::filesystem::exists(path.string() + "-journal"));
    assert(std::filesystem::file_size(path.string() + "-journal") > 0);
  }
}

void createWalDatabaseWithVersion(const std::filesystem::path &path,
                                  int userVersion) {
  std::filesystem::create_directories(path.parent_path());
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    const std::string sql =
        "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
        "CREATE TABLE sentinel(value TEXT);"
        "INSERT INTO sentinel VALUES('unchanged');"
        "PRAGMA user_version=" +
        std::to_string(userVersion);
    executeChildSqlAndExit(path, sql.c_str());
  }
  waitForSuccessfulChild(child);
  assert(std::filesystem::exists(path.string() + "-wal"));
  assert(std::filesystem::exists(path.string() + "-shm"));
}

void createLargeFutureWalDatabase(const std::filesystem::path &path) {
  {
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE padding(payload BLOB)");
    execOrAbort(db.get(),
                "WITH RECURSIVE counter(value) AS (VALUES(1) UNION ALL "
                "SELECT value + 1 FROM counter WHERE value < 512) "
                "INSERT INTO padding SELECT randomblob(4096) FROM counter");
  }
  assert(std::filesystem::file_size(path) > 2 * 1024 * 1024);

  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    executeChildSqlAndExit(
        path, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
              "PRAGMA user_version=99;");
  }
  waitForSuccessfulChild(child);
  assert(std::filesystem::exists(path.string() + "-wal"));
}

template <typename Operation>
void withLiveWalWriter(const std::filesystem::path &path, int userVersion,
                       const Operation &operation) {
  std::filesystem::create_directories(path.parent_path());
  int readyPipe[2] = {-1, -1};
  int releasePipe[2] = {-1, -1};
  assert(pipe(readyPipe) == 0);
  assert(pipe(releasePipe) == 0);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(readyPipe[0]);
    close(releasePipe[1]);
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK ||
        db == nullptr) {
      _exit(20);
    }
    const std::string setup =
        "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
        "CREATE TABLE sentinel(value TEXT);"
        "INSERT INTO sentinel VALUES ('unchanged');"
        "PRAGMA user_version=" +
        std::to_string(userVersion) +
        "; BEGIN IMMEDIATE; UPDATE sentinel SET value='uncommitted';";
    char *error = nullptr;
    if (sqlite3_exec(db, setup.c_str(), nullptr, nullptr, &error) !=
        SQLITE_OK) {
      sqlite3_free(error);
      _exit(21);
    }
    const char ready = 'R';
    if (write(readyPipe[1], &ready, 1) != 1) {
      _exit(22);
    }
    char release = 0;
    if (read(releasePipe[0], &release, 1) != 1) {
      _exit(23);
    }
    _exit(0);
  }

  close(readyPipe[1]);
  close(releasePipe[0]);
  char ready = 0;
  assert(read(readyPipe[0], &ready, 1) == 1);
  assert(ready == 'R');
  close(readyPipe[0]);
  operation();
  const char release = 'X';
  assert(write(releasePipe[1], &release, 1) == 1);
  close(releasePipe[1]);
  waitForSuccessfulChild(child);
}

bool anotherProcessHoldsWalReadLock(int shmFd) {
  struct flock lock{};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  // SQLite's unix VFS reserves bytes 120-127 for WAL-index locks. Reader
  // marks occupy offsets 3-7 within that range.
  lock.l_start = 120 + 3;
  lock.l_len = 5;
  if (fcntl(shmFd, F_GETLK, &lock) == -1) {
    return false;
  }
  return lock.l_type != F_UNLCK;
}

template <typename Operation>
auto withMutationDuringLongWalRead(const std::filesystem::path &path,
                                   const std::string &mutationSql,
                                   const Operation &operation) {
  int readyPipe[2] = {-1, -1};
  assert(pipe(readyPipe) == 0);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(readyPipe[0]);
    const char ready = 'R';
    if (write(readyPipe[1], &ready, 1) != 1) {
      _exit(30);
    }
    close(readyPipe[1]);

    int shmFd = -1;
    int continuouslyLocked = 0;
    for (int attempt = 0; attempt < 80000; ++attempt) {
      if (shmFd == -1) {
        shmFd = open((path.string() + "-shm").c_str(), O_RDWR);
      }
      if (shmFd != -1 && anotherProcessHoldsWalReadLock(shmFd)) {
        ++continuouslyLocked;
        if (continuouslyLocked >= 20) {
          break;
        }
      } else {
        continuouslyLocked = 0;
      }
      usleep(250);
    }
    if (shmFd != -1) {
      close(shmFd);
    }
    if (continuouslyLocked < 20) {
      _exit(31);
    }

    sqlite3 *rawDb = nullptr;
    if (sqlite3_open(path.string().c_str(), &rawDb) != SQLITE_OK ||
        rawDb == nullptr) {
      _exit(32);
    }
    SqliteConnectionHandle writer(rawDb);
    sqlite3_busy_timeout(writer.get(), 5000);
    const std::string transaction =
        "BEGIN IMMEDIATE;" + mutationSql + ";COMMIT;";
    char *error = nullptr;
    if (sqlite3_exec(writer.get(), transaction.c_str(), nullptr, nullptr,
                     &error) != SQLITE_OK) {
      sqlite3_free(error);
      _exit(33);
    }
    _exit(0);
  }

  close(readyPipe[1]);
  char ready = 0;
  assert(read(readyPipe[0], &ready, 1) == 1);
  assert(ready == 'R');
  close(readyPipe[0]);
  auto result = operation();
  waitForSuccessfulChild(child);
  return result;
}
#endif

class ScopedLogCapture {
public:
  ScopedLogCapture() {
    SDL_LogGetOutputFunction(&previous_, &previousUserdata_);
    SDL_LogSetOutputFunction(capture, this);
  }

  ScopedLogCapture(const ScopedLogCapture &) = delete;
  ScopedLogCapture &operator=(const ScopedLogCapture &) = delete;

  ~ScopedLogCapture() {
    SDL_LogSetOutputFunction(previous_, previousUserdata_);
  }

  [[nodiscard]] int countContaining(const std::string &needle) const {
    return static_cast<int>(
        std::count_if(messages.begin(), messages.end(), [&](const auto &value) {
          return value.find(needle) != std::string::npos;
        }));
  }

  [[nodiscard]] bool anyContains(const std::string &needle) const {
    return countContaining(needle) > 0;
  }

  std::vector<std::string> messages;

private:
  static void SDLCALL capture(void *userdata, int, SDL_LogPriority,
                              const char *message) {
    auto *self = static_cast<ScopedLogCapture *>(userdata);
    self->messages.emplace_back(message != nullptr ? message : "");
  }

  SDL_LogOutputFunction previous_ = nullptr;
  void *previousUserdata_ = nullptr;
};

void createVersion2ReplayFixture(const std::filesystem::path &path) {
  auto db = openDatabase(path);
  execOrAbort(
      db.get(),
      "CREATE TABLE replays ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, chart_path TEXT, chart_md5 TEXT,"
      "chart_sha256 TEXT, chart_title TEXT, chart_artist TEXT,"
      "ln_mode INTEGER NOT NULL DEFAULT 0, gauge_type INTEGER NOT NULL,"
      "gauge_auto_shift INTEGER NOT NULL, final_score INTEGER NOT NULL,"
      "max_combo INTEGER NOT NULL DEFAULT 0, final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL, random_seed INTEGER, random_prng TEXT,"
      "random_values TEXT, play_option TEXT, play_option_seed INTEGER,"
      "play_option2 TEXT, play_option2_seed INTEGER, assist_option TEXT,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
  execOrAbort(
      db.get(),
      "CREATE TABLE replay_events ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, replay_id INTEGER NOT NULL,"
      "event_index INTEGER NOT NULL, action INTEGER NOT NULL, lane INTEGER NOT "
      "NULL,"
      "note_time_micros INTEGER NOT NULL, song_time_micros INTEGER NOT NULL,"
      "judge_time_micros INTEGER NOT NULL, judgement INTEGER NOT NULL,"
      "diff_micros INTEGER NOT NULL, gauge REAL NOT NULL,"
      "gauge_type INTEGER NOT NULL, combo INTEGER NOT NULL, score INTEGER NOT "
      "NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE)");
  execOrAbort(
      db.get(),
      "CREATE TABLE replay_touch_samples ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, replay_id INTEGER NOT NULL,"
      "sample_index INTEGER NOT NULL, action INTEGER NOT NULL,"
      "finger_id INTEGER NOT NULL, song_time_micros INTEGER NOT NULL,"
      "x REAL NOT NULL, y REAL NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE)");
  execOrAbort(
      db.get(),
      "CREATE TABLE replay_lane_cover_events ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, replay_id INTEGER NOT NULL,"
      "event_index INTEGER NOT NULL, song_time_micros INTEGER NOT NULL,"
      "note_start_position_percent INTEGER NOT NULL,"
      "reset_visible_time_reference INTEGER NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE)");
  execOrAbort(
      db.get(),
      "CREATE TABLE course_replays ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, course_id INTEGER NOT NULL,"
      "course_name TEXT, course_group_name TEXT, constraint_json TEXT,"
      "gauge_type INTEGER NOT NULL, gauge_profile INTEGER NOT NULL DEFAULT 0,"
      "gauge_auto_shift INTEGER NOT NULL, ln_mode INTEGER NOT NULL DEFAULT 0,"
      "requested_play_option TEXT, assist_option TEXT, final_score INTEGER NOT "
      "NULL,"
      "max_combo INTEGER NOT NULL DEFAULT 0, final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL, completed_charts INTEGER NOT NULL,"
      "total_charts INTEGER NOT NULL,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
  execOrAbort(
      db.get(),
      "CREATE TABLE course_replay_stages ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, course_replay_id INTEGER NOT NULL,"
      "stage_index INTEGER NOT NULL, replay_id INTEGER NOT NULL,"
      "rest_micros_after_stage INTEGER NOT NULL DEFAULT 0,"
      "FOREIGN KEY(course_replay_id) REFERENCES course_replays(id) ON DELETE "
      "CASCADE,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE)");
  execOrAbort(
      db.get(),
      "INSERT INTO replays (chart_path, chart_md5, chart_sha256, chart_title,"
      "chart_artist, ln_mode, gauge_type, gauge_auto_shift, final_score,"
      "max_combo, final_gauge, clear_type, play_option, assist_option, "
      "created_at)"
      " VALUES ('BMS/legacy.bms','legacy-md5','legacy-sha','Legacy','Artist',"
      "2,2,1,1234,456,73.5,300,'MIRROR','OFF','2026-01-02 03:04:05')");
  execOrAbort(
      db.get(),
      "INSERT INTO replays (chart_path, chart_md5, chart_sha256, chart_title,"
      "chart_artist, ln_mode, gauge_type, gauge_auto_shift, final_score,"
      "max_combo, final_gauge, clear_type, play_option, assist_option, "
      "created_at)"
      " VALUES ('BMS/stage.bms','stage-md5','stage-sha','Stage','Artist',"
      "1,1,0,777,88,55.5,200,'NORMAL','OFF','2026-02-03 04:05:06')");
  execOrAbort(db.get(),
              "INSERT INTO replay_events (replay_id,event_index,action,lane,"
              "note_time_micros,song_time_micros,judge_time_micros,judgement,"
              "diff_micros,gauge,gauge_type,combo,score)"
              " VALUES (1,0,0,3,100000,100100,100050,0,-50,73.5,2,1,2)");
  execOrAbort(
      db.get(),
      "INSERT INTO course_replays (course_id,course_name,course_group_name,"
      "constraint_json,gauge_type,gauge_profile,gauge_auto_shift,ln_mode,"
      "requested_play_option,assist_option,final_score,max_combo,final_gauge,"
      "clear_type,completed_charts,total_charts,created_at)"
      " VALUES (17,'Legacy Course','Group','{}',2,1,1,2,'MIRROR','OFF',"
      "2345,321,61.25,200,1,2,'2026-03-04 05:06:07')");
  execOrAbort(db.get(),
              "INSERT INTO course_replay_stages "
              "(course_replay_id,stage_index,replay_id,rest_micros_after_stage)"
              " VALUES (1,0,2,250000)");
  execOrAbort(db.get(), "PRAGMA user_version = 2");
}

std::string replayOutcome(sqlite3 *db) {
  return queryText(
      db,
      "SELECT final_score || '|' || max_combo || '|' || final_gauge || '|' || "
      "clear_type || '|' || created_at FROM replays WHERE id=1");
}

std::string courseOutcome(sqlite3 *db) {
  return queryText(
      db,
      "SELECT final_score || '|' || max_combo || '|' || final_gauge || '|' || "
      "clear_type || '|' || completed_charts || '|' || total_charts || '|' || "
      "created_at FROM course_replays WHERE id=1");
}

ScoreProvenance sampleProvenance(const std::string &hash) {
  ScoreProvenance value;
  value.ruleset = RulesetDescriptor::Current();
  value.stages = {{
      .chartMd5 = "md5-" + hash,
      .chartSha256 = "sha-" + hash,
      .longNoteMode = 2,
      .judgeRankSource = JudgeRankSource::Chart,
      .sourceJudgeRank = 2,
      .effectiveJudgeWindows = {{PGreat, -10000, 10000},
                                {Great, -30000, 30000}},
  }};
  value.gaugeType = GaugeType::Hard;
  value.player1 = {.option = "RANDOM", .seed = 1234};
  value.inputDevices = {InputDeviceCategory::Keyboard};
  value.eligibility = ScoreEligibility::Verified;
  return value;
}

ReplayData sampleReplay(const std::filesystem::path &root,
                        const std::string &hash) {
  ReplayData replay;
  replay.chartMeta.BmsPath = root / "BMS" / (hash + ".bms");
  replay.chartMeta.MD5 = "md5-" + hash;
  replay.chartMeta.SHA256 = "sha-" + hash;
  replay.chartMeta.Title = "Title " + hash;
  replay.chartMeta.Artist = "Artist";
  replay.chartMeta.TotalNotes = 50;
  replay.chartMeta.LnMode = 2;
  replay.initialGaugeType = GaugeType::Hard;
  replay.gaugeAutoShift = true;
  replay.finalScore = 91;
  replay.maxCombo = 45;
  replay.finalGauge = 82.5f;
  replay.clearType = kClearTypeHardClearRank;
  replay.playOption = "RANDOM";
  replay.playOptionSeed = 1234;
  replay.events.push_back({.action = ReplayEventAction::Press,
                           .lane = 3,
                           .noteTimeMicros = 100000,
                           .songTimeMicros = 100100,
                           .judgeTimeMicros = 100050,
                           .judgement = PGreat,
                           .diffMicros = -50,
                           .gauge = 82.5f,
                           .gaugeType = GaugeType::Hard,
                           .combo = 1,
                           .score = 2});
  replay.provenance = sampleProvenance(hash);
  return replay;
}

ScoreProvenance readStoredProvenance(sqlite3 *db, const std::string &table,
                                     int id) {
  const std::string json =
      queryText(db, "SELECT provenance_json FROM " + table +
                        " WHERE id=" + std::to_string(id));
  std::string error;
  const auto value = deserializeScoreProvenance(json, error);
  assert(value.has_value());
  assert(error.empty());
  return *value;
}

void testVersion2MigrationPreservesOutcomesAndRows(
    const std::filesystem::path &root) {
  const auto path = root / "migration" / "replay.db";
  createVersion2ReplayFixture(path);
  auto before = openDatabase(path);
  const std::string replayBefore = replayOutcome(before.get());
  const std::string courseBefore = courseOutcome(before.get());
  before.reset();

  ReplayDBHelper helper(path);
  assert(helper.GetDatabasePath() == path);
  assert(helper.EnsureSchema());

  auto migrated = openDatabase(path);
  assert(queryInt(migrated.get(), "PRAGMA user_version") == 3);
  assert(queryInt(migrated.get(), "SELECT COUNT(*) FROM replays") == 2);
  assert(queryInt(migrated.get(), "SELECT COUNT(*) FROM course_replays") == 1);
  assert(queryInt(migrated.get(), "SELECT COUNT(*) FROM replay_events") == 1);
  assert(queryInt(migrated.get(),
                  "SELECT COUNT(*) FROM course_replay_stages") == 1);
  assert(replayOutcome(migrated.get()) == replayBefore);
  assert(courseOutcome(migrated.get()) == courseBefore);
  for (const std::string table : {"replays", "course_replays"}) {
    assert(columnExists(migrated.get(), table, "ruleset_version"));
    assert(columnExists(migrated.get(), table, "eligibility"));
    assert(columnExists(migrated.get(), table, "provenance_json"));
    assert(queryInt(migrated.get(), "SELECT ruleset_version FROM " + table +
                                        " WHERE id=1") == 0);
    assert(queryInt(migrated.get(),
                    "SELECT eligibility FROM " + table + " WHERE id=1") ==
           static_cast<int>(ScoreEligibility::LegacyUnverified));
    assert(readStoredProvenance(migrated.get(), table, 1) ==
           ScoreProvenance::Legacy());
    assert(queryText(migrated.get(),
                     "SELECT provenance_json FROM " + table + " WHERE id=1") ==
           kLegacyProvenanceJson);
  }
  const std::string firstSchema = schemaSnapshot(migrated.get());
  migrated.reset();

  assert(helper.EnsureSchema());
  auto second = openDatabase(path);
  assert(schemaSnapshot(second.get()) == firstSchema);
  assert(queryInt(second.get(), "SELECT COUNT(*) FROM replays") == 2);
  assert(queryInt(second.get(), "SELECT COUNT(*) FROM course_replays") == 1);
}

void testChartAndCourseRoundTripAndPathIsolation(
    const std::filesystem::path &root) {
  const auto firstPath = root / "profiles" / "one" / "replay.db";
  const auto secondPath = root / "profiles" / "two" / "replay.db";
  ReplayDBHelper first(firstPath);
  ReplayDBHelper second(secondPath);
  assert(first.EnsureSchema());
  assert(second.EnsureSchema());

  ReplayData replay = sampleReplay(root, "chart");
  const auto replayId = first.SaveReplay(replay);
  assert(replayId.has_value());
  const auto loaded = first.LoadReplay(*replayId, replay.chartMeta);
  assert(loaded.has_value());
  assert(loaded->provenance == replay.provenance);
  const auto summaries = first.ListReplays(replay.chartMeta, 0);
  assert(summaries.size() == 1);
  assert(summaries.front().rulesetVersion == 1);
  assert(summaries.front().eligibility == ScoreEligibility::Verified);

  CourseReplayData course;
  course.courseId = 31;
  course.courseName = "Course";
  course.courseGroupName = "Group";
  course.constraintJson = "{}";
  course.finalScore = replay.finalScore;
  course.maxCombo = replay.maxCombo;
  course.finalGauge = replay.finalGauge;
  course.clearType = replay.clearType;
  course.completedCharts = 1;
  course.totalCharts = 1;
  course.provenance = replay.provenance;
  course.stages.push_back(
      {.replay = sampleReplay(root, "stage"), .restMicrosAfterStage = 250000});
  const auto courseId = first.SaveCourseReplay(course);
  assert(courseId.has_value());
  const auto loadedCourse = first.LoadCourseReplay(*courseId);
  assert(loadedCourse.has_value());
  assert(loadedCourse->provenance == course.provenance);
  assert(loadedCourse->stages.size() == 1);
  assert(loadedCourse->stages.front().replay.provenance ==
         course.stages.front().replay.provenance);
  const auto courseSummaries = first.ListCourseReplays(course.courseId, 0);
  assert(courseSummaries.size() == 1);
  assert(courseSummaries.front().rulesetVersion == 1);
  assert(courseSummaries.front().eligibility == ScoreEligibility::Verified);

  assert(second.ListReplays(replay.chartMeta, 0).empty());
  assert(second.ListCourseReplays(course.courseId, 0).empty());
  assert(first.GetDatabasePath() == firstPath);
  assert(second.GetDatabasePath() == secondPath);

  ReplayDBHelper retargetable;
  retargetable.SetDatabasePath(firstPath);
  assert(retargetable.GetDatabasePath() == firstPath);
}

void testInvalidNewProvenanceFailsLoad(const std::filesystem::path &root) {
  const auto path = root / "invalid" / "replay.db";
  ReplayDBHelper helper(path);
  assert(helper.EnsureSchema());
  ReplayData replay = sampleReplay(root, "invalid");
  const auto replayId = helper.SaveReplay(replay);
  assert(replayId.has_value());

  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE replays SET provenance_json='{' WHERE id=" +
                            std::to_string(*replayId));
  db.reset();
  assert(!helper.LoadReplay(*replayId, replay.chartMeta).has_value());

  CourseReplayData course;
  course.courseId = 44;
  course.provenance = replay.provenance;
  course.stages.push_back({.replay = sampleReplay(root, "invalid-stage")});
  const auto courseId = helper.SaveCourseReplay(course);
  assert(courseId.has_value());
  db = openDatabase(path);
  execOrAbort(db.get(),
              "UPDATE course_replays SET provenance_json='{' WHERE id=" +
                  std::to_string(*courseId));
  db.reset();
  assert(!helper.LoadCourseReplay(*courseId).has_value());
}

void testInvalidProvenanceRejectsReplayWrites(
    const std::filesystem::path &root) {
  const auto path = root / "invalid-provenance-write" / "replay.db";
  ReplayDBHelper helper(path);
  assert(helper.EnsureSchema());

  const auto assertRejectedWithoutRows = [&](const ScoreProvenance &value,
                                             const std::string &label) {
    ReplayData chartReplay = sampleReplay(root, "invalid-chart-" + label);
    chartReplay.provenance = value;
    assert(!helper.SaveReplay(chartReplay).has_value());

    CourseReplayData invalidCourse;
    invalidCourse.courseId = 60;
    invalidCourse.courseName = "Invalid course provenance";
    invalidCourse.provenance = value;
    invalidCourse.stages.push_back(
        {.replay = sampleReplay(root, "valid-stage-" + label)});
    assert(!helper.SaveCourseReplay(invalidCourse).has_value());

    CourseReplayData invalidStage;
    invalidStage.courseId = 61;
    invalidStage.courseName = "Invalid stage provenance";
    invalidStage.provenance = sampleProvenance("valid-course-" + label);
    invalidStage.stages.push_back(
        {.replay = sampleReplay(root, "invalid-stage-" + label)});
    invalidStage.stages.front().replay.provenance = value;
    assert(!helper.SaveCourseReplay(invalidStage).has_value());

    auto db = openDatabase(path);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == 0);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_replays") == 0);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_replay_stages") ==
           0);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM replay_events") == 0);
  };

  auto futureSchema = sampleProvenance("future-schema");
  futureSchema.schemaVersion = ScoreProvenance::kSchemaVersion + 1;
  assertRejectedWithoutRows(futureSchema, "future-schema");

  auto futureRuleset = sampleProvenance("future-ruleset");
  futureRuleset.ruleset.version = RulesetDescriptor::kCurrentVersion + 1;
  assertRejectedWithoutRows(futureRuleset, "future-ruleset");

  auto invalidEnum = sampleProvenance("invalid-enum");
  invalidEnum.eligibility = static_cast<ScoreEligibility>(999);
  assertRejectedWithoutRows(invalidEnum, "invalid-enum");
}

void testChartSummariesOmitInvalidProvenanceAndCountValidLimit(
    const std::filesystem::path &root) {
  const auto path = root / "invalid-chart-summaries" / "replay.db";
  ReplayDBHelper helper(path);
  assert(helper.EnsureSchema());

  ReplayData replay = sampleReplay(root, "summary-chart");
  std::vector<int> replayIds;
  for (int i = 0; i < 4; ++i) {
    replay.finalScore = 100 + i;
    const auto id = helper.SaveReplay(replay);
    assert(id.has_value());
    replayIds.push_back(*id);
  }

  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE replays SET ruleset_version=99 WHERE id=" +
                            std::to_string(replayIds[2]));
  execOrAbort(db.get(), "UPDATE replays SET provenance_json='{' WHERE id=" +
                            std::to_string(replayIds[3]));
  db.reset();

  const auto limited = helper.ListReplays(replay.chartMeta, 2);
  assert(limited.size() == 2);
  assert(limited[0].id == replayIds[1]);
  assert(limited[1].id == replayIds[0]);

  const auto all = helper.ListReplays(replay.chartMeta, 0);
  assert(all.size() == 2);
  assert(all[0].id == replayIds[1]);
  assert(all[1].id == replayIds[0]);
}

void testCourseSummariesOmitInvalidProvenanceAndCountValidLimit(
    const std::filesystem::path &root) {
  const auto path = root / "invalid-course-summaries" / "replay.db";
  ReplayDBHelper helper(path);
  assert(helper.EnsureSchema());

  CourseReplayData course;
  course.courseId = 62;
  course.courseName = "Summary course";
  course.completedCharts = 1;
  course.totalCharts = 1;
  course.provenance = sampleProvenance("summary-course");
  course.stages.push_back(
      {.replay = sampleReplay(root, "summary-course-stage")});

  std::vector<int> courseReplayIds;
  for (int i = 0; i < 4; ++i) {
    course.finalScore = 200 + i;
    const auto id = helper.SaveCourseReplay(course);
    assert(id.has_value());
    courseReplayIds.push_back(*id);
  }

  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE course_replays SET eligibility=2 WHERE id=" +
                            std::to_string(courseReplayIds[2]));
  execOrAbort(db.get(),
              "UPDATE course_replays SET provenance_json='{' WHERE id=" +
                  std::to_string(courseReplayIds[3]));
  db.reset();

  const auto limited = helper.ListCourseReplays(course.courseId, 2);
  assert(limited.size() == 2);
  assert(limited[0].id == courseReplayIds[1]);
  assert(limited[1].id == courseReplayIds[0]);

  const auto all = helper.ListCourseReplays(course.courseId, 0);
  assert(all.size() == 2);
  assert(all[0].id == courseReplayIds[1]);
  assert(all[1].id == courseReplayIds[0]);
}

void testCourseSummariesOmitInvalidLinkedStages(
    const std::filesystem::path &root) {
  const auto path = root / "invalid-course-summary-stages" / "replay.db";
  ReplayDBHelper helper(path);
  assert(helper.EnsureSchema());

  CourseReplayData course;
  course.courseId = 64;
  course.courseName = "Summary stage course";
  course.completedCharts = 2;
  course.totalCharts = 2;
  course.provenance = sampleProvenance("summary-stage-course");
  course.stages.push_back(
      {.replay = sampleReplay(root, "summary-stage-course-a")});
  course.stages.push_back(
      {.replay = sampleReplay(root, "summary-stage-course-b")});

  std::vector<int> courseReplayIds;
  for (int i = 0; i < 6; ++i) {
    course.finalScore = 300 + i;
    const auto id = helper.SaveCourseReplay(course);
    assert(id.has_value());
    courseReplayIds.push_back(*id);
  }

  auto db = openDatabase(path);
  const int mismatchedStageId = queryInt(
      db.get(),
      "SELECT replay_id FROM course_replay_stages WHERE course_replay_id=" +
          std::to_string(courseReplayIds[1]) + " ORDER BY stage_index LIMIT 1");
  const int malformedStageId = queryInt(
      db.get(),
      "SELECT replay_id FROM course_replay_stages WHERE course_replay_id=" +
          std::to_string(courseReplayIds[2]) +
          " ORDER BY stage_index DESC LIMIT 1");
  execOrAbort(db.get(),
              "DELETE FROM course_replay_stages WHERE course_replay_id=" +
                  std::to_string(courseReplayIds[3]));
  execOrAbort(db.get(),
              "DELETE FROM replays WHERE id IN (SELECT replay_id FROM "
              "course_replay_stages WHERE course_replay_id=" +
                  std::to_string(courseReplayIds[4]) + ")");
  execOrAbort(db.get(),
              "UPDATE course_replay_stages SET stage_index=-1-stage_index "
              "WHERE course_replay_id=" +
                  std::to_string(courseReplayIds[5]));
  execOrAbort(db.get(), "UPDATE replays SET ruleset_version=99 WHERE id=" +
                            std::to_string(mismatchedStageId));
  execOrAbort(db.get(), "UPDATE replays SET provenance_json='{' WHERE id=" +
                            std::to_string(malformedStageId));
  db.reset();

  const auto limited = helper.ListCourseReplays(course.courseId, 1);
  assert(limited.size() == 1);
  assert(limited.front().id == courseReplayIds[0]);
  const auto all = helper.ListCourseReplays(course.courseId, 0);
  assert(all.size() == 1);
  assert(all.front().id == courseReplayIds[0]);
  assert(!helper.LoadCourseReplay(courseReplayIds[1]).has_value());
  assert(!helper.LoadCourseReplay(courseReplayIds[2]).has_value());
  assert(!helper.LoadCourseReplay(courseReplayIds[3]).has_value());
  assert(!helper.LoadCourseReplay(courseReplayIds[4]).has_value());
  assert(!helper.LoadCourseReplay(courseReplayIds[5]).has_value());
}

void testFutureVersionRejectsWithoutSchemaMutation(
    const std::filesystem::path &root) {
  const auto path = root / "future" / "replay.db";
  auto db = openDatabase(path);
  execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT)");
  execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('unchanged')");
  execOrAbort(db.get(), "PRAGMA user_version = 99");
  const std::string before = schemaSnapshot(db.get());
  db.reset();

  ReplayDBHelper helper(path);
  assert(!helper.EnsureSchema());

  auto after = openDatabase(path);
  assert(queryInt(after.get(), "PRAGMA user_version") == 99);
  assert(schemaSnapshot(after.get()) == before);
  assert(queryText(after.get(), "SELECT value FROM sentinel") == "unchanged");
  assert(!tableExists(after.get(), "replays"));
  assert(!tableExists(after.get(), "course_replays"));
}

void testFutureReplayWritesPreservePersistentDatabaseState(
    const std::filesystem::path &root) {
  ReplayData replay = sampleReplay(root, "future-persistent-state");
  CourseReplayData course;
  course.courseId = 63;
  course.courseName = "Future persistent state course";
  course.provenance = replay.provenance;
  course.stages.push_back(
      {.replay = sampleReplay(root, "future-persistent-state-stage")});

  const auto assertUnchanged = [&](const std::filesystem::path &path,
                                   const auto &operation) {
    createFutureSentinelDatabase(path);
    const auto before = persistentDatabaseSnapshot(path);
    operation(path);
    const auto after = persistentDatabaseSnapshot(path);
    assert(after == before);
  };

  assertUnchanged(root / "future-save-replay" / "replay.db",
                  [&](const auto &path) {
                    ReplayDBHelper helper(path);
                    assert(!helper.SaveReplay(replay).has_value());
                  });
  assertUnchanged(root / "future-save-course-replay" / "replay.db",
                  [&](const auto &path) {
                    ReplayDBHelper helper(path);
                    assert(!helper.SaveCourseReplay(course).has_value());
                  });
  assertUnchanged(root / "future-direct-replay" / "replay.db",
                  [&](const auto &path) {
                    ReplayDBHelper helper(path);
                    SqliteConnectionHandle db(helper.Connect());
                    assert(!db);
                  });
}

void testFutureReplayPreflightPreservesRawDatabaseFamily(
    const std::filesystem::path &root) {
#if !TARGET_OS_WINDOWS
  ReplayData replay = sampleReplay(root, "future-raw-preflight");
  CourseReplayData course;
  course.courseId = 66;
  course.courseName = "Future raw preflight course";
  course.provenance = replay.provenance;
  course.stages.push_back(
      {.replay = sampleReplay(root, "future-raw-preflight-stage")});

  const auto assertRejectedWithoutFamilyMutation =
      [&](FutureDatabaseState databaseState, const std::string &operationName,
          const auto &operation) {
        const auto path =
            root / "future-raw-replay" /
            (operationName + "-" + futureDatabaseStateName(databaseState)) /
            "replay.db";
        createFutureDatabaseState(path, databaseState);
        const auto before = rawDatabaseFamilySnapshot(path);
        const bool rejected = operation(path);
        const auto after = rawDatabaseFamilySnapshot(path);
        assert(after == before);
        assert(rejected);
      };

  constexpr std::array states = {FutureDatabaseState::WalAfterExit,
                                 FutureDatabaseState::HotJournal,
                                 FutureDatabaseState::Delete};
  for (const FutureDatabaseState databaseState : states) {
    assertRejectedWithoutFamilyMutation(
        databaseState, "connect", [&](const auto &path) {
          ReplayDBHelper helper(path);
          SqliteConnectionHandle db(helper.Connect());
          return !db;
        });
    assertRejectedWithoutFamilyMutation(
        databaseState, "save-chart", [&](const auto &path) {
          ReplayDBHelper helper(path);
          return !helper.SaveReplay(replay).has_value();
        });
    assertRejectedWithoutFamilyMutation(
        databaseState, "save-course", [&](const auto &path) {
          ReplayDBHelper helper(path);
          return !helper.SaveCourseReplay(course).has_value();
        });
  }

  const auto directPath =
      root / "future-raw-replay" / "direct-live-wal" / "replay.db";
  auto directDb = openDatabase(directPath);
  execOrAbort(directDb.get(), "PRAGMA journal_mode=WAL");
  execOrAbort(directDb.get(), "PRAGMA wal_autocheckpoint=0");
  execOrAbort(directDb.get(), "CREATE TABLE sentinel(value TEXT)");
  execOrAbort(directDb.get(), "INSERT INTO sentinel VALUES ('unchanged')");
  execOrAbort(directDb.get(), "PRAGMA user_version=99");
  const auto directBefore = rawDatabaseFamilySnapshot(directPath);
  ReplayDBHelper directHelper(directPath);
  assert(!directHelper.CreateReplayTables(directDb.get()));
  assert(rawDatabaseFamilySnapshot(directPath) == directBefore);
#else
  (void)root;
#endif
}

void testReplayPreflightRejectsMalformedStatesAndAllowsCreation(
    const std::filesystem::path &root) {
  {
    const auto path = root / "preflight-negative-version" / "replay.db";
    auto db = openDatabase(path);
    execOrAbort(db.get(), "PRAGMA user_version=-1");
    db.reset();
    const auto before = rawDatabaseFamilySnapshot(path);
    ReplayDBHelper helper(path);
    SqliteConnectionHandle connection(helper.Connect());
    assert(!connection);
    assert(rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = root / "preflight-stale-shm" / "replay.db";
    createFutureSentinelDatabase(path);
    std::ofstream staleShm(path.string() + "-shm", std::ios::binary);
    staleShm << "stale-shm-without-wal";
    staleShm.close();
    const auto before = rawDatabaseFamilySnapshot(path);
    ReplayDBHelper helper(path);
    SqliteConnectionHandle connection(helper.Connect());
    assert(!connection);
    assert(rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = root / "preflight-zero-byte" / "replay.db";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream empty(path, std::ios::binary);
    empty.close();
    ReplayDBHelper helper(path);
    assert(helper.EnsureSchema());
    assert(std::filesystem::file_size(path) > 0);
  }

  {
    const auto path = root / "preflight-missing" / "replay.db";
    ReplayDBHelper helper(path);
    assert(helper.EnsureSchema());
    assert(std::filesystem::exists(path));
  }

#if !TARGET_OS_WINDOWS
  {
    const auto path = root / "preflight-live-writer" / "replay.db";
    withLiveWalWriter(path, 3, [&] {
      const auto before = rawDatabaseFamilySnapshot(path);
      ReplayDBHelper helper(path);
      SqliteConnectionHandle connection(helper.Connect());
      const auto after = rawDatabaseFamilySnapshot(path);
      assert(!connection);
      assert(after == before);
    });
  }
#endif
}

int denyJournalModeAuthorizer(void *, int action, const char *first,
                              const char *, const char *, const char *) {
  return action == SQLITE_PRAGMA && first != nullptr &&
                 std::string_view(first) == "journal_mode"
             ? SQLITE_DENY
             : SQLITE_OK;
}

int denyUserVersionAuthorizer(void *, int action, const char *first,
                              const char *, const char *, const char *) {
  return action == SQLITE_PRAGMA && first != nullptr &&
                 std::string_view(first) == "user_version"
             ? SQLITE_DENY
             : SQLITE_OK;
}

int installDenyJournalMode(sqlite3 *db, char **, const sqlite3_api_routines *) {
  return sqlite3_set_authorizer(db, denyJournalModeAuthorizer, nullptr);
}

class ScopedDenyJournalModeAutoExtension {
public:
  ScopedDenyJournalModeAutoExtension() {
    sqlite3_reset_auto_extension();
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(
               installDenyJournalMode)) == SQLITE_OK);
  }
  ~ScopedDenyJournalModeAutoExtension() { sqlite3_reset_auto_extension(); }
};

void writeHeaderShapedCorruptDatabase(const std::filesystem::path &path) {
  std::filesystem::create_directories(path.parent_path());
  std::string bytes(4096, '\0');
  constexpr std::string_view magic("SQLite format 3\0", 16);
  std::copy(magic.begin(), magic.end(), bytes.begin());
  bytes[16] = 0x10;
  bytes[17] = 0;
  bytes[18] = 1;
  bytes[19] = 1;
  std::ofstream output(path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  assert(output);
}

void testReplayOwnedOpenRejectsRecoveryAndConfigurationRaces(
    const std::filesystem::path &root) {
#if !TARGET_OS_WINDOWS
  {
    const auto path = root / "wal-without-shm-supported" / "replay.db";
    createWalDatabaseWithVersion(path, 3);
    assert(std::filesystem::remove(path.string() + "-shm"));
    const auto before = rawDatabaseFamilySnapshot(path);
    ReplayDBHelper helper(path);
    SqliteConnectionHandle connection(helper.Connect());
    assert(connection);
    connection.reset();
    assert(rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = root / "wal-without-shm-future" / "replay.db";
    createWalDatabaseWithVersion(path, 99);
    assert(std::filesystem::remove(path.string() + "-shm"));
    const auto before = rawDatabaseFamilySnapshot(path);
    ReplayDBHelper helper(path);
    SqliteConnectionHandle connection(helper.Connect());
    assert(!connection);
    assert(rawDatabaseFamilySnapshot(path) == before);
  }
#endif

  {
    const auto path = root / "header-shaped-corrupt" / "replay.db";
    writeHeaderShapedCorruptDatabase(path);
    const auto before = rawDatabaseFamilySnapshot(path);
    ReplayDBHelper helper(path);
    SqliteConnectionHandle connection(helper.Connect());
    assert(!connection);
    assert(rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = root / "required-pragma-error" / "replay.db";
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT)");
    execOrAbort(db.get(), "PRAGMA user_version=3");
    db.reset();
    const auto before = rawDatabaseFamilySnapshot(path);
    ScopedDenyJournalModeAutoExtension denyJournalMode;
    ReplayDBHelper helper(path);
    SqliteConnectionHandle connection(helper.Connect());
    assert(!connection);
    assert(rawDatabaseFamilySnapshot(path) == before);
  }
}

void testReplayTransactionalVersionErrorsDoNotMutateSchema(
    const std::filesystem::path &root) {
  {
    const auto path = root / "transaction-negative-version" / "replay.db";
    auto db = openDatabase(path);
    execOrAbort(db.get(), "PRAGMA user_version=-1");
    const std::string beforeSchema = schemaSnapshot(db.get());
    execOrAbort(db.get(), "BEGIN TRANSACTION");
    ReplayDBHelper helper(path);
    assert(!helper.CreateReplayTables(db.get()));
    assert(queryInt(db.get(), "PRAGMA user_version") == -1);
    assert(schemaSnapshot(db.get()) == beforeSchema);
    execOrAbort(db.get(), "ROLLBACK");
  }

  {
    const auto path = root / "transaction-version-read-error" / "replay.db";
    auto db = openDatabase(path);
    const std::string beforeSchema = schemaSnapshot(db.get());
    execOrAbort(db.get(), "BEGIN TRANSACTION");
    assert(sqlite3_set_authorizer(db.get(), denyUserVersionAuthorizer,
                                  nullptr) == SQLITE_OK);
    ReplayDBHelper helper(path);
    assert(!helper.CreateReplayTables(db.get()));
    assert(schemaSnapshot(db.get()) == beforeSchema);
    assert(sqlite3_set_authorizer(db.get(), nullptr, nullptr) == SQLITE_OK);
    execOrAbort(db.get(), "ROLLBACK");
  }
}

void testLargeWalReplayPreflightPreservesFamily(
    const std::filesystem::path &root) {
#if !TARGET_OS_WINDOWS
  const auto path = root / "preflight-large-wal" / "replay.db";
  createLargeFutureWalDatabase(path);
  const auto before = rawDatabaseFamilySnapshot(path);
  ReplayDBHelper helper(path);
  SqliteConnectionHandle connection(helper.Connect());
  assert(!connection);
  assert(rawDatabaseFamilySnapshot(path) == before);
#else
  (void)root;
#endif
}

void insertChartReplays(sqlite3 *db, int count) {
  for (int i = 1; i <= count; ++i) {
    execOrAbort(
        db,
        "INSERT INTO replays (chart_path, chart_md5, chart_sha256, "
        "chart_title, chart_artist, gauge_type, gauge_auto_shift, final_score,"
        "max_combo, final_gauge, clear_type, assist_option) VALUES "
        "('BMS/chart.bms','md5','sha','Title','Artist',0,0," +
            std::to_string(i) + "," + std::to_string(i) + ",100.0,300,'OFF')");
  }
}

void insertCourseReplays(sqlite3 *db, int courseId, int count) {
  execOrAbort(
      db,
      "INSERT INTO replays (chart_path, chart_md5, chart_sha256, "
      "chart_title, chart_artist, gauge_type, gauge_auto_shift, final_score,"
      "max_combo, final_gauge, clear_type, assist_option) VALUES "
      "('BMS/course-stage.bms','course-stage-md5','course-stage-sha',"
      "'Course stage','Artist',0,0,0,0,100.0,300,'OFF')");
  const int stageReplayId = static_cast<int>(sqlite3_last_insert_rowid(db));
  for (int i = 1; i <= count; ++i) {
    execOrAbort(
        db, "INSERT INTO course_replays (course_id, course_name, "
            "course_group_name, constraint_json, gauge_type, gauge_profile,"
            "gauge_auto_shift, ln_mode, requested_play_option, assist_option,"
            "final_score, max_combo, final_gauge, clear_type, completed_charts,"
            "total_charts) VALUES (" +
                std::to_string(courseId) +
                ",'Course','Group','{}',0,0,0,0,'NORMAL','OFF'," +
                std::to_string(i) + "," + std::to_string(i) +
                ",100.0,300,1,1)");
    const int courseReplayId = static_cast<int>(sqlite3_last_insert_rowid(db));
    execOrAbort(db, "INSERT INTO course_replay_stages (course_replay_id,"
                    "stage_index,replay_id,rest_micros_after_stage) VALUES (" +
                        std::to_string(courseReplayId) + ",0," +
                        std::to_string(stageReplayId) + ",0)");
  }
}

void padStoredProvenance(sqlite3 *db, const std::string &table,
                         std::size_t paddingBytes) {
  std::string padded = queryText(db, "SELECT provenance_json FROM " + table +
                                         " ORDER BY id LIMIT 1");
  padded.append(paddingBytes, ' ');
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(db,
                                "UPDATE " + table + " SET provenance_json=?",
                                stmt) == SQLITE_OK);
  assert(sqlite3_bind_text(stmt.get(), 1, padded.data(),
                           static_cast<int>(padded.size()),
                           SQLITE_TRANSIENT) == SQLITE_OK);
  assert(sqlite3_step(stmt.get()) == SQLITE_DONE);
}

void testChartSummaryValidationAndDetailsShareWalSnapshot(
    const std::filesystem::path &root) {
#if !TARGET_OS_WINDOWS
  constexpr int kCandidateCount = 64;
  constexpr std::size_t kPaddingBytes = 512 * 1024;
  const auto path = root / "chart-summary-wal-snapshot" / "replay.db";
  ReplayDBHelper helper(path);
  assert(helper.EnsureSchema());
  auto db = openDatabase(path);
  insertChartReplays(db.get(), kCandidateCount);
  padStoredProvenance(db.get(), "replays", kPaddingBytes);
  const int targetId = queryInt(db.get(), "SELECT MAX(id) FROM replays");
  db.reset();

  const std::string mutation =
      "UPDATE replays SET ruleset_version=99 WHERE id=" +
      std::to_string(targetId) +
      ";INSERT INTO replay_events (replay_id,event_index,action,lane,"
      "note_time_micros,song_time_micros,judge_time_micros,judgement,"
      "diff_micros,gauge,gauge_type,combo,score) VALUES (" +
      std::to_string(targetId) + ",0,0,0,0,0,0,0,0,0.0,0,0,0)";

  bms_parser::ChartMeta meta;
  meta.SHA256 = "sha";
  meta.MD5 = "md5";
  meta.BmsPath = root / "BMS" / "chart.bms";
  meta.TotalNotes = 500;
  const auto summaries = withMutationDuringLongWalRead(
      path, mutation, [&] { return helper.ListReplays(meta, 0); });
  const auto target =
      std::find_if(summaries.begin(), summaries.end(),
                   [&](const auto &value) { return value.id == targetId; });
  assert(target != summaries.end());
  assert(target->rulesetVersion == 0);
  assert(target->eventCount == 0);

  auto mutated = openDatabase(path);
  assert(queryInt(mutated.get(), "SELECT ruleset_version FROM replays WHERE "
                                 "id=" +
                                     std::to_string(targetId)) == 99);
  assert(queryInt(mutated.get(), "SELECT COUNT(*) FROM replay_events WHERE "
                                 "replay_id=" +
                                     std::to_string(targetId)) == 1);
  mutated.reset();
  const auto laterSummaries = helper.ListReplays(meta, 0);
  assert(std::none_of(laterSummaries.begin(), laterSummaries.end(),
                      [&](const auto &value) { return value.id == targetId; }));
#else
  (void)root;
#endif
}

void testCourseSummaryValidationAndDetailsShareWalSnapshot(
    const std::filesystem::path &root) {
#if !TARGET_OS_WINDOWS
  constexpr int kCandidateCount = 64;
  constexpr std::size_t kPaddingBytes = 512 * 1024;
  constexpr int kCourseId = 71;
  const auto path = root / "course-summary-wal-snapshot" / "replay.db";
  ReplayDBHelper helper(path);
  assert(helper.EnsureSchema());
  auto db = openDatabase(path);
  insertCourseReplays(db.get(), kCourseId, kCandidateCount);
  padStoredProvenance(db.get(), "course_replays", kPaddingBytes);
  const int targetId = queryInt(db.get(), "SELECT MAX(id) FROM course_replays");
  const int stageReplayId =
      queryInt(db.get(), "SELECT replay_id FROM course_replay_stages WHERE "
                         "course_replay_id=" +
                             std::to_string(targetId));
  db.reset();

  const std::string mutation =
      "UPDATE course_replays SET ruleset_version=99,completed_charts=9 "
      "WHERE id=" +
      std::to_string(targetId) +
      ";UPDATE replays SET ruleset_version=99 WHERE id=" +
      std::to_string(stageReplayId) +
      ";INSERT INTO replay_events (replay_id,event_index,action,lane,"
      "note_time_micros,song_time_micros,judge_time_micros,judgement,"
      "diff_micros,gauge,gauge_type,combo,score) VALUES (" +
      std::to_string(stageReplayId) + ",0,0,0,0,0,0,0,0,0.0,0,0,0)";

  const auto summaries = withMutationDuringLongWalRead(
      path, mutation, [&] { return helper.ListCourseReplays(kCourseId, 0); });
  const auto target =
      std::find_if(summaries.begin(), summaries.end(),
                   [&](const auto &value) { return value.id == targetId; });
  assert(target != summaries.end());
  assert(target->rulesetVersion == 0);
  assert(target->completedCharts == 1);
  assert(target->stageCount == 1);
  assert(target->eventCount == 0);

  auto mutated = openDatabase(path);
  assert(queryInt(mutated.get(),
                  "SELECT ruleset_version FROM course_replays WHERE id=" +
                      std::to_string(targetId)) == 99);
  assert(queryInt(mutated.get(), "SELECT ruleset_version FROM replays WHERE "
                                 "id=" +
                                     std::to_string(stageReplayId)) == 99);
  mutated.reset();
  const auto laterSummaries = helper.ListCourseReplays(kCourseId, 0);
  assert(std::none_of(laterSummaries.begin(), laterSummaries.end(),
                      [&](const auto &value) { return value.id == targetId; }));
#else
  (void)root;
#endif
}

void insertCorruptChartSummaryRows(sqlite3 *db, const ReplayData &replay,
                                   int count) {
  execOrAbort(db, "BEGIN IMMEDIATE TRANSACTION");
  const char *query =
      "INSERT INTO replays (chart_path, chart_md5, chart_sha256, chart_title,"
      "chart_artist, gauge_type, gauge_auto_shift, final_score, max_combo,"
      "final_gauge, clear_type, ruleset_version, eligibility, provenance_json) "
      "VALUES ('BMS/budget.bms', ?, ?, 'Budget', 'Artist', 0, 0, ?, 0, "
      "100.0, 300, 1, 0, '{')";
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(db, query, stmt) == SQLITE_OK);
  for (int i = 0; i < count; ++i) {
    sqlite3_reset(stmt.get());
    sqlite3_clear_bindings(stmt.get());
    bindSqliteText(stmt.get(), 1, replay.chartMeta.MD5);
    bindSqliteText(stmt.get(), 2, replay.chartMeta.SHA256);
    sqlite3_bind_int(stmt.get(), 3, 1000 + i);
    assert(sqlite3_step(stmt.get()) == SQLITE_DONE);
  }
  stmt.reset();
  execOrAbort(db, "COMMIT");
}

void insertCorruptCourseSummaryRows(sqlite3 *db, int courseId, int count) {
  execOrAbort(db, "BEGIN IMMEDIATE TRANSACTION");
  const char *query =
      "INSERT INTO course_replays (course_id, course_name, course_group_name,"
      "constraint_json, gauge_type, gauge_profile, gauge_auto_shift, ln_mode,"
      "requested_play_option, assist_option, final_score, max_combo,"
      "final_gauge, clear_type, completed_charts, total_charts,"
      "ruleset_version, eligibility, provenance_json) "
      "VALUES (?, 'Budget course', 'Group', '{}', 0, 0, 0, 0, 'NORMAL', "
      "'OFF', ?, 0, 100.0, 300, 1, 1, 1, 0, '{')";
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(db, query, stmt) == SQLITE_OK);
  for (int i = 0; i < count; ++i) {
    sqlite3_reset(stmt.get());
    sqlite3_clear_bindings(stmt.get());
    sqlite3_bind_int(stmt.get(), 1, courseId);
    sqlite3_bind_int(stmt.get(), 2, 2000 + i);
    assert(sqlite3_step(stmt.get()) == SQLITE_DONE);
  }
  stmt.reset();
  execOrAbort(db, "COMMIT");
}

void testLimitedSummaryScansHaveFiniteCorruptBudget(
    const std::filesystem::path &root) {
  constexpr int kRequestedLimit = 1;
  constexpr int kExpectedCandidateBudget =
      kRequestedLimit + replay_summary_scan::kCorruptCandidateAllowance;
  constexpr int kCorruptPrefix = 600;
  const std::string inspectedText =
      "inspected=" + std::to_string(kExpectedCandidateBudget);
  const std::string budgetText =
      "budget=" + std::to_string(kExpectedCandidateBudget);

  {
    const auto path = root / "bounded-chart-summary" / "replay.db";
    ReplayDBHelper helper(path);
    assert(helper.EnsureSchema());
    ReplayData valid = sampleReplay(root, "bounded-chart-summary");
    assert(helper.SaveReplay(valid).has_value());
    auto db = openDatabase(path);
    insertCorruptChartSummaryRows(db.get(), valid, kCorruptPrefix);
    db.reset();

    ScopedLogCapture logs;
    const auto summaries = helper.ListReplays(valid.chartMeta, kRequestedLimit);
    assert(summaries.empty());
    assert(logs.countContaining("Replay summary provenance scan") == 1);
    assert(logs.anyContains(inspectedText));
    assert(logs.anyContains(budgetText));
    assert(logs.countContaining("Failed to load replay summary provenance") ==
           0);
  }

  {
    const auto path = root / "bounded-course-summary" / "replay.db";
    ReplayDBHelper helper(path);
    assert(helper.EnsureSchema());
    CourseReplayData valid;
    valid.courseId = 65;
    valid.courseName = "Bounded summary course";
    valid.provenance = sampleProvenance("bounded-course-summary");
    valid.stages.push_back(
        {.replay = sampleReplay(root, "bounded-course-summary-stage")});
    assert(helper.SaveCourseReplay(valid).has_value());
    auto db = openDatabase(path);
    insertCorruptCourseSummaryRows(db.get(), valid.courseId, kCorruptPrefix);
    db.reset();

    ScopedLogCapture logs;
    const auto summaries =
        helper.ListCourseReplays(valid.courseId, kRequestedLimit);
    assert(summaries.empty());
    assert(logs.countContaining("Course replay summary provenance scan") == 1);
    assert(logs.anyContains(inspectedText));
    assert(logs.anyContains(budgetText));
    assert(logs.countContaining(
               "Failed to load course replay summary provenance") == 0);
  }
}

void testExistingListLimits(const std::filesystem::path &root) {
  const auto path = root / "limits" / "replay.db";
  ReplayDBHelper helper(path);
  assert(helper.EnsureSchema());
  auto db = openDatabase(path);
  insertChartReplays(db.get(), 105);
  insertCourseReplays(db.get(), 7, 105);
  db.reset();

  bms_parser::ChartMeta meta;
  meta.SHA256 = "sha";
  meta.MD5 = "md5";
  meta.BmsPath = root / "BMS" / "chart.bms";
  meta.TotalNotes = 500;
  assert(helper.ListReplays(meta).size() == 100);
  const auto allChart = helper.ListReplays(meta, 0);
  assert(allChart.size() == 105);
  assert(allChart.front().id == 105);
  assert(allChart.back().id == 1);
  assert(helper.ListCourseReplays(7).size() == 100);
  const auto allCourse = helper.ListCourseReplays(7, 0);
  assert(allCourse.size() == 105);
  assert(allCourse.front().id == 105);
  assert(allCourse.back().id == 1);
}

} // namespace

int main() {
#if TARGET_OS_WINDOWS
  return 0;
#endif

  const auto root = std::filesystem::temp_directory_path() /
                    "asobmashow_replay_db_helper_tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  testVersion2MigrationPreservesOutcomesAndRows(root);
  testChartAndCourseRoundTripAndPathIsolation(root);
  testInvalidNewProvenanceFailsLoad(root);
  testInvalidProvenanceRejectsReplayWrites(root);
  testChartSummariesOmitInvalidProvenanceAndCountValidLimit(root);
  testCourseSummariesOmitInvalidProvenanceAndCountValidLimit(root);
  testCourseSummariesOmitInvalidLinkedStages(root);
  testFutureVersionRejectsWithoutSchemaMutation(root);
  testFutureReplayWritesPreservePersistentDatabaseState(root);
  testFutureReplayPreflightPreservesRawDatabaseFamily(root);
  testReplayPreflightRejectsMalformedStatesAndAllowsCreation(root);
  testReplayOwnedOpenRejectsRecoveryAndConfigurationRaces(root);
  testReplayTransactionalVersionErrorsDoNotMutateSchema(root);
  testLargeWalReplayPreflightPreservesFamily(root);
  testChartSummaryValidationAndDetailsShareWalSnapshot(root);
  testCourseSummaryValidationAndDetailsShareWalSnapshot(root);
  testExistingListLimits(root);
  testLimitedSummaryScansHaveFiniteCorruptBudget(root);

  std::filesystem::remove_all(root);
  std::cout << "replay database helper tests passed\n";
  return 0;
}
