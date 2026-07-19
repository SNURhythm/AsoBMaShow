#include "../src/repositories/ReplayRepository.h"
#include "../src/ReplayClearMarkUtils.h"
#include "../src/ResultPersistenceModel.h"
#include "../src/CourseIdentity.h"
#include "../src/FileChecksum.h"
#include "../src/LongNoteModeUtils.h"
#include "../src/ScoreProvenance.h"
#include "../src/ir/IrRemoteScoreModels.h"
#include "../src/ir/IrScoreReconciliation.h"
#include "../src/repositories/SqliteRAII.h"
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
#include <limits>
#include <optional>
#include <string>
#include <thread>
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

bool indexExists(sqlite3 *db, const std::string &index) {
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(
             db, "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?",
             stmt) == SQLITE_OK);
  bindSqliteText(stmt.get(), 1, index);
  return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

std::vector<std::string> indexColumns(sqlite3 *db,
                                      const std::string &index) {
  SqliteStatementHandle stmt;
  const std::string sql = "PRAGMA index_info(\"" + index + "\")";
  assert(prepareSqliteStatement(db, sql, stmt) == SQLITE_OK);
  std::vector<std::string> result;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *name = sqlite3_column_text(stmt.get(), 2);
    assert(name != nullptr);
    result.emplace_back(reinterpret_cast<const char *>(name));
  }
  return result;
}

bool indexIsUniqueAndPartial(sqlite3 *db, const std::string &table,
                             const std::string &index) {
  SqliteStatementHandle stmt;
  const std::string sql = "PRAGMA index_list(\"" + table + "\")";
  assert(prepareSqliteStatement(db, sql, stmt) == SQLITE_OK);
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *name = sqlite3_column_text(stmt.get(), 1);
    if (name != nullptr && index == reinterpret_cast<const char *>(name)) {
      return sqlite3_column_type(stmt.get(), 2) == SQLITE_INTEGER &&
             sqlite3_column_int(stmt.get(), 2) == 1 &&
             sqlite3_column_type(stmt.get(), 4) == SQLITE_INTEGER &&
             sqlite3_column_int(stmt.get(), 4) == 1;
    }
  }
  return false;
}

bool columnIsTextPrimaryKeyNotNull(sqlite3 *db, const std::string &table,
                                   const std::string &column) {
  SqliteStatementHandle stmt;
  const std::string sql = "PRAGMA table_info(\"" + table + "\")";
  assert(prepareSqliteStatement(db, sql, stmt) == SQLITE_OK);
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *name = sqlite3_column_text(stmt.get(), 1);
    if (name != nullptr && column == reinterpret_cast<const char *>(name)) {
      const auto *type = sqlite3_column_text(stmt.get(), 2);
      return type != nullptr &&
             std::string(reinterpret_cast<const char *>(type)) == "TEXT" &&
             sqlite3_column_int(stmt.get(), 3) == 1 &&
             sqlite3_column_int(stmt.get(), 5) == 1;
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

struct ReplayMigrationSnapshot {
  int userVersion = 0;
  std::string schema;
  std::string sentinel;

  bool operator==(const ReplayMigrationSnapshot &) const = default;
};

ReplayMigrationSnapshot
replayMigrationSnapshot(const std::filesystem::path &path) {
  auto db = openDatabase(path);
  return {
      .userVersion = queryInt(db.get(), "PRAGMA user_version"),
      .schema = schemaSnapshot(db.get()),
      .sentinel = queryText(db.get(), "SELECT value FROM sentinel"),
  };
}

void expectReplayMigrationRejectedWithoutMutation(
    const std::filesystem::path &path) {
  const auto before = replayMigrationSnapshot(path);
  ReplayRepository helper(path);
  assert(!helper.EnsureSchema());
  const auto after = replayMigrationSnapshot(path);
  assert(after == before);
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

void createFutureSentinelDatabase(const std::filesystem::path &path,
                                  int version = 99) {
  auto db = openDatabase(path);
  execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT)");
  execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('unchanged')");
  execOrAbort(db.get(), "PRAGMA user_version = " + std::to_string(version));
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

bool sameDatabaseFamilyIgnoringWalSharedMemoryBytes(
    const RawDatabaseFamilySnapshot &left,
    const RawDatabaseFamilySnapshot &right) {
  return left.files[0] == right.files[0] && left.files[1] == right.files[1] &&
         left.files[2] == right.files[2] &&
         left.files[3].has_value() == right.files[3].has_value();
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

void createVersion3CourseKeyFixture(const std::filesystem::path &path) {
  createVersion2ReplayFixture(path);
  auto db = openDatabase(path);
  for (const std::string table : {"replays", "course_replays"}) {
    execOrAbort(db.get(), "ALTER TABLE " + table +
                              " ADD COLUMN ruleset_version INTEGER NOT NULL "
                              "DEFAULT 0");
    execOrAbort(db.get(), "ALTER TABLE " + table +
                              " ADD COLUMN eligibility INTEGER NOT NULL "
                              "DEFAULT 2");
    execOrAbort(db.get(), "ALTER TABLE " + table +
                              " ADD COLUMN provenance_json TEXT NOT NULL "
                              "DEFAULT '" +
                              kLegacyProvenanceJson + "'");
  }

  const std::string completeSha = file_checksum::sha256("v3-complete");
  const std::string partialSha = file_checksum::sha256("v3-partial");
  execOrAbort(db.get(), "UPDATE replays SET chart_sha256='" + completeSha +
                            "', chart_md5='' WHERE id=1");
  execOrAbort(db.get(), "UPDATE replays SET chart_sha256='" + partialSha +
                            "', chart_md5='' WHERE id=2");
  execOrAbort(
      db.get(),
      "INSERT INTO course_replays (course_id,course_name,course_group_name,"
      "constraint_json,gauge_type,gauge_profile,gauge_auto_shift,ln_mode,"
      "requested_play_option,assist_option,final_score,max_combo,final_gauge,"
      "clear_type,completed_charts,total_charts) VALUES "
      "(18,'Complete','Group','{}',0,0,0,0,'NORMAL','OFF',100,1,100.0,300,"
      "1,1)");
  const int completeId = static_cast<int>(sqlite3_last_insert_rowid(db.get()));
  execOrAbort(db.get(),
              "INSERT INTO course_replay_stages (course_replay_id,stage_index,"
              "replay_id,rest_micros_after_stage) VALUES (" +
                  std::to_string(completeId) + ",0,1,0)");

  execOrAbort(
      db.get(),
      "INSERT INTO replays (chart_path,chart_md5,chart_sha256,chart_title,"
      "chart_artist,ln_mode,gauge_type,gauge_auto_shift,final_score,max_combo,"
      "final_gauge,clear_type,assist_option) VALUES "
      "('BMS/path-only.bms','','','Path only','Artist',0,0,0,0,0,0.0,0,"
      "'OFF')");
  const int pathOnlyReplayId =
      static_cast<int>(sqlite3_last_insert_rowid(db.get()));
  execOrAbort(
      db.get(),
      "INSERT INTO course_replays (course_id,course_name,course_group_name,"
      "constraint_json,gauge_type,gauge_profile,gauge_auto_shift,ln_mode,"
      "requested_play_option,assist_option,final_score,max_combo,final_gauge,"
      "clear_type,completed_charts,total_charts) VALUES "
      "(19,'Path only','Group','{}',0,0,0,0,'NORMAL','OFF',100,1,100.0,300,"
      "1,1)");
  const int pathOnlyId = static_cast<int>(sqlite3_last_insert_rowid(db.get()));
  execOrAbort(db.get(),
              "INSERT INTO course_replay_stages (course_replay_id,stage_index,"
              "replay_id,rest_micros_after_stage) VALUES (" +
                  std::to_string(pathOnlyId) + ",0," +
                  std::to_string(pathOnlyReplayId) + ",0)");

  execOrAbort(
      db.get(),
      "INSERT INTO course_replays (course_id,course_name,course_group_name,"
      "constraint_json,gauge_type,gauge_profile,gauge_auto_shift,ln_mode,"
      "requested_play_option,assist_option,final_score,max_combo,final_gauge,"
      "clear_type,completed_charts,total_charts) VALUES "
      "(20,'Malformed constraints','Group','{',0,0,0,0,'NORMAL','OFF',100,1,"
      "100.0,300,1,1)");
  const int malformedConstraintId =
      static_cast<int>(sqlite3_last_insert_rowid(db.get()));
  execOrAbort(db.get(),
              "INSERT INTO course_replay_stages (course_replay_id,stage_index,"
              "replay_id,rest_micros_after_stage) VALUES (" +
                  std::to_string(malformedConstraintId) + ",0,1,0)");

  const auto insertCourse = [&](int courseId, int completedCharts,
                                int totalCharts) {
    execOrAbort(
        db.get(),
        "INSERT INTO course_replays (course_id,course_name,course_group_name,"
        "constraint_json,gauge_type,gauge_profile,gauge_auto_shift,ln_mode,"
        "requested_play_option,assist_option,final_score,max_combo,"
        "final_gauge,clear_type,completed_charts,total_charts) VALUES (" +
            std::to_string(courseId) +
            ",'Corrupt','Group','{}',0,0,0,0,"
            "'NORMAL','OFF',100,1,100.0,300," +
            std::to_string(completedCharts) + "," +
            std::to_string(totalCharts) + ")");
    return static_cast<int>(sqlite3_last_insert_rowid(db.get()));
  };
  const auto linkStage = [&](int courseReplayId, int stageIndex, int replayId) {
    execOrAbort(db.get(),
                "INSERT INTO course_replay_stages (course_replay_id,"
                "stage_index,replay_id,rest_micros_after_stage) VALUES (" +
                    std::to_string(courseReplayId) + "," +
                    std::to_string(stageIndex) + "," +
                    std::to_string(replayId) + ",0)");
  };

  execOrAbort(
      db.get(),
      "INSERT INTO replays (chart_path,chart_md5,chart_sha256,chart_title,"
      "chart_artist,ln_mode,gauge_type,gauge_auto_shift,final_score,max_combo,"
      "final_gauge,clear_type,assist_option) VALUES "
      "('BMS/malformed.bms','','not-a-sha','Malformed','Artist',0,0,0,0,0,"
      "0.0,0,'OFF')");
  const int malformedHashReplayId =
      static_cast<int>(sqlite3_last_insert_rowid(db.get()));
  linkStage(insertCourse(21, 1, 1), 0, malformedHashReplayId);

  const int gapId = insertCourse(22, 2, 2);
  linkStage(gapId, 0, 1);
  linkStage(gapId, 2, 1);
  linkStage(insertCourse(23, 1, 1), 0, 999999);

  const int excessiveId =
      insertCourse(24, replay_summary_scan::kMaxCourseStagesPerCandidate + 1,
                   replay_summary_scan::kMaxCourseStagesPerCandidate + 1);
  execOrAbort(
      db.get(),
      "WITH RECURSIVE stage(value) AS (VALUES(0) UNION ALL SELECT value+1 "
      "FROM stage WHERE value<256) INSERT INTO course_replay_stages "
      "(course_replay_id,stage_index,replay_id,rest_micros_after_stage) "
      "SELECT " +
          std::to_string(excessiveId) + ",value,1,0 FROM stage");

  const int invalidCourseProvenanceId = insertCourse(25, 1, 1);
  linkStage(invalidCourseProvenanceId, 0, 1);
  execOrAbort(db.get(),
              "UPDATE course_replays SET provenance_json='{' WHERE id=" +
                  std::to_string(invalidCourseProvenanceId));

  execOrAbort(
      db.get(),
      "INSERT INTO replays (chart_path,chart_md5,chart_sha256,chart_title,"
      "chart_artist,ln_mode,gauge_type,gauge_auto_shift,final_score,max_combo,"
      "final_gauge,clear_type,assist_option) VALUES ('BMS/bad-provenance.bms',"
      "'', '" +
          file_checksum::sha256("bad-provenance-stage") +
          "','Bad provenance','Artist',0,0,0,0,0,0.0,0,'OFF')");
  const int invalidStageProvenanceReplayId =
      static_cast<int>(sqlite3_last_insert_rowid(db.get()));
  execOrAbort(db.get(), "UPDATE replays SET provenance_json='{' WHERE id=" +
                            std::to_string(invalidStageProvenanceReplayId));
  linkStage(insertCourse(26, 1, 1), 0, invalidStageProvenanceReplayId);

  const int missingStageId = insertCourse(27, 2, 2);
  linkStage(missingStageId, 0, 1);

  const int duplicateIndexId = insertCourse(28, 2, 2);
  linkStage(duplicateIndexId, 0, 1);
  linkStage(duplicateIndexId, 0, 1);

  const int countMismatchId = insertCourse(29, 2, 2);
  linkStage(countMismatchId, 0, 1);
  linkStage(countMismatchId, 1, 1);
  linkStage(countMismatchId, 2, 1);
  execOrAbort(db.get(), "PRAGMA user_version = 3");
}

void createVersion4ReplayFixture(const std::filesystem::path &path) {
  createVersion2ReplayFixture(path);
  auto db = openDatabase(path);
  for (const std::string table : {"replays", "course_replays"}) {
    execOrAbort(db.get(), "ALTER TABLE " + table +
                              " ADD COLUMN ruleset_version INTEGER NOT NULL "
                              "DEFAULT 0");
    execOrAbort(db.get(), "ALTER TABLE " + table +
                              " ADD COLUMN eligibility INTEGER NOT NULL "
                              "DEFAULT 2");
    execOrAbort(db.get(), "ALTER TABLE " + table +
                              " ADD COLUMN provenance_json TEXT NOT NULL "
                              "DEFAULT '" +
                              kLegacyProvenanceJson + "'");
  }
  execOrAbort(db.get(),
              "ALTER TABLE course_replays ADD COLUMN course_key TEXT NOT NULL "
              "DEFAULT ''");
  execOrAbort(db.get(), "CREATE INDEX idx_course_replays_key_id ON "
                        "course_replays(course_key, id)");
  execOrAbort(db.get(), "PRAGMA user_version = 4");
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
  ScoreProvenanceBuildInput input;
  input.chartMeta.MD5 = "md5-" + hash;
  input.chartMeta.SHA256 = "sha-" + hash;
  input.chartMeta.Rank = 2;
  input.chartMeta.TotalNotes = 100;
  input.longNoteMode = 2;
  input.judgeRankSource = JudgeRankSource::Chart;
  input.sourceJudgeRank = 2;
  input.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  input.totalNotes = 100;
  input.effectiveGaugeTotal = 176.0;
  input.candidateSelection = gameplay::CandidateSelectionMode::LR2;
  input.ruleset = RulesetDescriptor::Current();
  ScoreProvenance value = makeScoreProvenance(input);
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
  replay.chartMeta.SHA256 = file_checksum::sha256(hash);
  replay.chartMeta.Title = "Title " + hash;
  replay.chartMeta.Artist = "Artist";
  replay.chartMeta.TotalNotes = 50;
  replay.chartMeta.TotalLongNotes = 1;
  replay.chartMeta.LnMode = 2;
  replay.initialGaugeType = GaugeType::Hard;
  replay.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
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

std::string chartAttemptId(int suffix) {
  assert(suffix >= 0 && suffix <= 4095);
  std::string value = "00000000-0000-4000-8000-000000000000";
  constexpr std::string_view digits = "0123456789abcdef";
  value[value.size() - 3] =
      digits[static_cast<std::size_t>((suffix / 256) % 16)];
  value[value.size() - 2] =
      digits[static_cast<std::size_t>((suffix / 16) % 16)];
  value.back() = digits[static_cast<std::size_t>(suffix % 16)];
  return value;
}

result_persistence::ChartResultAttempt
sampleChartAttempt(const std::filesystem::path &root, const std::string &hash,
                   int idSuffix) {
  ReplayData replay = sampleReplay(root, hash);
  replay.provenance.stages.front().chartMd5 = replay.chartMeta.MD5;
  replay.provenance.stages.front().chartSha256 = replay.chartMeta.SHA256;
  replay.provenance.stages.front().longNoteMode = replay.chartMeta.LnMode;
  replay.touchSamples.push_back({.action = ReplayTouchAction::Move,
                                 .fingerId = 17,
                                 .songTimeMicros = 100200,
                                 .x = 0.25f,
                                 .y = 0.75f});
  replay.laneCoverEvents.push_back({.songTimeMicros = 100300,
                                    .noteStartPositionPercent = 31,
                                    .resetVisibleTimeReference = true});

  result_persistence::ChartScoreWrite score{
      .chartPath = Utils::GetStoragePathUtf8RelativeToDocuments(
          replay.chartMeta.BmsPath, "BMS/"),
      .chartMd5 = replay.chartMeta.MD5,
      .chartSha256 = replay.chartMeta.SHA256,
      .chartTitle = replay.chartMeta.Title,
      .chartArtist = replay.chartMeta.Artist,
      .longNoteMode = replay.chartMeta.LnMode,
      .score = replay.finalScore,
      .maxScore = replay.chartMeta.TotalNotes * 2,
      .maxCombo = replay.maxCombo,
      .comboBreak = 5,
      .pGreat = 40,
      .great = 11,
      .good = 2,
      .bad = 1,
      .poor = 3,
      .kPoor = 4,
      .fast = 7,
      .slow = 8,
      .finalGauge = replay.finalGauge,
      .clearType = replay.clearType,
      .provenance = replay.provenance,
  };
  return {
      .attemptId = chartAttemptId(idSuffix),
      .replay = replay,
      .score = score,
      .payloadFingerprint =
          result_persistence::payloadFingerprint(replay, score),
  };
}

void rewriteReplayResultOutboxWithMixedCaseIdentifiers(sqlite3 *db) {
  execOrAbort(db, "DROP INDEX idx_replays_attempt_id");
  execOrAbort(db, "DROP INDEX idx_pending_chart_score_created");

  execOrAbort(db, "ALTER TABLE pending_chart_score_writes RENAME TO "
                  "pending_chart_score_writes_case_tmp");
  execOrAbort(db, "ALTER TABLE pending_chart_score_writes_case_tmp RENAME TO "
                  "PENDING_CHART_SCORE_WRITES");
  execOrAbort(db, "ALTER TABLE replays RENAME TO replays_case_tmp");
  execOrAbort(db, "ALTER TABLE replays_case_tmp RENAME TO RePlAyS");

  execOrAbort(db, "ALTER TABLE RePlAyS RENAME COLUMN attempt_id TO "
                  "attempt_id_case_tmp");
  execOrAbort(db, "ALTER TABLE RePlAyS RENAME COLUMN attempt_id_case_tmp TO "
                  "ATTEMPT_ID");
  execOrAbort(db, "ALTER TABLE RePlAyS RENAME COLUMN attempt_fingerprint TO "
                  "attempt_fingerprint_case_tmp");
  execOrAbort(db, "ALTER TABLE RePlAyS RENAME COLUMN "
                  "attempt_fingerprint_case_tmp TO Attempt_Fingerprint");

  for (const auto &[original, temporary, replacement] :
       std::vector<std::array<const char *, 3>>{
           {"attempt_id", "attempt_id_case_tmp", "Attempt_Id"},
           {"replay_id", "replay_id_case_tmp", "Replay_Id"},
           {"recovery_attempts", "recovery_attempts_case_tmp",
            "Recovery_Attempts"},
           {"last_recovery_at", "last_recovery_at_case_tmp",
            "Last_Recovery_At"},
           {"created_at", "created_at_case_tmp", "Created_At"},
       }) {
    execOrAbort(db, "ALTER TABLE PENDING_CHART_SCORE_WRITES RENAME COLUMN " +
                        std::string(original) + " TO " + temporary);
    execOrAbort(db, "ALTER TABLE PENDING_CHART_SCORE_WRITES RENAME COLUMN " +
                        std::string(temporary) + " TO " + replacement);
  }

  execOrAbort(db, "CREATE UNIQUE INDEX IDX_REPLAYS_ATTEMPT_ID ON "
                  "RePlAyS(ATTEMPT_ID) WHERE ATTEMPT_ID IS NOT NULL");
  execOrAbort(db, "CREATE INDEX Idx_Pending_Chart_Score_Created ON "
                  "PENDING_CHART_SCORE_WRITES(Recovery_Attempts, "
                  "Last_Recovery_At, Created_At, Attempt_Id)");
}

void testVersion4MigrationAddsResultOutbox(const std::filesystem::path &root) {
  const auto path = root / "result-outbox-v5-migration" / "replay.db";
  createVersion4ReplayFixture(path);

  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
  assert(columnExists(db.get(), "replays", "attempt_id"));
  assert(columnExists(db.get(), "replays", "attempt_fingerprint"));
  assert(indexExists(db.get(), "idx_replays_attempt_id"));
  assert(
      indexIsUniqueAndPartial(db.get(), "replays", "idx_replays_attempt_id"));
  const std::string replayAttemptIndexSql = queryText(
      db.get(), "SELECT sql FROM sqlite_master WHERE type='index' AND "
                "name='idx_replays_attempt_id'");
  assert(replayAttemptIndexSql.find("WHERE attempt_id IS NOT NULL") !=
         std::string::npos);
  assert(tableExists(db.get(), "pending_chart_score_writes"));
  assert(columnIsTextPrimaryKeyNotNull(db.get(), "pending_chart_score_writes",
                                       "attempt_id"));
  for (const std::string column : {
           "attempt_id",
           "replay_id",
           "chart_path",
           "chart_md5",
           "chart_sha256",
           "chart_title",
           "chart_artist",
           "ln_mode",
           "score",
           "max_score",
           "max_combo",
           "combo_break",
           "pgreat",
           "great",
           "good",
           "bad",
           "poor",
           "kpoor",
           "fast",
           "slow",
           "final_gauge",
           "clear_type",
           "ruleset_version",
           "eligibility",
           "provenance_json",
           "created_at",
           "recovery_attempts",
           "last_recovery_at",
       }) {
    assert(columnExists(db.get(), "pending_chart_score_writes", column));
  }
  assert(indexExists(db.get(), "idx_pending_chart_score_created"));
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM replays WHERE attempt_id IS NOT NULL "
                  "OR attempt_fingerprint IS NOT NULL") == 0);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 0);
}

void testVersion4MarkerAcceptsExactVersion5ResultOutbox(
    const std::filesystem::path &root) {
  const auto path = root / "exact-v5-outbox-with-v4-marker" / "replay.db";
  const auto attempt =
      sampleChartAttempt(root, "exact-v5-outbox-with-v4-marker", 50);
  {
    ReplayRepository helper(path);
    assert(helper.StageChartResult(attempt, {}).status ==
           result_persistence::StageStatus::Staged);
  }

  auto db = openDatabase(path);
  const std::string schemaBefore = schemaSnapshot(db.get());
  const std::string replayBefore = queryText(
      db.get(), "SELECT attempt_id || '|' || attempt_fingerprint || '|' || "
                "final_score || '|' || max_combo FROM replays");
  const std::string pendingBefore = queryText(
      db.get(),
      "SELECT attempt_id || '|' || replay_id || '|' || score || '|' || "
      "fast || '|' || provenance_json FROM pending_chart_score_writes");
  execOrAbort(db.get(), "PRAGMA user_version=4");
  db.reset();

  ReplayRepository reopened(path);
  assert(reopened.EnsureSchema());
  db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
  assert(schemaSnapshot(db.get()) == schemaBefore);
  assert(queryText(db.get(),
                   "SELECT attempt_id || '|' || attempt_fingerprint || '|' "
                   "|| final_score || '|' || max_combo FROM replays") ==
         replayBefore);
  assert(queryText(db.get(),
                   "SELECT attempt_id || '|' || replay_id || '|' || score "
                   "|| '|' || fast || '|' || provenance_json FROM "
                   "pending_chart_score_writes") == pendingBefore);
  db.reset();

  const auto pending = reopened.LoadPendingChartScore(attempt.attemptId);
  assert(pending.status == result_persistence::PendingReadStatus::Found);
  assert(pending.value.has_value());
  assert(pending.value->score == attempt.score);
}

void testVersion4MarkerAcceptsStructurallyExactFormattedArtifacts(
    const std::filesystem::path &root) {
  const auto path = root / "formatted-v5-outbox-with-v4-marker" / "replay.db";
  {
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
  }
  auto db = openDatabase(path);
  execOrAbort(db.get(), "ALTER TABLE pending_chart_score_writes RENAME TO "
                        "pending_chart_score_writes_formatted");
  execOrAbort(db.get(),
              "ALTER TABLE pending_chart_score_writes_formatted RENAME TO "
              "pending_chart_score_writes");
  execOrAbort(db.get(), "DROP INDEX idx_replays_attempt_id");
  execOrAbort(db.get(),
              "create unique index \"idx_replays_attempt_id\" on \"replays\" "
              "(\"attempt_id\") where \"attempt_id\" is not null");
  execOrAbort(db.get(), "DROP INDEX idx_pending_chart_score_created");
  execOrAbort(db.get(),
              "create index \"idx_pending_chart_score_created\" on "
              "\"pending_chart_score_writes\" (\"recovery_attempts\", "
              "\"last_recovery_at\", \"created_at\", \"attempt_id\")");
  execOrAbort(db.get(), "PRAGMA user_version=4");
  const std::string schemaBefore = schemaSnapshot(db.get());
  db.reset();

  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
  assert(schemaSnapshot(db.get()) == schemaBefore);
}

void testVersion4MarkerAcceptsExactMixedCaseIdentifiers(
    const std::filesystem::path &root) {
  const auto path = root / "mixed-case-v5-outbox-with-v4-marker" / "replay.db";
  const auto attempt =
      sampleChartAttempt(root, "mixed-case-v5-outbox-with-v4-marker", 54);
  {
    ReplayRepository helper(path);
    assert(helper.StageChartResult(attempt, {}).status ==
           result_persistence::StageStatus::Staged);
  }

  auto db = openDatabase(path);
  rewriteReplayResultOutboxWithMixedCaseIdentifiers(db.get());
  execOrAbort(db.get(), "PRAGMA user_version=4");
  const std::string schemaBefore = schemaSnapshot(db.get());
  const std::string replayBefore = queryText(
      db.get(), "SELECT attempt_id || '|' || attempt_fingerprint || '|' || "
                "final_score FROM replays");
  const std::string pendingBefore = queryText(
      db.get(), "SELECT attempt_id || '|' || replay_id || '|' || score || "
                "'|' || recovery_attempts FROM pending_chart_score_writes");
  db.reset();

  ReplayRepository reopened(path);
  assert(reopened.EnsureSchema());
  db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
  assert(schemaSnapshot(db.get()) == schemaBefore);
  assert(queryText(db.get(),
                   "SELECT attempt_id || '|' || attempt_fingerprint || '|' "
                   "|| final_score FROM replays") == replayBefore);
  assert(queryText(db.get(),
                   "SELECT attempt_id || '|' || replay_id || '|' || score "
                   "|| '|' || recovery_attempts FROM "
                   "pending_chart_score_writes") == pendingBefore);
  db.reset();

  const auto pending = reopened.LoadPendingChartScore(attempt.attemptId);
  assert(pending.status == result_persistence::PendingReadStatus::Found);
  assert(pending.value.has_value());
  assert(pending.value->score == attempt.score);
}

void testCurrentVersionAcceptsExactMixedCaseIdentifiersOnCachedPaths(
    const std::filesystem::path &root) {
  const auto path = root / "current-v5-mixed-case-outbox" / "replay.db";
  const auto attempt =
      sampleChartAttempt(root, "current-v5-mixed-case-outbox", 55);
  ReplayRepository helper(path);
  assert(helper.StageChartResult(attempt, {}).status ==
         result_persistence::StageStatus::Staged);

  auto db = openDatabase(path);
  rewriteReplayResultOutboxWithMixedCaseIdentifiers(db.get());
  const std::string schemaBefore = schemaSnapshot(db.get());
  const std::string pendingBefore = queryText(
      db.get(), "SELECT attempt_id || '|' || replay_id || '|' || score || "
                "'|' || recovery_attempts FROM pending_chart_score_writes");
  db.reset();

  assert(helper.EnsureSchema());
  std::string bindError;
  assert(helper.BindDatabasePath(path, bindError));
  assert(bindError.empty());
  auto trusted = openDatabase(path);
  assert(queryInt(trusted.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
  assert(queryText(trusted.get(),
                   "SELECT attempt_id || '|' || replay_id || '|' || score "
                   "|| '|' || recovery_attempts FROM "
                   "pending_chart_score_writes") == pendingBefore);
  trusted.reset();

  const auto pending = helper.LoadPendingChartScore(attempt.attemptId);
  assert(pending.status == result_persistence::PendingReadStatus::Found);
  assert(pending.value.has_value());
  assert(pending.value->score == attempt.score);
  db = openDatabase(path);
  assert(schemaSnapshot(db.get()) == schemaBefore);
}

void testVersion4MarkerRejectsPartialOrMalformedVersion5Artifacts(
    const std::filesystem::path &root) {
  {
    const auto path = root / "partial-v5-outbox-column" / "replay.db";
    {
      ReplayRepository helper(path);
      assert(helper.EnsureSchema());
    }
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
    execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('partial-column')");
    execOrAbort(db.get(), "DROP TABLE pending_chart_score_writes");
    execOrAbort(db.get(), "DROP INDEX idx_replays_attempt_id");
    execOrAbort(db.get(),
                "ALTER TABLE replays DROP COLUMN attempt_fingerprint");
    execOrAbort(db.get(), "PRAGMA user_version=4");
    db.reset();
    expectReplayMigrationRejectedWithoutMutation(path);
  }

  {
    const auto path = root / "partial-v5-outbox-missing-index" / "replay.db";
    const auto attempt =
        sampleChartAttempt(root, "partial-v5-outbox-missing-index", 51);
    {
      ReplayRepository helper(path);
      assert(helper.StageChartResult(attempt, {}).status ==
             result_persistence::StageStatus::Staged);
    }
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
    execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('missing-index')");
    execOrAbort(db.get(), "DROP INDEX idx_replays_attempt_id");
    execOrAbort(db.get(), "PRAGMA user_version=4");
    db.reset();
    expectReplayMigrationRejectedWithoutMutation(path);
  }

  {
    const auto path =
        root / "partial-v5-outbox-missing-recovery-index" / "replay.db";
    {
      ReplayRepository helper(path);
      assert(helper.EnsureSchema());
    }
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
    execOrAbort(db.get(),
                "INSERT INTO sentinel VALUES ('missing-recovery-index')");
    execOrAbort(db.get(), "DROP INDEX idx_pending_chart_score_created");
    execOrAbort(db.get(), "PRAGMA user_version=4");
    db.reset();
    expectReplayMigrationRejectedWithoutMutation(path);
  }

  {
    const auto path = root / "malformed-v5-outbox-attempt-index" / "replay.db";
    {
      ReplayRepository helper(path);
      assert(helper.EnsureSchema());
    }
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
    execOrAbort(db.get(),
                "INSERT INTO sentinel VALUES ('malformed-attempt-index')");
    execOrAbort(db.get(), "DROP INDEX idx_replays_attempt_id");
    execOrAbort(db.get(),
                "CREATE INDEX idx_replays_attempt_id ON replays(attempt_id)");
    execOrAbort(db.get(), "PRAGMA user_version=4");
    db.reset();
    expectReplayMigrationRejectedWithoutMutation(path);
  }

  {
    const auto path =
        root / "malformed-v5-outbox-collapsed-predicate" / "replay.db";
    {
      ReplayRepository helper(path);
      assert(helper.EnsureSchema());
    }
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
    execOrAbort(db.get(),
                "INSERT INTO sentinel VALUES ('collapsed-predicate')");
    execOrAbort(db.get(), "DROP INDEX idx_replays_attempt_id");
    execOrAbort(db.get(), "CREATE UNIQUE INDEX idx_replays_attempt_id ON "
                          "replays(attempt_id) WHERE \"attempt_idisnotnull\"");
    execOrAbort(db.get(), "PRAGMA user_version=4");
    db.reset();
    expectReplayMigrationRejectedWithoutMutation(path);
  }

  {
    const auto path = root / "partial-v5-outbox-with-base-repair" / "replay.db";
    {
      ReplayRepository helper(path);
      assert(helper.EnsureSchema());
    }
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
    execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('rollback-repair')");
    execOrAbort(db.get(), "DROP INDEX idx_replays_attempt_id");
    execOrAbort(db.get(), "DROP INDEX idx_replays_chart_sha256");
    execOrAbort(db.get(), "PRAGMA user_version=4");
    db.reset();
    expectReplayMigrationRejectedWithoutMutation(path);
  }

  {
    const auto path = root / "malformed-v5-outbox-table" / "replay.db";
    {
      ReplayRepository helper(path);
      assert(helper.EnsureSchema());
    }
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
    execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('malformed-table')");
    execOrAbort(db.get(), "DROP TABLE pending_chart_score_writes");
    execOrAbort(db.get(), "CREATE TABLE pending_chart_score_writes("
                          "attempt_id TEXT PRIMARY KEY NOT NULL,"
                          "replay_id INTEGER NOT NULL)");
    execOrAbort(db.get(), "PRAGMA user_version=4");
    db.reset();
    expectReplayMigrationRejectedWithoutMutation(path);
  }
}

void testCurrentVersionRejectsMalformedVersion5Artifacts(
    const std::filesystem::path &root) {
  {
    const auto path = root / "current-v5-outbox-missing-index" / "replay.db";
    const auto attempt =
        sampleChartAttempt(root, "current-v5-outbox-missing-index", 52);
    {
      ReplayRepository helper(path);
      assert(helper.StageChartResult(attempt, {}).status ==
             result_persistence::StageStatus::Staged);
    }
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
    execOrAbort(db.get(),
                "INSERT INTO sentinel VALUES ('current-missing-index')");
    execOrAbort(db.get(), "DROP INDEX idx_replays_attempt_id");
    db.reset();
    expectReplayMigrationRejectedWithoutMutation(path);
  }

  {
    const auto path = root / "current-v5-malformed-outbox" / "replay.db";
    const auto attempt =
        sampleChartAttempt(root, "current-v5-malformed-outbox", 53);
    {
      ReplayRepository helper(path);
      assert(helper.StageChartResult(attempt, {}).status ==
             result_persistence::StageStatus::Staged);
    }
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
    execOrAbort(db.get(),
                "INSERT INTO sentinel VALUES ('current-malformed-outbox')");
    execOrAbort(db.get(), "DROP TABLE pending_chart_score_writes");
    execOrAbort(db.get(), "CREATE TABLE pending_chart_score_writes("
                          "attempt_id TEXT PRIMARY KEY NOT NULL,"
                          "replay_id INTEGER NOT NULL)");
    db.reset();
    expectReplayMigrationRejectedWithoutMutation(path);
  }

  {
    const auto path = root / "current-v5-cached-helper-malformed" / "replay.db";
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
    execOrAbort(db.get(),
                "INSERT INTO sentinel VALUES ('cached-malformed-outbox')");
    execOrAbort(db.get(), "DROP INDEX idx_pending_chart_score_created");
    db.reset();

    const auto before = replayMigrationSnapshot(path);
    assert(!helper.EnsureSchema());
    std::string bindError;
    assert(!helper.BindDatabasePath(path, bindError));
    assert(!bindError.empty());
    assert(replayMigrationSnapshot(path) == before);
  }
}

void testVersion4OutboxCreationFailureRollsBackBaseRepairs(
    const std::filesystem::path &root) {
  const auto path = root / "v5-outbox-create-failure" / "replay.db";
  createVersion4ReplayFixture(path);
  auto db = openDatabase(path);
  execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
  execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('ddl-rollback')");
  execOrAbort(db.get(), "CREATE VIEW pending_chart_score_writes AS "
                        "SELECT 'blocked' AS attempt_id");
  const ReplayMigrationSnapshot before{
      .userVersion = queryInt(db.get(), "PRAGMA user_version"),
      .schema = schemaSnapshot(db.get()),
      .sentinel = queryText(db.get(), "SELECT value FROM sentinel"),
  };
  db.reset();
  ReplayRepository helper(path);
  assert(!helper.EnsureSchema());
  db = openDatabase(path);
  const ReplayMigrationSnapshot after{
      .userVersion = queryInt(db.get(), "PRAGMA user_version"),
      .schema = schemaSnapshot(db.get()),
      .sentinel = queryText(db.get(), "SELECT value FROM sentinel"),
  };
  assert(after == before);
}

void testLegacyReplayRowsRemainRepeatableWithNullAttemptId(
    const std::filesystem::path &root) {
  const auto path = root / "legacy-null-attempt" / "replay.db";
  ReplayRepository helper(path);
  const ReplayData replay = sampleReplay(root, "legacy-null-attempt");
  const auto first = helper.SaveReplay(replay);
  const auto second = helper.SaveReplay(replay);
  assert(first.has_value() && second.has_value() && first != second);

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == 2);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM replays WHERE attempt_id IS NULL AND "
                  "attempt_fingerprint IS NULL") == 2);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 0);
}

void testStageChartResultIsAtomicAndReturnsTimestamp(
    const std::filesystem::path &root) {
  const auto path = root / "stage-chart-result" / "replay.db";
  ReplayRepository helper(path);
  const auto attempt = sampleChartAttempt(root, "stage-chart-result", 1);
  const auto staged = helper.StageChartResult(attempt, {});
  assert(staged.status == result_persistence::StageStatus::Staged);
  assert(staged.receipt.has_value());
  assert(staged.receipt->attemptId == attempt.attemptId);
  assert(staged.receipt->replayId > 0);
  assert(!staged.receipt->createdAt.empty());
  assert(staged.receipt->scorePending);
  assert(staged.diagnostic.empty());

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == 1);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replay_events") == 1);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replay_touch_samples") == 1);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replay_lane_cover_events") ==
         1);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 1);
  assert(queryText(db.get(), "SELECT attempt_id FROM replays") ==
         attempt.attemptId);
  assert(queryText(db.get(), "SELECT attempt_fingerprint FROM replays") ==
         attempt.payloadFingerprint);
  assert(queryText(db.get(), "SELECT created_at FROM replays") ==
         staged.receipt->createdAt);
  assert(queryText(db.get(),
                   "SELECT created_at FROM pending_chart_score_writes") ==
         staged.receipt->createdAt);
  db.reset();

  const auto pending = helper.LoadPendingChartScore(attempt.attemptId);
  assert(pending.status == result_persistence::PendingReadStatus::Found);
  assert(pending.value.has_value());
  assert(pending.value->attemptId == attempt.attemptId);
  assert(pending.value->replayId == staged.receipt->replayId);
  assert(pending.value->createdAt == staged.receipt->createdAt);
  assert(pending.value->score == attempt.score);
}

void testIdenticalAttemptIsIdempotent(const std::filesystem::path &root) {
  const auto path = root / "stage-idempotent" / "replay.db";
  ReplayRepository helper(path);
  const auto attempt = sampleChartAttempt(root, "stage-idempotent", 2);
  const auto first = helper.StageChartResult(attempt, {});
  const auto second = helper.StageChartResult(attempt, {});
  assert(first.status == result_persistence::StageStatus::Staged);
  assert(second.status == result_persistence::StageStatus::AlreadyStaged);
  assert(first.receipt.has_value() && second.receipt.has_value());
  assert(first.receipt->attemptId == second.receipt->attemptId);
  assert(first.receipt->replayId == second.receipt->replayId);
  assert(first.receipt->createdAt == second.receipt->createdAt);
  assert(first.receipt->scorePending == second.receipt->scorePending);

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == 1);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replay_events") == 1);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 1);
}

void testRestageRejectsCorruptRetainedOutbox(
    const std::filesystem::path &root) {
  const auto path = root / "restage-corrupt-retained-outbox" / "replay.db";
  ReplayRepository helper(path);

  const auto pendingOnlyCorruption =
      sampleChartAttempt(root, "restage-pending-only-corruption", 40);
  assert(helper.StageChartResult(pendingOnlyCorruption, {}).status ==
         result_persistence::StageStatus::Staged);
  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE pending_chart_score_writes SET fast=fast+1 "
                        "WHERE attempt_id='" +
                            pendingOnlyCorruption.attemptId + "'");
  db.reset();

  const auto pendingOnlyRestage =
      helper.StageChartResult(pendingOnlyCorruption, {});
  assert(pendingOnlyRestage.status ==
         result_persistence::StageStatus::IntegrityConflict);
  assert(!pendingOnlyRestage.receipt.has_value());
  assert(!pendingOnlyRestage.diagnostic.empty());
  const auto changedPending =
      helper.LoadPendingChartScore(pendingOnlyCorruption.attemptId);
  assert(changedPending.status == result_persistence::PendingReadStatus::Found);
  assert(changedPending.value.has_value());
  assert(changedPending.value->score.fast ==
         pendingOnlyCorruption.score.fast + 1);

  const auto semanticCorruption =
      sampleChartAttempt(root, "restage-semantic-corruption", 41);
  assert(helper.StageChartResult(semanticCorruption, {}).status ==
         result_persistence::StageStatus::Staged);
  db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE pending_chart_score_writes SET score=score+1 "
                        "WHERE attempt_id='" +
                            semanticCorruption.attemptId + "'");
  db.reset();

  const auto semanticRestage = helper.StageChartResult(semanticCorruption, {});
  assert(semanticRestage.status ==
         result_persistence::StageStatus::IntegrityConflict);
  assert(!semanticRestage.receipt.has_value());
  assert(!semanticRestage.diagnostic.empty());
  assert(helper.LoadPendingChartScore(semanticCorruption.attemptId).status ==
         result_persistence::PendingReadStatus::IntegrityConflict);

  db = openDatabase(path);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 2);
}

void testChangedPayloadForSameAttemptConflicts(
    const std::filesystem::path &root) {
  const auto path = root / "stage-conflict" / "replay.db";
  ReplayRepository helper(path);
  const auto original = sampleChartAttempt(root, "stage-conflict", 3);
  assert(helper.StageChartResult(original, {}).status ==
         result_persistence::StageStatus::Staged);

  auto changed = original;
  ++changed.replay.events.front().diffMicros;
  changed.payloadFingerprint =
      result_persistence::payloadFingerprint(changed.replay, changed.score);
  const auto conflict = helper.StageChartResult(changed, {});
  assert(conflict.status == result_persistence::StageStatus::IntegrityConflict);
  assert(!conflict.receipt.has_value());
  assert(!conflict.diagnostic.empty());

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == 1);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replay_events") == 1);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 1);
  assert(queryText(db.get(), "SELECT attempt_fingerprint FROM replays") ==
         original.payloadFingerprint);
}

void testStageRejectsSemanticResultConflicts(
    const std::filesystem::path &root) {
  const auto path = root / "stage-semantic-conflicts" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  const auto expectConflict = [&](auto attempt) {
    attempt.payloadFingerprint =
        result_persistence::payloadFingerprint(attempt.replay, attempt.score);
    const auto outcome = helper.StageChartResult(attempt, {});
    assert(outcome.status ==
           result_persistence::StageStatus::IntegrityConflict);
    assert(!outcome.receipt.has_value());
    assert(!outcome.diagnostic.empty());
  };

  auto clearRankConflict = sampleChartAttempt(root, "stage-semantic-clear", 10);
  clearRankConflict.replay.clearType = kClearTypeFailedRank;
  expectConflict(clearRankConflict);

  auto longNoteModeConflict =
      sampleChartAttempt(root, "stage-semantic-ln-mode", 11);
  longNoteModeConflict.score.longNoteMode = 1;
  expectConflict(longNoteModeConflict);

  auto impossibleScore =
      sampleChartAttempt(root, "stage-semantic-impossible", 12);
  ++impossibleScore.score.score;
  ++impossibleScore.replay.finalScore;
  expectConflict(impossibleScore);

  auto md5OnlyIdentity =
      sampleChartAttempt(root, "stage-semantic-md5-only", 15);
  md5OnlyIdentity.score.chartSha256.clear();
  md5OnlyIdentity.replay.chartMeta.SHA256.clear();
  md5OnlyIdentity.score.provenance.stages.front().chartSha256.clear();
  md5OnlyIdentity.replay.provenance = md5OnlyIdentity.score.provenance;
  md5OnlyIdentity.payloadFingerprint = result_persistence::payloadFingerprint(
      md5OnlyIdentity.replay, md5OnlyIdentity.score);
  const auto md5OnlyOutcome = helper.StageChartResult(md5OnlyIdentity, {});
  assert(md5OnlyOutcome.status ==
         result_persistence::StageStatus::IntegrityConflict);
  assert(!md5OnlyOutcome.receipt.has_value());
  assert(md5OnlyOutcome.diagnostic ==
         "score chart identity is not projectable");

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == 0);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 0);
}

void testStageAcceptsStandard9KeysGaugeMaximum(
    const std::filesystem::path &root) {
  const auto path = root / "stage-pms-gauge-maximum" / "replay.db";
  ReplayRepository helper(path);
  auto attempt = sampleChartAttempt(root, "stage-pms-gauge-maximum", 13);
  attempt.replay.initialGaugeType = GaugeType::Normal;
  attempt.replay.finalGauge = 120.0f;
  attempt.replay.clearType = kClearTypeNormalClearRank;
  attempt.replay.provenance.gaugeType = GaugeType::Normal;
  attempt.replay.provenance.gaugeProfile = GaugeProfile::Standard9Keys;
  attempt.score.finalGauge = attempt.replay.finalGauge;
  attempt.score.clearType = attempt.replay.clearType;
  attempt.score.provenance = attempt.replay.provenance;
  attempt.payloadFingerprint =
      result_persistence::payloadFingerprint(attempt.replay, attempt.score);

  const auto staged = helper.StageChartResult(attempt, {});
  assert(staged.status == result_persistence::StageStatus::Staged);
  const auto pending = helper.LoadPendingChartScore(attempt.attemptId);
  assert(pending.status == result_persistence::PendingReadStatus::Found);
  assert(pending.value.has_value());
  assert(pending.value->score.finalGauge == 120.0f);
}

void testStageAcceptsNonLongNoteChartMode(const std::filesystem::path &root) {
  const auto path = root / "stage-non-long-note-chart" / "replay.db";
  ReplayRepository helper(path);
  auto attempt = sampleChartAttempt(root, "stage-non-long-note-chart", 16);
  attempt.replay.chartMeta.TotalLongNotes = 0;
  attempt.replay.chartMeta.LnMode = long_note_mode::kUnknownValue;
  attempt.score.longNoteMode = long_note_mode::kUnknownValue;
  attempt.score.provenance = attempt.replay.provenance;
  attempt.payloadFingerprint =
      result_persistence::payloadFingerprint(attempt.replay, attempt.score);

  const auto staged = helper.StageChartResult(attempt, {});
  assert(staged.status == result_persistence::StageStatus::Staged);
  const auto pending = helper.LoadPendingChartScore(attempt.attemptId);
  assert(pending.status == result_persistence::PendingReadStatus::Found);
  assert(pending.value.has_value());
  assert(pending.value->score.longNoteMode == long_note_mode::kUnknownValue);
}

void testStageAcceptsUnforcedLongNoteChartMode(
    const std::filesystem::path &root) {
  const auto path = root / "stage-unforced-long-note-chart" / "replay.db";
  ReplayRepository helper(path);
  auto attempt = sampleChartAttempt(root, "stage-unforced-long-note-chart", 17);
  attempt.replay.chartMeta.LnMode = long_note_mode::kUnknownValue;
  attempt.replay.provenance.stages.front().longNoteMode =
      long_note_mode::kCnValue;
  attempt.score.longNoteMode = long_note_mode::kCnValue;
  attempt.score.provenance = attempt.replay.provenance;
  attempt.payloadFingerprint =
      result_persistence::payloadFingerprint(attempt.replay, attempt.score);

  const auto staged = helper.StageChartResult(attempt, {});
  assert(staged.status == result_persistence::StageStatus::Staged);
  const auto pending = helper.LoadPendingChartScore(attempt.attemptId);
  assert(pending.status == result_persistence::PendingReadStatus::Found);
  assert(pending.value.has_value());
  assert(pending.value->score.longNoteMode == long_note_mode::kCnValue);
}

void testStageAcceptsChargeNoteJudgementsAboveNominalNoteCount(
    const std::filesystem::path &root) {
  const auto path = root / "stage-charge-note-tail-judgements" / "replay.db";
  ReplayRepository helper(path);
  auto attempt =
      sampleChartAttempt(root, "stage-charge-note-tail-judgements", 14);
  attempt.score.score = 110;
  attempt.score.maxCombo = 55;
  attempt.score.comboBreak = 0;
  attempt.score.pGreat = 55;
  attempt.score.great = 0;
  attempt.replay.finalScore = attempt.score.score;
  attempt.replay.maxCombo = attempt.score.maxCombo;
  attempt.replay.clearType = kClearTypeFullComboRank;
  attempt.payloadFingerprint =
      result_persistence::payloadFingerprint(attempt.replay, attempt.score);

  const auto staged = helper.StageChartResult(attempt, {});
  assert(staged.status == result_persistence::StageStatus::Staged);
  const auto pending = helper.LoadPendingChartScore(attempt.attemptId);
  assert(pending.status == result_persistence::PendingReadStatus::Found);
  assert(pending.value.has_value());
  assert(pending.value->score.score > pending.value->score.maxScore);
  assert(pending.value->score.maxCombo > pending.value->score.maxScore / 2);
}

void testAcknowledgedAttemptRemainsIdempotentByFingerprint(
    const std::filesystem::path &root) {
  const auto path = root / "stage-acknowledged" / "replay.db";
  ReplayRepository helper(path);
  const auto attempt = sampleChartAttempt(root, "stage-acknowledged", 4);
  const auto staged = helper.StageChartResult(attempt, {});
  assert(staged.receipt.has_value());

  const auto wrongReplay = helper.AcknowledgePendingChartScoreAndActivateIr(
      attempt.attemptId, staged.receipt->replayId + 1);
  assert(wrongReplay.status ==
         result_persistence::AcknowledgeStatus::IntegrityConflict);
  assert(helper.LoadPendingChartScore(attempt.attemptId).status ==
         result_persistence::PendingReadStatus::Found);

  const auto acknowledged = helper.AcknowledgePendingChartScoreAndActivateIr(
      attempt.attemptId, staged.receipt->replayId);
  assert(acknowledged.status ==
         result_persistence::AcknowledgeStatus::Acknowledged);
  assert(acknowledged.diagnostic.empty());
  const auto repeated = helper.AcknowledgePendingChartScoreAndActivateIr(
      attempt.attemptId, staged.receipt->replayId);
  assert(repeated.status ==
         result_persistence::AcknowledgeStatus::AlreadyAcknowledged);

  const auto restaged = helper.StageChartResult(attempt, {});
  assert(restaged.status == result_persistence::StageStatus::AlreadyStaged);
  assert(restaged.receipt.has_value());
  assert(restaged.receipt->replayId == staged.receipt->replayId);
  assert(!restaged.receipt->scorePending);

  auto changed = attempt;
  ++changed.score.fast;
  changed.payloadFingerprint =
      result_persistence::payloadFingerprint(changed.replay, changed.score);
  assert(helper.StageChartResult(changed, {}).status ==
         result_persistence::StageStatus::IntegrityConflict);

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == 1);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 0);
}

void testOutboxInsertFailureRollsBackReplayAndChildren(
    const std::filesystem::path &root) {
  const auto path = root / "stage-outbox-rollback" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  auto db = openDatabase(path);
  execOrAbort(
      db.get(),
      "CREATE TRIGGER fail_pending_score BEFORE INSERT ON "
      "pending_chart_score_writes BEGIN SELECT RAISE(ABORT, 'forced pending "
      "failure'); END");
  db.reset();

  const auto attempt = sampleChartAttempt(root, "stage-outbox-rollback", 5);
  const auto failed = helper.StageChartResult(attempt, {});
  assert(failed.status == result_persistence::StageStatus::StorageFailure);
  assert(!failed.receipt.has_value());

  db = openDatabase(path);
  for (const std::string table :
       {"replays", "replay_events", "replay_touch_samples",
        "replay_lane_cover_events", "pending_chart_score_writes"}) {
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM " + table) == 0);
  }
}

void testPendingReadsDistinguishMissingFailureAndConflict(
    const std::filesystem::path &root) {
  const auto path = root / "pending-read-status" / "replay.db";
  ReplayRepository helper(path);
  const auto attempt = sampleChartAttempt(root, "pending-read-status", 6);
  assert(helper.LoadPendingChartScore(attempt.attemptId).status ==
         result_persistence::PendingReadStatus::NotFound);
  assert(helper.StageChartResult(attempt, {}).status ==
         result_persistence::StageStatus::Staged);

  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE pending_chart_score_writes SET "
                        "attempt_id='00000000-0000-4000-8000-00000000000A' "
                        "WHERE attempt_id='" +
                            attempt.attemptId + "'");
  db.reset();
  const auto malformedIdentity =
      helper.LoadPendingChartScore(attempt.attemptId);
  assert(malformedIdentity.status ==
         result_persistence::PendingReadStatus::IntegrityConflict);
  assert(!malformedIdentity.value.has_value());

  db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE pending_chart_score_writes SET attempt_id='" +
                            attempt.attemptId + "'");
  execOrAbort(db.get(),
              "UPDATE pending_chart_score_writes SET provenance_json='{' "
              "WHERE attempt_id='" +
                  attempt.attemptId + "'");
  db.reset();
  const auto malformed = helper.LoadPendingChartScore(attempt.attemptId);
  assert(malformed.status ==
         result_persistence::PendingReadStatus::IntegrityConflict);
  assert(!malformed.value.has_value());
  assert(!malformed.diagnostic.empty());

  db = openDatabase(path);
  execOrAbort(db.get(), "DROP TABLE pending_chart_score_writes");
  db.reset();
  const auto unavailable = helper.LoadPendingChartScore(attempt.attemptId);
  assert(unavailable.status ==
         result_persistence::PendingReadStatus::StorageFailure);
  assert(!unavailable.value.has_value());
}

void testRecoverySnapshotKeepsMalformedRowsAndContinues(
    const std::filesystem::path &root) {
  const auto path = root / "pending-batch-malformed" / "replay.db";
  ReplayRepository helper(path);
  const auto malformedAttempt =
      sampleChartAttempt(root, "pending-batch-malformed-a", 7);
  const auto validAttempt =
      sampleChartAttempt(root, "pending-batch-malformed-b", 8);
  assert(helper.StageChartResult(malformedAttempt, {}).receipt.has_value());
  assert(helper.StageChartResult(validAttempt, {}).receipt.has_value());

  auto db = openDatabase(path);
  execOrAbort(db.get(),
              "UPDATE pending_chart_score_writes SET chart_sha256='bad' "
              "WHERE attempt_id='" +
                  malformedAttempt.attemptId + "'");
  db.reset();

  const auto batch = helper.ListPendingChartScores();
  assert(batch.storageAvailable);
  assert(batch.entries.size() == 2);
  const auto malformed = std::find_if(
      batch.entries.begin(), batch.entries.end(), [&](const auto &entry) {
        return entry.attemptId == malformedAttempt.attemptId;
      });
  const auto valid = std::find_if(
      batch.entries.begin(), batch.entries.end(), [&](const auto &entry) {
        return entry.attemptId == validAttempt.attemptId;
      });
  assert(malformed != batch.entries.end());
  assert(malformed->status ==
         result_persistence::PendingReadStatus::IntegrityConflict);
  assert(!malformed->value.has_value());
  assert(valid != batch.entries.end());
  assert(valid->status == result_persistence::PendingReadStatus::Found);
  assert(valid->value.has_value());
  assert(helper.LoadPendingChartScore(malformedAttempt.attemptId).status ==
         result_persistence::PendingReadStatus::IntegrityConflict);

  db = openDatabase(path);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 2);
}

void testRecoverySnapshotPrioritizesNeverAttemptedRows(
    const std::filesystem::path &root) {
  const auto path = root / "pending-batch-fairness" / "replay.db";
  ReplayRepository helper(path);
  const auto first = sampleChartAttempt(root, "pending-fair-a", 1);
  const auto second = sampleChartAttempt(root, "pending-fair-b", 2);
  const auto third = sampleChartAttempt(root, "pending-fair-c", 3);
  assert(helper.StageChartResult(first, {}).receipt.has_value());
  assert(helper.StageChartResult(second, {}).receipt.has_value());
  assert(helper.StageChartResult(third, {}).receipt.has_value());

  const auto marked = helper.RecordPendingChartScoreRecoveryAttempt(
      first.attemptId, result_persistence::RecoveryAttemptKind::StorageFailure);
  assert(marked.status == result_persistence::RecoveryMarkStatus::Recorded);
  const auto missing = helper.RecordPendingChartScoreRecoveryAttempt(
      chartAttemptId(9),
      result_persistence::RecoveryAttemptKind::IntegrityConflict);
  assert(missing.status == result_persistence::RecoveryMarkStatus::NotFound);

  const auto batch = helper.ListPendingChartScores(2);
  assert(batch.storageAvailable);
  assert(batch.entries.size() == 2);
  assert(batch.entries[0].attemptId == second.attemptId);
  assert(batch.entries[1].attemptId == third.attemptId);

  auto db = openDatabase(path);
  assert(queryInt(db.get(),
                  "SELECT recovery_attempts FROM pending_chart_score_writes "
                  "WHERE attempt_id='" +
                      first.attemptId + "'") == 1);
  assert(queryInt(db.get(), "SELECT last_recovery_at IS NOT NULL FROM "
                            "pending_chart_score_writes WHERE attempt_id='" +
                                first.attemptId + "'") == 1);
}

void testPendingSemanticConflictsAreRetainedByAcknowledgement(
    const std::filesystem::path &root) {
  const auto path = root / "pending-semantic-conflicts" / "replay.db";
  ReplayRepository helper(path);
  const auto attempt =
      sampleChartAttempt(root, "pending-semantic-conflicts", 20);
  const auto staged = helper.StageChartResult(attempt, {});
  assert(staged.status == result_persistence::StageStatus::Staged);
  assert(staged.receipt.has_value());

  const auto expectRetainedConflict = [&] {
    const auto pending = helper.LoadPendingChartScore(attempt.attemptId);
    assert(pending.status ==
           result_persistence::PendingReadStatus::IntegrityConflict);
    assert(!pending.value.has_value());
    assert(!pending.diagnostic.empty());
    const auto acknowledged = helper.AcknowledgePendingChartScoreAndActivateIr(
        attempt.attemptId, staged.receipt->replayId);
    assert(acknowledged.status ==
           result_persistence::AcknowledgeStatus::IntegrityConflict);
    auto db = openDatabase(path);
    assert(queryInt(db.get(),
                    "SELECT COUNT(*) FROM pending_chart_score_writes") == 1);
  };

  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE replays SET final_score=final_score+1");
  db.reset();
  expectRetainedConflict();
  db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE replays SET final_score=" +
                            std::to_string(attempt.replay.finalScore));
  execOrAbort(db.get(), "UPDATE replays SET max_combo=max_combo+1");
  db.reset();
  expectRetainedConflict();
  db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE replays SET max_combo=" +
                            std::to_string(attempt.replay.maxCombo));
  execOrAbort(db.get(), "UPDATE replays SET final_gauge=final_gauge+1.0");
  db.reset();
  expectRetainedConflict();
  db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE replays SET final_gauge=82.5");
  execOrAbort(db.get(), "UPDATE pending_chart_score_writes SET clear_type=" +
                            std::to_string(kClearTypeFailedRank));
  db.reset();
  expectRetainedConflict();
  db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE pending_chart_score_writes SET clear_type=" +
                            std::to_string(attempt.score.clearType));
  execOrAbort(db.get(), "UPDATE pending_chart_score_writes SET ln_mode=1");
  db.reset();
  expectRetainedConflict();
  db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE pending_chart_score_writes SET ln_mode=" +
                            std::to_string(attempt.score.longNoteMode));
  execOrAbort(db.get(), "UPDATE replays SET ruleset_version=ruleset_version+1");
  db.reset();
  expectRetainedConflict();
  db = openDatabase(path);
  execOrAbort(db.get(),
              "UPDATE replays SET ruleset_version=" +
                  std::to_string(attempt.replay.provenance.ruleset.version));
  execOrAbort(db.get(), "UPDATE replays SET eligibility=1");
  db.reset();
  expectRetainedConflict();
  db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE replays SET eligibility=" +
                            std::to_string(static_cast<int>(
                                attempt.replay.provenance.eligibility)));

  ScoreProvenance changedProvenance = attempt.replay.provenance;
  changedProvenance.player1.option = "MIRROR";
  std::string provenanceError;
  const auto changedProvenanceJson =
      serializeValidatedScoreProvenance(changedProvenance, provenanceError);
  assert(changedProvenanceJson.has_value());
  SqliteStatementHandle provenanceUpdate;
  assert(prepareSqliteStatement(db.get(),
                                "UPDATE replays SET provenance_json=?",
                                provenanceUpdate) == SQLITE_OK);
  bindSqliteText(provenanceUpdate.get(), 1, *changedProvenanceJson);
  assert(sqlite3_step(provenanceUpdate.get()) == SQLITE_DONE);
  provenanceUpdate.reset();
  db.reset();
  expectRetainedConflict();

  db = openDatabase(path);
  const auto originalProvenanceJson = serializeValidatedScoreProvenance(
      attempt.replay.provenance, provenanceError);
  assert(originalProvenanceJson.has_value());
  assert(prepareSqliteStatement(db.get(),
                                "UPDATE replays SET provenance_json=?",
                                provenanceUpdate) == SQLITE_OK);
  bindSqliteText(provenanceUpdate.get(), 1, *originalProvenanceJson);
  assert(sqlite3_step(provenanceUpdate.get()) == SQLITE_DONE);
  provenanceUpdate.reset();
  execOrAbort(db.get(), "UPDATE pending_chart_score_writes SET score=score+1");
  execOrAbort(db.get(), "UPDATE replays SET final_score=final_score+1");
  db.reset();
  expectRetainedConflict();
}

void testPendingBatchHardCapsAt256(const std::filesystem::path &root) {
  const auto path = root / "pending-batch-hard-cap" / "replay.db";
  ReplayRepository helper(path);
  for (int suffix = 0; suffix < 257; ++suffix) {
    const auto attempt = sampleChartAttempt(
        root, "pending-cap-" + std::to_string(suffix), suffix);
    assert(helper.StageChartResult(attempt, {}).status ==
           result_persistence::StageStatus::Staged);
  }

  const auto oversized = helper.ListPendingChartScores(300);
  assert(oversized.storageAvailable);
  assert(oversized.entries.size() == 256);
  assert(oversized.remaining == 1);
  const auto smaller = helper.ListPendingChartScores(3);
  assert(smaller.storageAvailable);
  assert(smaller.entries.size() == 3);
  assert(smaller.remaining == 254);
  const auto empty = helper.ListPendingChartScores(0);
  assert(empty.storageAvailable);
  assert(empty.entries.empty());
  assert(empty.remaining == 257);
}

void testMalformedPendingIdentitiesCanRotate(
    const std::filesystem::path &root) {
  const auto path = root / "pending-malformed-identity-fairness" / "replay.db";
  ReplayRepository helper(path);
  const auto first = sampleChartAttempt(root, "pending-malformed-id-a", 30);
  const auto second = sampleChartAttempt(root, "pending-malformed-id-b", 31);
  const auto valid = sampleChartAttempt(root, "pending-malformed-id-c", 32);
  assert(helper.StageChartResult(first, {}).receipt.has_value());
  assert(helper.StageChartResult(second, {}).receipt.has_value());
  assert(helper.StageChartResult(valid, {}).receipt.has_value());

  constexpr std::string_view firstRawId = "!malformed-attempt-a";
  constexpr std::string_view secondRawId = "!malformed-attempt-b";
  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE pending_chart_score_writes SET attempt_id='" +
                            std::string(firstRawId) + "' WHERE attempt_id='" +
                            first.attemptId + "'");
  execOrAbort(db.get(), "UPDATE pending_chart_score_writes SET attempt_id='" +
                            std::string(secondRawId) + "' WHERE attempt_id='" +
                            second.attemptId + "'");
  db.reset();

  const auto firstBatch = helper.ListPendingChartScores(2);
  assert(firstBatch.storageAvailable);
  assert(firstBatch.entries.size() == 2);
  assert(firstBatch.entries[0].attemptId == firstRawId);
  assert(firstBatch.entries[1].attemptId == secondRawId);
  for (const auto &entry : firstBatch.entries) {
    assert(entry.status ==
           result_persistence::PendingReadStatus::IntegrityConflict);
    assert(!entry.value.has_value());
    assert(helper
               .RecordPendingChartScoreRecoveryAttempt(
                   entry.attemptId,
                   result_persistence::RecoveryAttemptKind::IntegrityConflict)
               .status == result_persistence::RecoveryMarkStatus::Recorded);
  }

  db = openDatabase(path);
  assert(
      queryText(db.get(), "SELECT attempt_id FROM pending_chart_score_writes "
                          "WHERE attempt_id='" +
                              std::string(firstRawId) + "'") == firstRawId);
  assert(
      queryText(db.get(), "SELECT attempt_id FROM pending_chart_score_writes "
                          "WHERE attempt_id='" +
                              std::string(secondRawId) + "'") == secondRawId);
  assert(queryInt(db.get(), "SELECT SUM(recovery_attempts) FROM "
                            "pending_chart_score_writes WHERE attempt_id LIKE "
                            "'!malformed-attempt-%'") == 2);
  db.reset();

  const auto secondBatch = helper.ListPendingChartScores(2);
  assert(secondBatch.storageAvailable);
  assert(secondBatch.entries.size() == 2);
  assert(std::ranges::any_of(secondBatch.entries, [&](const auto &entry) {
    return entry.attemptId == valid.attemptId &&
           entry.status == result_persistence::PendingReadStatus::Found;
  }));
  assert(helper
             .RecordPendingChartScoreRecoveryAttempt(
                 "!missing-attempt",
                 result_persistence::RecoveryAttemptKind::StorageFailure)
             .status == result_persistence::RecoveryMarkStatus::NotFound);
}

void finalizeCourseReplay(CourseReplayData &course) {
  if (course.completedCharts <= 0) {
    course.completedCharts = static_cast<int>(course.stages.size());
  }
  if (course.totalCharts <= 0) {
    course.totalCharts = course.completedCharts;
  }
  std::vector<course_identity::ChartIdentity> charts;
  charts.reserve(course.stages.size());
  for (const auto &stage : course.stages) {
    charts.push_back({.sha256 = stage.replay.chartMeta.SHA256,
                      .md5 = stage.replay.chartMeta.MD5});
  }
  course.courseKey =
      course_identity::makeCourseKey(charts, course.constraintJson);
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

  ReplayRepository helper(path);
  assert(helper.GetDatabasePath() == path);
  assert(helper.EnsureSchema());

  auto migrated = openDatabase(path);
  assert(queryInt(migrated.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
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
  ReplayRepository first(firstPath);
  ReplayRepository second(secondPath);
  assert(first.EnsureSchema());
  assert(second.EnsureSchema());

  ReplayData replay = sampleReplay(root, "chart");
  replay.events.insert(replay.events.begin(),
                       {.action = ReplayEventAction::Press,
                        .lane = 1,
                        .noteTimeMicros = -1,
                        .songTimeMicros = -1900000,
                        .judgeTimeMicros = -1900000,
                        .judgement = None});
  replay.touchSamples.push_back({.action = ReplayTouchAction::Down,
                                 .fingerId = 9,
                                 .songTimeMicros = -1800000,
                                 .x = 0.25f,
                                 .y = 0.5f});
  replay.laneCoverEvents.push_back({.songTimeMicros = -2000000,
                                    .noteStartPositionPercent = 20,
                                    .resetVisibleTimeReference = false});
  replay.provenance.playback = {.percent = 75,
                                .mode = audio::PlaybackMode::PitchShift};
  replay.provenance.judgeWindowScalePercent = 80;
  replay.provenance.startingGaugePercent = 37;
  replay.provenance.eligibility = ScoreEligibility::Modified;
  replay.maxCombo = replay.chartMeta.TotalNotes;
  replay.clearType = kClearTypeFullComboRank;
  const auto replayId = first.SaveReplay(replay);
  assert(replayId.has_value());
  const auto loaded = first.LoadReplay(*replayId, replay.chartMeta);
  assert(loaded.has_value());
  assert(loaded->provenance == replay.provenance);
  assert(loaded->provenance.playback == replay.provenance.playback);
  assert(loaded->provenance.judgeWindowScalePercent == 80);
  assert(loaded->provenance.startingGaugePercent == 37);
  assert(!loaded->events.empty());
  assert(loaded->events.front().songTimeMicros == -1900000);
  assert(!loaded->touchSamples.empty());
  assert(loaded->touchSamples.front().songTimeMicros == -1800000);
  assert(!loaded->laneCoverEvents.empty());
  assert(loaded->laneCoverEvents.front().songTimeMicros == -2000000);
  auto db = openDatabase(firstPath);
  const std::string storedProvenance =
      queryText(db.get(), "SELECT provenance_json FROM replays WHERE id=" +
                              std::to_string(*replayId));
  assert(storedProvenance.find("\"playback\"") != std::string::npos);
  assert(storedProvenance.find("\"judgeWindowScalePercent\":80") !=
         std::string::npos);
  assert(storedProvenance.find("\"startingGaugePercent\":37") !=
         std::string::npos);
  assert(!columnExists(db.get(), "replays", "playback_percent"));
  assert(!columnExists(db.get(), "replays", "playback_mode"));
  assert(!columnExists(db.get(), "replays", "judge_window_scale_percent"));
  assert(!columnExists(db.get(), "replays", "starting_gauge_percent"));
  assert(!columnExists(db.get(), "course_replays", "playback_percent"));
  assert(!columnExists(db.get(), "course_replays", "playback_mode"));
  assert(
      !columnExists(db.get(), "course_replays", "judge_window_scale_percent"));
  assert(!columnExists(db.get(), "course_replays", "starting_gauge_percent"));
  db.reset();
  const auto summaries = first.ListReplays(replay.chartMeta, 0);
  assert(summaries.size() == 1);
  assert(summaries.front().rulesetVersion ==
         RulesetDescriptor::kCurrentVersion);
  assert(summaries.front().eligibility == ScoreEligibility::Modified);
  assert(summaries.front().playback == replay.provenance.playback);
  assert(replay_clear_mark::effectiveClearRank(summaries.front()) ==
         kClearTypeAssistedEasyClearRank);

  CourseReplayData course;
  course.courseId = 31;
  course.courseName = "Course";
  course.courseGroupName = "Group";
  course.constraintJson = "{}";
  course.finalScore = replay.finalScore;
  course.maxCombo = replay.maxCombo;
  course.finalGauge = replay.finalGauge;
  course.clearType = kClearTypeFullComboRank;
  course.completedCharts = 1;
  course.totalCharts = 1;
  course.provenance = replay.provenance;
  course.stages.push_back(
      {.replay = sampleReplay(root, "stage"), .restMicrosAfterStage = 250000});
  finalizeCourseReplay(course);
  const auto courseId = first.SaveCourseReplay(course);
  assert(courseId.has_value());
  const auto loadedCourse = first.LoadCourseReplay(*courseId);
  assert(loadedCourse.has_value());
  assert(loadedCourse->provenance == course.provenance);
  assert(loadedCourse->stages.size() == 1);
  assert(loadedCourse->stages.front().replay.provenance ==
         course.stages.front().replay.provenance);
  const auto courseSummaries = first.ListCourseReplays(
      {.courseKey = course.courseKey, .legacyCourseId = course.courseId}, 0);
  assert(courseSummaries.size() == 1);
  assert(courseSummaries.front().rulesetVersion ==
         RulesetDescriptor::kCurrentVersion);
  assert(courseSummaries.front().eligibility == ScoreEligibility::Modified);
  assert(courseSummaries.front().playback == course.provenance.playback);
  assert(replay_clear_mark::effectiveClearRank(courseSummaries.front()) ==
         kClearTypeAssistedEasyClearRank);

  assert(second.ListReplays(replay.chartMeta, 0).empty());
  assert(second
             .ListCourseReplays({.courseKey = course.courseKey,
                                 .legacyCourseId = course.courseId},
                                0)
             .empty());
  assert(first.GetDatabasePath() == firstPath);
  assert(second.GetDatabasePath() == secondPath);

  ReplayRepository retargetable;
  retargetable.SetDatabasePath(firstPath);
  assert(retargetable.GetDatabasePath() == firstPath);
}

void testReplayResultRecordMetadata(const std::filesystem::path &root) {
  const auto path = root / "result-recall" / "replay.db";
  ReplayRepository repository(path);
  assert(repository.EnsureSchema());

  ReplayData replay = sampleReplay(root, "result-recall-chart");
  const auto replayId = repository.SaveReplay(replay);
  assert(replayId.has_value());

  auto db = openDatabase(path);
  execOrAbort(
      db.get(),
      "UPDATE replays SET "
      "attempt_id='123e4567-e89b-42d3-a456-426614174000',"
      "attempt_fingerprint='0123456789abcdef0123456789abcdef"
      "0123456789abcdef0123456789abcdef',"
      "created_at='2026-07-19 03:04:05' WHERE id=" +
          std::to_string(*replayId));
  db.reset();

  const auto recalled =
      repository.LoadReplayResult(*replayId, replay.chartMeta);
  assert(recalled.has_value());
  assert(recalled->replay.id == *replayId);
  assert(recalled->attemptId ==
         "123e4567-e89b-42d3-a456-426614174000");
  assert(recalled->attemptFingerprint ==
         "0123456789abcdef0123456789abcdef"
         "0123456789abcdef0123456789abcdef");
  assert(recalled->playedAtUnixMillis == 1784430245000LL);

  auto summaries = repository.ListReplays(replay.chartMeta, 0, "tachi");
  assert(summaries.size() == 1);
  assert(summaries.front().attemptId ==
         "123e4567-e89b-42d3-a456-426614174000");
  assert(summaries.front().hasCanonicalAttemptFingerprint);
  assert(summaries.front().provenance != nullptr);
  assert(!summaries.front().requestedIrOutboxState.has_value());
  assert(!summaries.front().irSubmissionEligible);
  assert(!summaries.front().hasIrReceipt);
  assert(summaries.front().receiptRemoteScoreId.empty());
  assert(summaries.front().irRecordState == ir::IrRecordState::Hidden);

  db = openDatabase(path);
  execOrAbort(
      db.get(),
      "INSERT INTO ir_outbox("
      "provider_id,attempt_id,chart_md5,chart_sha256,payload_json,"
      "ruleset_id,ruleset_revision,validation_fingerprint,state,"
      "local_result_ready,created_at_ms,updated_at_ms,completed_at_ms) VALUES("
      "'tachi','123e4567-e89b-42d3-a456-426614174000',"
      "'0123456789abcdef0123456789abcdef',"
      "'0123456789abcdef0123456789abcdef"
      "0123456789abcdef0123456789abcdef','{}','lr2',3,"
      "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',5,1,1,1,1)");
  execOrAbort(
      db.get(),
      "INSERT INTO ir_submission_receipts("
      "provider_id,server_origin,replay_id,attempt_id,chart_md5,chart_sha256,"
      "remote_score_id,confirmation_source,observed_in_snapshot,"
      "confirmed_at_ms) VALUES("
      "'tachi','https://boku.tachi.ac'," +
          std::to_string(*replayId) +
          ",'123e4567-e89b-42d3-a456-426614174000',"
          "'0123456789abcdef0123456789abcdef',"
          "'0123456789abcdef0123456789abcdef"
          "0123456789abcdef0123456789abcdef',"
          "'Tscore',0,0,2)");
  db.reset();

  summaries = repository.ListReplays(replay.chartMeta, 0, "tachi");
  assert(summaries.size() == 1);
  assert(summaries.front().requestedIrOutboxState ==
         ir::IrOutboxState::Succeeded);
  assert(!summaries.front().hasIrReceipt);
  assert(summaries.front().receiptRemoteScoreId.empty());

  summaries = repository.ListReplays(
      replay.chartMeta, 0, "tachi", "HTTPS://BOKU.TACHI.AC:443/");
  assert(summaries.size() == 1);
  assert(summaries.front().hasIrReceipt);
  assert(summaries.front().receiptRemoteScoreId == "Tscore");

  summaries = repository.ListReplays(replay.chartMeta, 0, "tachi",
                                     "https://other.example");
  assert(summaries.size() == 1);
  assert(!summaries.front().hasIrReceipt);
  assert(summaries.front().receiptRemoteScoreId.empty());

  summaries = repository.ListReplays(replay.chartMeta, 0, {},
                                     "https://boku.tachi.ac");
  assert(summaries.size() == 1);
  assert(!summaries.front().hasIrReceipt);
  assert(summaries.front().receiptRemoteScoreId.empty());

  db = openDatabase(path);
  execOrAbort(db.get(),
              "UPDATE replays SET attempt_id=NULL,"
              "attempt_fingerprint=NULL WHERE id=" +
                  std::to_string(*replayId));
  db.reset();
  const auto legacy =
      repository.LoadReplayResult(*replayId, replay.chartMeta);
  assert(legacy.has_value());
  assert(!legacy->attemptId.has_value());
  assert(!legacy->attemptFingerprint.has_value());
}

void testInvalidNewProvenanceFailsLoad(const std::filesystem::path &root) {
  const auto path = root / "invalid" / "replay.db";
  ReplayRepository helper(path);
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
  finalizeCourseReplay(course);
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
  ReplayRepository helper(path);
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
    finalizeCourseReplay(invalidCourse);
    assert(!helper.SaveCourseReplay(invalidCourse).has_value());

    CourseReplayData invalidStage;
    invalidStage.courseId = 61;
    invalidStage.courseName = "Invalid stage provenance";
    invalidStage.provenance = sampleProvenance("valid-course-" + label);
    invalidStage.stages.push_back(
        {.replay = sampleReplay(root, "invalid-stage-" + label)});
    finalizeCourseReplay(invalidStage);
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

  auto invalidEnum = sampleProvenance("invalid-enum");
  invalidEnum.eligibility = static_cast<ScoreEligibility>(999);
  assertRejectedWithoutRows(invalidEnum, "invalid-enum");

  ReplayData retainedReplay = sampleReplay(root, "future-ruleset");
  retainedReplay.provenance.ruleset.id = "future-ruleset";
  retainedReplay.provenance.ruleset.version =
      RulesetDescriptor::kCurrentVersion + 1;
  retainedReplay.provenance.eligibility = ScoreEligibility::Modified;
  const auto retainedId = helper.SaveReplay(retainedReplay);
  assert(retainedId.has_value());
  const auto retained =
      helper.LoadReplay(*retainedId, retainedReplay.chartMeta);
  assert(retained.has_value());
  assert(retained->provenance.ruleset == retainedReplay.provenance.ruleset);
  assert(!isSupportedRulesetDescriptor(retained->provenance.ruleset));
}

void testInvalidReplayWritesDoNotCreateDatabase(
    const std::filesystem::path &root) {
  const auto path = root / "invalid-write-no-database" / "replay.db";
  ReplayRepository helper(path);
  const auto assertDatabaseMissing = [&] {
    assert(!std::filesystem::exists(path));
  };

  ReplayData unmatchable = sampleReplay(root, "missing-identity");
  unmatchable.chartMeta.BmsPath.clear();
  unmatchable.chartMeta.MD5.clear();
  unmatchable.chartMeta.SHA256.clear();
  assert(!helper.SaveReplay(unmatchable).has_value());
  assertDatabaseMissing();

  auto invalidProvenance = sampleProvenance("invalid-no-database");
  invalidProvenance.schemaVersion = ScoreProvenance::kSchemaVersion + 1;
  ReplayData invalidReplay = sampleReplay(root, "invalid-no-database");
  invalidReplay.provenance = invalidProvenance;
  assert(!helper.SaveReplay(invalidReplay).has_value());
  assertDatabaseMissing();

  CourseReplayData emptyCourse;
  emptyCourse.courseId = 80;
  emptyCourse.provenance = sampleProvenance("empty-course-no-database");
  assert(!helper.SaveCourseReplay(emptyCourse).has_value());
  assertDatabaseMissing();

  CourseReplayData unmatchableCourse;
  unmatchableCourse.courseId = 81;
  unmatchableCourse.provenance =
      sampleProvenance("unmatchable-course-no-database");
  unmatchableCourse.stages.push_back({.replay = unmatchable});
  unmatchableCourse.courseKey = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = file_checksum::sha256("unmatchable-course-key")}},
      "[]");
  unmatchableCourse.completedCharts = 1;
  unmatchableCourse.totalCharts = 1;
  assert(!helper.SaveCourseReplay(unmatchableCourse).has_value());
  assertDatabaseMissing();

  CourseReplayData invalidCourse;
  invalidCourse.courseId = 82;
  invalidCourse.provenance = invalidProvenance;
  invalidCourse.stages.push_back(
      {.replay = sampleReplay(root, "valid-course-stage-no-database")});
  finalizeCourseReplay(invalidCourse);
  assert(!helper.SaveCourseReplay(invalidCourse).has_value());
  assertDatabaseMissing();

  CourseReplayData invalidStage;
  invalidStage.courseId = 83;
  invalidStage.provenance = sampleProvenance("valid-course-no-database");
  invalidStage.stages.push_back(
      {.replay = sampleReplay(root, "invalid-course-stage-no-database")});
  invalidStage.stages.front().replay.provenance = invalidProvenance;
  finalizeCourseReplay(invalidStage);
  assert(!helper.SaveCourseReplay(invalidStage).has_value());
  assertDatabaseMissing();

  CourseReplayData oversizedCourse;
  oversizedCourse.courseId = 84;
  oversizedCourse.provenance = sampleProvenance("oversized-course-no-database");
  oversizedCourse.stages.resize(
      replay_summary_scan::kMaxCourseStagesPerCandidate + 1,
      CourseReplayStageData{
          .replay = sampleReplay(root, "oversized-stage-no-database")});
  oversizedCourse.courseKey = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = file_checksum::sha256("oversized-course-key")}},
      "[]");
  oversizedCourse.completedCharts =
      replay_summary_scan::kMaxCourseStagesPerCandidate + 1;
  oversizedCourse.totalCharts =
      replay_summary_scan::kMaxCourseStagesPerCandidate + 1;
  assert(!helper.SaveCourseReplay(oversizedCourse).has_value());
  assertDatabaseMissing();
}

void testUnmatchableAndOversizedReplayWritesLeaveNoRows(
    const std::filesystem::path &root) {
  const auto path = root / "invalid-replay-shape-write" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  const auto assertNoReplayRows = [&] {
    auto db = openDatabase(path);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == 0);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_replays") == 0);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_replay_stages") ==
           0);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM replay_events") == 0);
  };

  ReplayData unmatchableReplay = sampleReplay(root, "unmatchable-chart");
  unmatchableReplay.chartMeta.BmsPath.clear();
  unmatchableReplay.chartMeta.MD5.clear();
  unmatchableReplay.chartMeta.SHA256.clear();
  assert(!helper.SaveReplay(unmatchableReplay).has_value());
  assertNoReplayRows();

  CourseReplayData unmatchableCourse;
  unmatchableCourse.courseId = 63;
  unmatchableCourse.provenance = sampleProvenance("unmatchable-course");
  unmatchableCourse.stages.push_back({.replay = unmatchableReplay});
  unmatchableCourse.courseKey = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = file_checksum::sha256("unmatchable-row-key")}},
      "[]");
  unmatchableCourse.completedCharts = 1;
  unmatchableCourse.totalCharts = 1;
  assert(!helper.SaveCourseReplay(unmatchableCourse).has_value());
  assertNoReplayRows();

  CourseReplayData oversizedCourse;
  oversizedCourse.courseId = 64;
  oversizedCourse.provenance = sampleProvenance("oversized-course");
  const ReplayData validStage = sampleReplay(root, "oversized-stage");
  oversizedCourse.stages.resize(
      replay_summary_scan::kMaxCourseStagesPerCandidate + 1,
      CourseReplayStageData{.replay = validStage});
  oversizedCourse.courseKey = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = file_checksum::sha256("oversized-row-key")}},
      "[]");
  oversizedCourse.completedCharts =
      replay_summary_scan::kMaxCourseStagesPerCandidate + 1;
  oversizedCourse.totalCharts =
      replay_summary_scan::kMaxCourseStagesPerCandidate + 1;
  assert(!helper.SaveCourseReplay(oversizedCourse).has_value());
  assertNoReplayRows();
}

void testChartSummariesOmitInvalidProvenanceAndCountValidLimit(
    const std::filesystem::path &root) {
  const auto path = root / "invalid-chart-summaries" / "replay.db";
  ReplayRepository helper(path);
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
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  CourseReplayData course;
  course.courseId = 62;
  course.courseName = "Summary course";
  course.completedCharts = 1;
  course.totalCharts = 1;
  course.provenance = sampleProvenance("summary-course");
  course.stages.push_back(
      {.replay = sampleReplay(root, "summary-course-stage")});
  finalizeCourseReplay(course);

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

  const auto limited = helper.ListCourseReplays(
      {.courseKey = course.courseKey, .legacyCourseId = course.courseId}, 2);
  assert(limited.size() == 2);
  assert(limited[0].id == courseReplayIds[1]);
  assert(limited[1].id == courseReplayIds[0]);

  const auto all = helper.ListCourseReplays(
      {.courseKey = course.courseKey, .legacyCourseId = course.courseId}, 0);
  assert(all.size() == 2);
  assert(all[0].id == courseReplayIds[1]);
  assert(all[1].id == courseReplayIds[0]);
}

void testCourseSummariesOmitInvalidLinkedStages(
    const std::filesystem::path &root) {
  const auto path = root / "invalid-course-summary-stages" / "replay.db";
  ReplayRepository helper(path);
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
  finalizeCourseReplay(course);

  std::vector<int> courseReplayIds;
  for (int i = 0; i < 12; ++i) {
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
  const auto replayIdForStage = [&](int courseReplayId, int stageIndex) {
    return queryInt(
        db.get(),
        "SELECT replay_id FROM course_replay_stages WHERE course_replay_id=" +
            std::to_string(courseReplayId) +
            " AND stage_index=" + std::to_string(stageIndex));
  };
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
  const int emptyIdentityReplayId = replayIdForStage(courseReplayIds[6], 0);
  execOrAbort(db.get(),
              "UPDATE replays SET chart_path='',chart_md5='',chart_sha256='' "
              "WHERE id=" +
                  std::to_string(emptyIdentityReplayId));
  const int partiallyMissingReplayId = replayIdForStage(courseReplayIds[7], 0);
  execOrAbort(db.get(), "DELETE FROM replays WHERE id=" +
                            std::to_string(partiallyMissingReplayId));
  execOrAbort(db.get(), "UPDATE course_replay_stages SET stage_index=2 WHERE "
                        "course_replay_id=" +
                            std::to_string(courseReplayIds[8]) +
                            " AND stage_index=1");
  execOrAbort(db.get(), "UPDATE course_replay_stages SET stage_index=0 WHERE "
                        "course_replay_id=" +
                            std::to_string(courseReplayIds[9]) +
                            " AND stage_index=1");
  execOrAbort(db.get(),
              "UPDATE course_replay_stages SET stage_index=CASE stage_index "
              "WHEN 0 THEN -1 ELSE 0 END WHERE course_replay_id=" +
                  std::to_string(courseReplayIds[10]));
  const int repeatedReplayId = replayIdForStage(courseReplayIds[11], 0);
  execOrAbort(
      db.get(),
      "WITH RECURSIVE counter(value) AS (VALUES(2) UNION ALL SELECT value+1 "
      "FROM counter WHERE value<256) INSERT INTO course_replay_stages "
      "(course_replay_id,stage_index,replay_id,rest_micros_after_stage) "
      "SELECT " +
          std::to_string(courseReplayIds[11]) + ",value," +
          std::to_string(repeatedReplayId) + ",0 FROM counter");
  execOrAbort(db.get(), "UPDATE replays SET ruleset_version=99 WHERE id=" +
                            std::to_string(mismatchedStageId));
  execOrAbort(db.get(), "UPDATE replays SET provenance_json='{' WHERE id=" +
                            std::to_string(malformedStageId));
  db.reset();

  const auto limited = helper.ListCourseReplays(
      {.courseKey = course.courseKey, .legacyCourseId = course.courseId}, 1);
  assert(limited.size() == 1);
  assert(limited.front().id == courseReplayIds[0]);
  const auto all = helper.ListCourseReplays(
      {.courseKey = course.courseKey, .legacyCourseId = course.courseId}, 0);
  assert(all.size() == 1);
  assert(all.front().id == courseReplayIds[0]);
  for (std::size_t i = 1; i < courseReplayIds.size(); ++i) {
    assert(!helper.LoadCourseReplay(courseReplayIds[i]).has_value());
  }
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

  ReplayRepository helper(path);
  assert(!helper.EnsureSchema());

  auto after = openDatabase(path);
  assert(queryInt(after.get(), "PRAGMA user_version") == 99);
  assert(schemaSnapshot(after.get()) == before);
  assert(queryText(after.get(), "SELECT value FROM sentinel") == "unchanged");
  assert(!tableExists(after.get(), "replays"));
  assert(!tableExists(after.get(), "course_replays"));
}

void testFutureVersionCurrentPlusOneIsRejected(
    const std::filesystem::path &root) {
  ReplayData stage = sampleReplay(root, "future-v6-stage");
  CourseReplayData course;
  course.courseId = 601;
  course.courseName = "Future v6";
  course.provenance = sampleProvenance("future-v6-course");
  course.stages.push_back({.replay = stage});
  finalizeCourseReplay(course);
  const auto attempt = sampleChartAttempt(root, "future-v6-attempt", 60);

  const auto assertUnchanged = [&](const std::string &label,
                                   const auto &operation) {
    const auto path = root / "future-v6" / label / "replay.db";
    createFutureSentinelDatabase(path,
                                 ReplayRepository::kCurrentSchemaVersion + 1);
    const auto before = persistentDatabaseSnapshot(path);
    ReplayRepository helper(path);
    operation(helper, path);
    helper.Shutdown();
    assert(persistentDatabaseSnapshot(path) == before);
  };
  assertUnchanged("ensure", [](ReplayRepository &helper, const auto &) {
    assert(!helper.EnsureSchema());
  });
  assertUnchanged("bind", [](ReplayRepository &helper, const auto &path) {
    std::string error;
    assert(!helper.BindDatabasePath(path, error));
    assert(!error.empty());
  });
  assertUnchanged("save-chart", [&](ReplayRepository &helper, const auto &) {
    assert(!helper.SaveReplay(stage).has_value());
  });
  assertUnchanged("save-course", [&](ReplayRepository &helper, const auto &) {
    assert(!helper.SaveCourseReplay(course).has_value());
  });
  assertUnchanged("list-chart", [&](ReplayRepository &helper, const auto &) {
    assert(helper.ListReplays(stage.chartMeta).empty());
  });
  assertUnchanged("list-course", [&](ReplayRepository &helper, const auto &) {
    assert(helper
               .ListCourseReplays({.courseKey = course.courseKey,
                                   .legacyCourseId = course.courseId})
               .empty());
  });
  assertUnchanged("load-chart", [&](ReplayRepository &helper, const auto &) {
    assert(!helper.LoadReplay(1, stage.chartMeta).has_value());
  });
  assertUnchanged("load-latest", [&](ReplayRepository &helper, const auto &) {
    assert(!helper.LoadLatestReplay(stage.chartMeta).has_value());
  });
  assertUnchanged("load-course", [](ReplayRepository &helper, const auto &) {
    assert(!helper.LoadCourseReplay(1).has_value());
  });
  assertUnchanged("recover", [](ReplayRepository &helper, const auto &) {
    std::string error;
    assert(!helper.RecoverCourseRecords({}, {}, error));
    assert(!error.empty());
  });
  assertUnchanged("stage", [&](ReplayRepository &helper, const auto &) {
    assert(helper.StageChartResult(attempt, {}).status ==
           result_persistence::StageStatus::StorageFailure);
  });
  assertUnchanged("load-pending", [&](ReplayRepository &helper, const auto &) {
    assert(helper.LoadPendingChartScore(attempt.attemptId).status ==
           result_persistence::PendingReadStatus::StorageFailure);
  });
  assertUnchanged("list-pending", [](ReplayRepository &helper, const auto &) {
    const auto batch = helper.ListPendingChartScores();
    assert(!batch.storageAvailable);
  });
  assertUnchanged("acknowledge", [&](ReplayRepository &helper, const auto &) {
    assert(
        helper.AcknowledgePendingChartScoreAndActivateIr(attempt.attemptId, 1)
            .status == result_persistence::AcknowledgeStatus::StorageFailure);
  });
  assertUnchanged("mark-recovery", [&](ReplayRepository &helper, const auto &) {
    assert(helper
               .RecordPendingChartScoreRecoveryAttempt(
                   attempt.attemptId,
                   result_persistence::RecoveryAttemptKind::StorageFailure)
               .status ==
           result_persistence::RecoveryMarkStatus::StorageFailure);
  });
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
  finalizeCourseReplay(course);

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
                    ReplayRepository helper(path);
                    assert(!helper.SaveReplay(replay).has_value());
                  });
  assertUnchanged(root / "future-save-course-replay" / "replay.db",
                  [&](const auto &path) {
                    ReplayRepository helper(path);
                    assert(!helper.SaveCourseReplay(course).has_value());
                  });
  assertUnchanged(root / "future-direct-replay" / "replay.db",
                  [&](const auto &path) {
                    ReplayRepository helper(path);
                    assert(!helper.EnsureSchema());
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
  finalizeCourseReplay(course);

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
    assertRejectedWithoutFamilyMutation(databaseState, "connect",
                                        [&](const auto &path) {
                                          ReplayRepository helper(path);
                                          return !helper.EnsureSchema();
                                        });
    assertRejectedWithoutFamilyMutation(
        databaseState, "save-chart", [&](const auto &path) {
          ReplayRepository helper(path);
          return !helper.SaveReplay(replay).has_value();
        });
    assertRejectedWithoutFamilyMutation(
        databaseState, "save-course", [&](const auto &path) {
          ReplayRepository helper(path);
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
  ReplayRepository directHelper(directPath);
  assert(!directHelper.EnsureSchema());
  const auto directAfter = rawDatabaseFamilySnapshot(directPath);
  // Reading a live WAL database may update transient reader marks in -shm.
  // Durable files and application-visible state must remain unchanged.
  assert(sameDatabaseFamilyIgnoringWalSharedMemoryBytes(directAfter,
                                                        directBefore));
  assert(queryInt(directDb.get(), "PRAGMA user_version") == 99);
  assert(queryText(directDb.get(), "SELECT value FROM sentinel") ==
         "unchanged");
  assert(!tableExists(directDb.get(), "replays"));
  assert(!tableExists(directDb.get(), "course_replays"));
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
    ReplayRepository helper(path);
    assert(!helper.EnsureSchema());
    assert(rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = root / "preflight-stale-shm" / "replay.db";
    createFutureSentinelDatabase(path);
    std::ofstream staleShm(path.string() + "-shm", std::ios::binary);
    staleShm << "stale-shm-without-wal";
    staleShm.close();
    const auto before = rawDatabaseFamilySnapshot(path);
    ReplayRepository helper(path);
    assert(!helper.EnsureSchema());
    assert(rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = root / "preflight-zero-byte" / "replay.db";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream empty(path, std::ios::binary);
    empty.close();
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
    assert(std::filesystem::file_size(path) > 0);
  }

  {
    const auto path = root / "preflight-missing" / "replay.db";
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
    assert(std::filesystem::exists(path));
  }

#if !TARGET_OS_WINDOWS
  {
    const auto path = root / "preflight-live-writer" / "replay.db";
    withLiveWalWriter(path, 3, [&] {
      const auto before = rawDatabaseFamilySnapshot(path);
      ReplayRepository helper(path);
      assert(!helper.EnsureSchema());
      const auto after = rawDatabaseFamilySnapshot(path);
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

int denySchemaReadAuthorizer(void *, int action, const char *first,
                             const char *, const char *, const char *) {
  return action == SQLITE_READ && first != nullptr &&
                 (std::string_view(first) == "sqlite_master" ||
                  std::string_view(first) == "sqlite_schema")
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

void testEquivalentReplayAliasesRetainValidatedSession(
    const std::filesystem::path &root) {
#if !TARGET_OS_WINDOWS
  const auto path = root / "equivalent-alias" / "replay.db";
  const auto firstHardLink =
      root / "equivalent-alias" / "replay-hard-link-one.db";
  const auto secondHardLink =
      root / "equivalent-alias" / "replay-hard-link-two.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  std::error_code linkError;
  std::filesystem::create_hard_link(path, firstHardLink, linkError);
  assert(!linkError);
  {
    ScopedDenyJournalModeAutoExtension denyJournalMode;
    std::string error;
    assert(helper.BindDatabasePath(firstHardLink, error));
  }
  assert(helper.GetDatabasePath() == firstHardLink);

  std::filesystem::create_hard_link(path, secondHardLink, linkError);
  assert(!linkError);
  {
    ScopedDenyJournalModeAutoExtension denyJournalMode;
    helper.SetDatabasePath(secondHardLink);
    assert(helper.EnsureSchema());
  }
  assert(helper.GetDatabasePath() == secondHardLink);
#else
  (void)root;
#endif
}

int installDenySchemaRead(sqlite3 *db, char **, const sqlite3_api_routines *) {
  return sqlite3_set_authorizer(db, denySchemaReadAuthorizer, nullptr);
}

class ScopedDenySchemaReadAutoExtension {
public:
  ScopedDenySchemaReadAutoExtension() {
    sqlite3_reset_auto_extension();
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(
               installDenySchemaRead)) == SQLITE_OK);
  }
  ~ScopedDenySchemaReadAutoExtension() { sqlite3_reset_auto_extension(); }
};

const char *interruptStatementFragment = nullptr;

struct InterruptStatementTrace {
  sqlite3 *db = nullptr;
  sqlite3_stmt *target = nullptr;
  std::string statementFragment;
  bool interrupted = false;
};

int interruptStatementTrace(unsigned traceType, void *rawContext,
                            void *statement, void *) {
  auto *context = static_cast<InterruptStatementTrace *>(rawContext);
  if (traceType == SQLITE_TRACE_STMT) {
    auto *stmt = static_cast<sqlite3_stmt *>(statement);
    const char *sql = sqlite3_sql(stmt);
    if (sql != nullptr &&
        std::string_view(sql).find(context->statementFragment) !=
            std::string_view::npos) {
      context->target = stmt;
    }
  } else if (traceType == SQLITE_TRACE_ROW && statement == context->target &&
             !context->interrupted) {
    context->interrupted = true;
    sqlite3_interrupt(context->db);
  } else if (traceType == SQLITE_TRACE_CLOSE) {
    delete context;
  }
  return 0;
}

int installInterruptStatementTrace(sqlite3 *db, char **,
                                   const sqlite3_api_routines *) {
  assert(interruptStatementFragment != nullptr);
  auto *context = new InterruptStatementTrace{
      .db = db, .statementFragment = interruptStatementFragment};
  const int rc = sqlite3_trace_v2(
      db, SQLITE_TRACE_STMT | SQLITE_TRACE_ROW | SQLITE_TRACE_CLOSE,
      interruptStatementTrace, context);
  if (rc != SQLITE_OK) {
    delete context;
  }
  return rc;
}

class ScopedInterruptStatementAutoExtension {
public:
  explicit ScopedInterruptStatementAutoExtension(const char *fragment) {
    assert(interruptStatementFragment == nullptr);
    interruptStatementFragment = fragment;
    sqlite3_reset_auto_extension();
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(
               installInterruptStatementTrace)) == SQLITE_OK);
  }
  ~ScopedInterruptStatementAutoExtension() {
    sqlite3_reset_auto_extension();
    interruptStatementFragment = nullptr;
  }
};

void testCourseKeyMigrationStageFailureRollsBack(
    const std::filesystem::path &root) {
  const auto path = root / "course-key-v4-stage-failure" / "replay.db";
  createVersion3CourseKeyFixture(path);
  {
    ScopedInterruptStatementAutoExtension interrupt(
        "FROM course_replay_stages s LEFT JOIN replays r");
    ReplayRepository helper(path);
    assert(!helper.EnsureSchema());
  }

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") == 3);
  assert(!columnExists(db.get(), "course_replays", "course_key"));
  assert(!indexExists(db.get(), "idx_course_replays_key_id"));
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_replays") == 13);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_replay_stages") ==
         273);
}

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
    const auto path = root / "wal-without-shm-future" / "replay.db";
    createWalDatabaseWithVersion(path, 99);
    assert(std::filesystem::remove(path.string() + "-shm"));
    const auto before = rawDatabaseFamilySnapshot(path);
    ReplayRepository helper(path);
    assert(!helper.EnsureSchema());
    assert(rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = root / "wal-schema-query-error" / "replay.db";
    createWalDatabaseWithVersion(path, 3);
    const auto before = rawDatabaseFamilySnapshot(path);
    ScopedDenySchemaReadAutoExtension denySchemaRead;
    std::string error;
    assert(!preflightSqliteUserVersion(path, 3, error).has_value());
    assert(!error.empty());
    ReplayRepository helper(path);
    assert(!helper.EnsureSchema());
    assert(rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = root / "wal-schema-terminal-step-error" / "replay.db";
    createWalDatabaseWithVersion(path, 3);
    const auto before = rawDatabaseFamilySnapshot(path);
    ScopedInterruptStatementAutoExtension interrupt(
        "SELECT count(*) FROM sqlite_schema");
    std::string error;
    assert(!preflightSqliteUserVersion(path, 3, error).has_value());
    assert(!error.empty());
    assert(rawDatabaseFamilySnapshot(path) == before);
  }
#endif

  {
    const auto path = root / "header-shaped-corrupt" / "replay.db";
    writeHeaderShapedCorruptDatabase(path);
    const auto before = rawDatabaseFamilySnapshot(path);
    ReplayRepository helper(path);
    assert(!helper.EnsureSchema());
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
    ReplayRepository helper(path);
    assert(!helper.EnsureSchema());
    assert(rawDatabaseFamilySnapshot(path) == before);
  }
}

void testCourseStageStepErrorsFailListsAndFullLoad(
    const std::filesystem::path &root) {
  const auto path = root / "course-stage-step-error" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  CourseReplayData course;
  course.courseId = 72;
  course.courseName = "Interrupted stage course";
  course.completedCharts = 2;
  course.totalCharts = 2;
  course.provenance = sampleProvenance("interrupted-stage-course");
  course.stages.push_back(
      {.replay = sampleReplay(root, "interrupted-stage-a")});
  course.stages.push_back(
      {.replay = sampleReplay(root, "interrupted-stage-b")});
  finalizeCourseReplay(course);
  const auto olderCourseReplayId = helper.SaveCourseReplay(course);
  assert(olderCourseReplayId.has_value());
  course.finalScore += 1;
  const auto courseReplayId = helper.SaveCourseReplay(course);
  assert(courseReplayId.has_value());

  helper.Shutdown();
  {
    ScopedInterruptStatementAutoExtension interrupt(
        "FROM course_replay_stages s");
    assert(helper
               .ListCourseReplays({.courseKey = course.courseKey,
                                   .legacyCourseId = course.courseId},
                                  1)
               .empty());
    helper.Shutdown();
  }
  {
    ScopedInterruptStatementAutoExtension interrupt(
        "FROM course_replay_stages s");
    assert(helper
               .ListCourseReplays({.courseKey = course.courseKey,
                                   .legacyCourseId = course.courseId},
                                  0)
               .empty());
    helper.Shutdown();
  }
  {
    ScopedInterruptStatementAutoExtension interrupt(
        "FROM course_replay_stages s");
    assert(!helper.LoadCourseReplay(*courseReplayId).has_value());
    helper.Shutdown();
  }
}

void testReplayHydrationStepErrorsFailWholeLoad(
    const std::filesystem::path &root) {
  const auto path = root / "replay-hydration-step-error" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  ReplayData chartReplay = sampleReplay(root, "interrupted-hydration-chart");
  chartReplay.events.push_back(chartReplay.events.front());
  chartReplay.events.back().songTimeMicros += 1;
  chartReplay.touchSamples = {
      {.action = ReplayTouchAction::Down,
       .fingerId = 1,
       .songTimeMicros = 200000,
       .x = 0.25f,
       .y = 0.5f},
      {.action = ReplayTouchAction::Move,
       .fingerId = 1,
       .songTimeMicros = 200001,
       .x = 0.5f,
       .y = 0.75f},
  };
  chartReplay.laneCoverEvents = {
      {.songTimeMicros = 300000,
       .noteStartPositionPercent = 20,
       .resetVisibleTimeReference = false},
      {.songTimeMicros = 300001,
       .noteStartPositionPercent = 30,
       .resetVisibleTimeReference = true},
  };
  const auto chartReplayId = helper.SaveReplay(chartReplay);
  assert(chartReplayId.has_value());

  CourseReplayData course;
  course.courseId = 73;
  course.courseName = "Interrupted hydration course";
  course.completedCharts = 1;
  course.totalCharts = 1;
  course.provenance = sampleProvenance("interrupted-hydration-course");
  course.stages.push_back({.replay = chartReplay});
  finalizeCourseReplay(course);
  const auto courseReplayId = helper.SaveCourseReplay(course);
  assert(courseReplayId.has_value());

  helper.Shutdown();
  for (const char *queryFragment : {
           "FROM replay_events WHERE replay_id",
           "FROM replay_touch_samples WHERE replay_id",
           "FROM replay_lane_cover_events WHERE replay_id",
       }) {
    {
      ScopedInterruptStatementAutoExtension interrupt(queryFragment);
      assert(!helper.LoadReplay(*chartReplayId, chartReplay.chartMeta)
                  .has_value());
      helper.Shutdown();
    }
    {
      ScopedInterruptStatementAutoExtension interrupt(queryFragment);
      assert(!helper.LoadCourseReplay(*courseReplayId).has_value());
      helper.Shutdown();
    }
  }
}

void testLargeWalReplayPreflightPreservesFamily(
    const std::filesystem::path &root) {
#if !TARGET_OS_WINDOWS
  const auto path = root / "preflight-large-wal" / "replay.db";
  createLargeFutureWalDatabase(path);
  const auto before = rawDatabaseFamilySnapshot(path);
  ReplayRepository helper(path);
  assert(!helper.EnsureSchema());
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
  ReplayRepository helper(path);
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
  ReplayRepository helper(path);
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

  const auto summaries = withMutationDuringLongWalRead(path, mutation, [&] {
    return helper.ListCourseReplays({.legacyCourseId = kCourseId}, 0);
  });
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
  const auto laterSummaries =
      helper.ListCourseReplays({.legacyCourseId = kCourseId}, 0);
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
    ReplayRepository helper(path);
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
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
    CourseReplayData valid;
    valid.courseId = 65;
    valid.courseName = "Bounded summary course";
    valid.provenance = sampleProvenance("bounded-course-summary");
    valid.stages.push_back(
        {.replay = sampleReplay(root, "bounded-course-summary-stage")});
    finalizeCourseReplay(valid);
    assert(helper.SaveCourseReplay(valid).has_value());
    auto db = openDatabase(path);
    insertCorruptCourseSummaryRows(db.get(), valid.courseId, kCorruptPrefix);
    db.reset();

    ScopedLogCapture logs;
    const auto summaries = helper.ListCourseReplays(
        {.courseKey = valid.courseKey, .legacyCourseId = valid.courseId},
        kRequestedLimit);
    assert(summaries.empty());
    assert(logs.countContaining("Course replay summary provenance scan") == 1);
    assert(logs.anyContains(inspectedText));
    assert(logs.anyContains(budgetText));
    assert(logs.countContaining(
               "Failed to load course replay summary provenance") == 0);
  }
}

void testVersion3To4BackfillsOnlyCompleteDurableCourseReplays(
    const std::filesystem::path &root) {
  const auto path = root / "course-key-v4-migration" / "replay.db";
  createVersion3CourseKeyFixture(path);
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  const std::string expectedKey = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = file_checksum::sha256("v3-complete")}},
      "{}");
  auto db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
  assert(columnExists(db.get(), "course_replays", "course_key"));
  assert(indexExists(db.get(), "idx_course_replays_key_id"));
  assert(
      queryText(db.get(),
                "SELECT course_key FROM course_replays WHERE course_id=18") ==
      expectedKey);
  assert(queryText(db.get(),
                   "SELECT course_key FROM course_replays WHERE course_id=17")
             .empty());
  assert(queryText(db.get(),
                   "SELECT course_key FROM course_replays WHERE course_id=19")
             .empty());
  assert(queryText(db.get(),
                   "SELECT course_key FROM course_replays WHERE course_id=20")
             .empty());
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_replays") == 13);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM course_replays WHERE course_id<>18 "
                  "AND course_key=''") == 12);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM course_replays WHERE course_id IN "
                  "(27,28,29) AND course_key=''") == 3);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM course_replays WHERE course_id=27 "
                  "AND completed_charts=2 AND total_charts=2") == 1);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM course_replay_stages s JOIN "
                  "course_replays cr ON cr.id=s.course_replay_id WHERE "
                  "cr.course_id=27") == 1);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM course_replay_stages s JOIN "
                  "course_replays cr ON cr.id=s.course_replay_id WHERE "
                  "cr.course_id=28 AND s.stage_index=0") == 2);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM course_replay_stages s JOIN "
                  "course_replays cr ON cr.id=s.course_replay_id WHERE "
                  "cr.course_id=29") == 3);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_replay_stages") ==
         273);
  const std::string firstSchema = schemaSnapshot(db.get());
  db.reset();
  assert(helper.EnsureSchema());
  db = openDatabase(path);
  assert(schemaSnapshot(db.get()) == firstSchema);
}

void testPartialCourseReplayStoresFullKeyAndRejectsInvalidShape(
    const std::filesystem::path &root) {
  const auto path = root / "partial-course-key" / "replay.db";
  ReplayRepository helper(path);
  const std::vector<course_identity::ChartIdentity> fullCharts = {
      {.sha256 = file_checksum::sha256("partial-stage-a")},
      {.sha256 = file_checksum::sha256("partial-stage-b")},
  };
  const std::string fullKey = course_identity::makeCourseKey(fullCharts, "[]");
  const std::string prefixKey = course_identity::makeCourseKey(
      std::span<const course_identity::ChartIdentity>(fullCharts).first(1),
      "[]");
  assert(!fullKey.empty() && fullKey != prefixKey);

  CourseReplayData partial;
  partial.courseId = 41;
  partial.courseKey = fullKey;
  partial.courseName = "Partial";
  partial.courseGroupName = "Group";
  partial.constraintJson = "[]";
  partial.completedCharts = 1;
  partial.totalCharts = 2;
  partial.provenance = sampleProvenance("partial-course");
  ReplayData stage = sampleReplay(root, "partial-stage-a");
  stage.chartMeta.SHA256 = fullCharts.front().sha256;
  partial.stages.push_back({.replay = std::move(stage)});

  const auto replayId = helper.SaveCourseReplay(partial);
  assert(replayId.has_value());
  const auto loaded = helper.LoadCourseReplay(*replayId);
  assert(loaded.has_value());
  assert(loaded->courseKey == fullKey);
  assert(loaded->courseKey != prefixKey);
  const auto afterIdChurn = helper.ListCourseReplays(
      {.courseKey = fullKey, .legacyCourseId = 9999}, 0);
  assert(afterIdChurn.size() == 1 && afterIdChurn.front().id == *replayId);
  assert(helper
             .ListCourseReplays(
                 {.courseKey = course_identity::makeCourseKey(
                      std::vector<course_identity::ChartIdentity>{
                          {.sha256 = file_checksum::sha256("other-course")}},
                      "[]"),
                  .legacyCourseId = partial.courseId},
                 0)
             .empty());

  const auto invalidPath = root / "invalid-course-v4" / "replay.db";
  ReplayRepository invalidHelper(invalidPath);
  const auto assertRejectedBeforeCreation = [&](CourseReplayData value) {
    assert(!invalidHelper.SaveCourseReplay(value).has_value());
    assert(!std::filesystem::exists(invalidPath));
  };
  CourseReplayData invalid = partial;
  invalid.courseKey.clear();
  assertRejectedBeforeCreation(invalid);
  invalid = partial;
  invalid.courseKey = "course:v1:not-a-digest";
  assertRejectedBeforeCreation(invalid);
  invalid = partial;
  invalid.completedCharts = 2;
  assertRejectedBeforeCreation(invalid);
  invalid = partial;
  invalid.completedCharts = 0;
  assertRejectedBeforeCreation(invalid);
  invalid = partial;
  invalid.totalCharts = 0;
  assertRejectedBeforeCreation(invalid);
  invalid = partial;
  invalid.stages.front().replay.chartMeta.SHA256.clear();
  invalid.stages.front().replay.chartMeta.MD5.clear();
  assertRejectedBeforeCreation(invalid);
}

void testCourseStagePreparationValidatesBeforeMetadataFallback(
    const std::filesystem::path &root) {
  bms_parser::ChartMeta expected =
      sampleReplay(root, "expected-stage").chartMeta;
  expected.MD5 = "11111111111111111111111111111111";

  CourseReplayStageData recorded{.replay =
                                     sampleReplay(root, "recorded-stage")};
  recorded.replay.chartMeta.BmsPath.clear();
  recorded.replay.chartMeta.SHA256.clear();
  recorded.replay.chartMeta.MD5.clear();
  assert(!course_replay::prepareStageForSave(recorded, expected).has_value());

  recorded.replay.chartMeta.SHA256 = file_checksum::sha256("wrong-stage");
  assert(!course_replay::prepareStageForSave(recorded, expected).has_value());

  recorded.replay.chartMeta.SHA256.clear();
  recorded.replay.chartMeta.MD5 = expected.MD5;
  const auto prepared = course_replay::prepareStageForSave(recorded, expected);
  assert(prepared.has_value());
  assert(prepared->replay.chartMeta.BmsPath == expected.BmsPath);
  assert(prepared->replay.chartMeta.SHA256.empty());
  assert(prepared->replay.chartMeta.MD5 == expected.MD5);

  std::vector<bms_parser::ChartMeta> expectedPrefix = {
      sampleReplay(root, "prefix-stage-0").chartMeta,
      sampleReplay(root, "prefix-stage-1").chartMeta,
  };
  std::vector<CourseReplayStageData> sparseStages(2);
  sparseStages[1].replay = sampleReplay(root, "prefix-stage-1");
  assert(!course_replay::prepareCompletedPrefixForSave(sparseStages,
                                                       expectedPrefix, 2)
              .has_value());

  sparseStages[0].replay = sampleReplay(root, "prefix-stage-0");
  const auto validPrefix = course_replay::prepareCompletedPrefixForSave(
      sparseStages, expectedPrefix, 2);
  assert(validPrefix.has_value());
  assert(validPrefix->size() == 2);
  assert((*validPrefix)[0].replay.chartMeta.SHA256 == expectedPrefix[0].SHA256);
  assert((*validPrefix)[1].replay.chartMeta.SHA256 == expectedPrefix[1].SHA256);
}

void testCourseReplayLookupMergesKeyAndBlankLegacyRows(
    const std::filesystem::path &root) {
  const auto path = root / "course-key-list" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  const std::vector<course_identity::ChartIdentity> chartsA = {
      {.sha256 = file_checksum::sha256("list-a")}};
  const std::vector<course_identity::ChartIdentity> chartsB = {
      {.sha256 = file_checksum::sha256("list-b")}};
  const std::string keyA = course_identity::makeCourseKey(chartsA, "[]");
  const std::string keyB = course_identity::makeCourseKey(chartsB, "[]");
  CourseReplayData replay;
  replay.courseName = "List";
  replay.constraintJson = "[]";
  replay.completedCharts = 1;
  replay.totalCharts = 1;
  replay.provenance = sampleProvenance("list-course");
  replay.stages.push_back({.replay = sampleReplay(root, "list-stage")});

  const auto save = [&](int courseId, const std::string &key, int score) {
    replay.courseId = courseId;
    replay.courseKey = key;
    replay.finalScore = score;
    const auto id = helper.SaveCourseReplay(replay);
    assert(id.has_value());
    return *id;
  };
  const int oldKeyed = save(1, keyA, 1);
  const int blankCompatible = save(77, keyA, 2);
  const int nonemptyMismatch = save(77, keyB, 3);
  const int newKeyed = save(999, keyA, 4);
  const int blankOtherId = save(88, keyA, 5);
  const int corruptNewest = save(999, keyA, 6);
  auto db = openDatabase(path);
  execOrAbort(db.get(),
              "UPDATE course_replays SET course_key='' WHERE id IN (" +
                  std::to_string(blankCompatible) + "," +
                  std::to_string(blankOtherId) + ")");
  execOrAbort(db.get(),
              "UPDATE course_replays SET provenance_json='{' WHERE id=" +
                  std::to_string(corruptNewest));
  db.reset();

  const CourseReplayLookup lookup{.courseKey = keyA, .legacyCourseId = 77};
  const auto limited = helper.ListCourseReplays(lookup, 2);
  assert(limited.size() == 2);
  assert(limited[0].id == newKeyed);
  assert(limited[1].id == blankCompatible);
  const auto all = helper.ListCourseReplays(lookup, 0);
  assert(all.size() == 3);
  assert(all[0].id == newKeyed);
  assert(all[1].id == blankCompatible);
  assert(all[2].id == oldKeyed);
  assert(std::none_of(all.begin(), all.end(), [&](const ReplaySummary &value) {
    return value.id == nonemptyMismatch || value.id == blankOtherId ||
           value.id == corruptNewest;
  }));
}

void testCourseReplayLookupInspectsInt64MaxFirstPage(
    const std::filesystem::path &root) {
  const auto path = root / "course-key-list-max-id" / "replay.db";
  ReplayRepository helper(path);
  CourseReplayData valid;
  valid.courseId = 78;
  valid.courseName = "Max ID boundary";
  valid.constraintJson = "[]";
  valid.provenance = sampleProvenance("max-id-boundary");
  valid.stages.push_back(
      {.replay = sampleReplay(root, "max-id-boundary-stage")});
  finalizeCourseReplay(valid);
  const auto validId = helper.SaveCourseReplay(valid);
  assert(validId.has_value());
  helper.Shutdown();

  auto db = openDatabase(path);
  execOrAbort(
      db.get(),
      "INSERT INTO course_replays (id,course_id,course_key,course_name,"
      "course_group_name,constraint_json,gauge_type,gauge_profile,"
      "gauge_auto_shift,ln_mode,requested_play_option,assist_option,"
      "final_score,max_combo,final_gauge,clear_type,completed_charts,"
      "total_charts,ruleset_version,eligibility,provenance_json) VALUES (" +
          std::to_string(std::numeric_limits<sqlite3_int64>::max()) + "," +
          std::to_string(valid.courseId) + ",'" + valid.courseKey +
          "','Corrupt max','Group','[]',0,0,0,0,'NORMAL','OFF',0,0,0.0,0,"
          "1,1,1,0,'{')");
  db.reset();

  ScopedLogCapture logs;
  const auto summaries = helper.ListCourseReplays(
      {.courseKey = valid.courseKey, .legacyCourseId = valid.courseId}, 0);
  assert(summaries.size() == 1 && summaries.front().id == *validId);
  assert(logs.anyContains("inspected=2"));
  assert(logs.anyContains("rejected=1"));
}

void testCourseReplayLookupRejectsOutOfRangeIdsBeforeHydration(
    const std::filesystem::path &root) {
  const auto path = root / "course-key-list-out-of-range-id" / "replay.db";
  ReplayRepository helper(path);
  CourseReplayData valid;
  valid.courseId = 79;
  valid.courseName = "Public ID boundary";
  valid.constraintJson = "[]";
  valid.finalScore = 1234;
  valid.provenance = sampleProvenance("public-id-boundary");
  valid.stages.push_back(
      {.replay = sampleReplay(root, "public-id-boundary-stage")});
  finalizeCourseReplay(valid);
  const auto validId = helper.SaveCourseReplay(valid);
  assert(validId.has_value() && *validId == 1);
  helper.Shutdown();

  constexpr sqlite3_int64 outOfRangeId = static_cast<sqlite3_int64>(1) << 32;
  constexpr sqlite3_int64 aliasedId = outOfRangeId + 1;
  auto db = openDatabase(path);
  execOrAbort(
      db.get(),
      "INSERT INTO course_replays (id,course_id,course_key,course_name,"
      "course_group_name,constraint_json,gauge_type,gauge_profile,"
      "gauge_auto_shift,ln_mode,requested_play_option,assist_option,"
      "final_score,max_combo,final_gauge,clear_type,completed_charts,"
      "total_charts,created_at,ruleset_version,eligibility,provenance_json) "
      "SELECT " +
          std::to_string(aliasedId) +
          ",course_id,course_key,'Out of range',course_group_name,"
          "constraint_json,gauge_type,gauge_profile,gauge_auto_shift,ln_mode,"
          "requested_play_option,assist_option,987654,max_combo,final_gauge,"
          "clear_type,completed_charts,total_charts,created_at,ruleset_version,"
          "eligibility,provenance_json FROM course_replays WHERE id=" +
          std::to_string(*validId));
  execOrAbort(db.get(),
              "INSERT INTO course_replay_stages (course_replay_id,stage_index,"
              "replay_id,rest_micros_after_stage) SELECT " +
                  std::to_string(aliasedId) +
                  ",stage_index,replay_id,rest_micros_after_stage FROM "
                  "course_replay_stages WHERE course_replay_id=" +
                  std::to_string(*validId));
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_replays WHERE id=" +
                                std::to_string(aliasedId)) == 1);
  db.reset();

  ScopedLogCapture logs;
  const CourseReplayLookup lookup{.courseKey = valid.courseKey,
                                  .legacyCourseId = valid.courseId};
  const auto limited = helper.ListCourseReplays(lookup, 1);
  assert(limited.size() == 1);
  assert(limited.front().id == *validId);
  assert(limited.front().finalScore == valid.finalScore);

  const auto all = helper.ListCourseReplays(lookup, 0);
  assert(all.size() == 1);
  assert(all.front().id == *validId);
  assert(all.front().finalScore == valid.finalScore);
  assert(logs.anyContains("inspected=2"));
  assert(logs.anyContains("rejected=1"));
}

void testCourseReplayRecoveryUsesPrefixThenExactScoreEvidence(
    const std::filesystem::path &root) {
  const auto path = root / "course-key-recovery" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  const course_identity::ChartIdentity common{
      .sha256 = file_checksum::sha256("recovery-common")};
  const course_identity::ChartIdentity secondA{
      .sha256 = file_checksum::sha256("recovery-a")};
  const course_identity::ChartIdentity secondB{
      .sha256 = file_checksum::sha256("recovery-b")};
  const course_identity::ChartIdentity unique{
      .sha256 = file_checksum::sha256("recovery-unique")};
  const course_identity::ChartIdentity uniqueSecond{
      .sha256 = file_checksum::sha256("recovery-unique-second")};
  const std::vector<course_identity::ChartIdentity> chartsA = {common, secondA};
  const std::vector<course_identity::ChartIdentity> chartsB = {common, secondB};
  const std::vector<course_identity::ChartIdentity> uniqueCharts = {
      unique, uniqueSecond};
  const std::string keyA = course_identity::makeCourseKey(chartsA, "[]");
  const std::string keyB = course_identity::makeCourseKey(chartsB, "[]");
  const std::string uniqueKey =
      course_identity::makeCourseKey(uniqueCharts, "[]");
  const std::vector<course_identity::Definition> definitions = {
      {.courseId = 701,
       .courseKey = keyA,
       .constraintJson = "[]",
       .charts = chartsA},
      {.courseId = 702,
       .courseKey = keyB,
       .constraintJson = "[]",
       .charts = chartsB},
      {.courseId = 703,
       .courseKey = uniqueKey,
       .constraintJson = "[]",
       .charts = uniqueCharts},
      {.courseId = 704,
       .courseKey = uniqueKey,
       .constraintJson = "[]",
       .charts = uniqueCharts},
  };

  const auto savePartial = [&](int legacyId, const std::string &name,
                               const course_identity::ChartIdentity &first,
                               const std::string &temporaryKey) {
    CourseReplayData replay;
    replay.courseId = legacyId;
    replay.courseKey = temporaryKey;
    replay.courseName = name;
    replay.courseGroupName = "Historical group";
    replay.constraintJson = "{}";
    replay.completedCharts = 1;
    replay.totalCharts = 2;
    replay.provenance = sampleProvenance("recovery-" + name);
    ReplayData stage = sampleReplay(root, "recovery-stage-" + name);
    stage.chartMeta.SHA256 = first.sha256;
    stage.chartMeta.MD5 = first.md5;
    replay.stages.push_back({.replay = std::move(stage)});
    const auto id = helper.SaveCourseReplay(replay);
    assert(id.has_value());
    return *id;
  };

  const int uniqueId = savePartial(90, "Unique", unique, uniqueKey);
  const int evidenceId = savePartial(91, "Evidence", common, keyA);
  const int ambiguousId = savePartial(92, "Ambiguous", common, keyA);
  const int mismatchId = savePartial(93, "Mismatch", common, keyA);
  auto db = openDatabase(path);
  execOrAbort(db.get(),
              "UPDATE course_replays SET course_key='' WHERE id IN (" +
                  std::to_string(uniqueId) + "," + std::to_string(evidenceId) +
                  "," + std::to_string(ambiguousId) + "," +
                  std::to_string(mismatchId) + ")");
  const std::string historicalTuple = queryText(
      db.get(),
      "SELECT course_id || '|' || course_name || '|' || course_group_name || "
      "'|' || constraint_json || '|' || total_charts FROM course_replays "
      "WHERE id=" +
          std::to_string(evidenceId));
  db.reset();

  const CourseScoreEvidence evidence{
      .legacyCourseId = 91,
      .totalCharts = 2,
      .courseName = "Evidence",
      .courseGroupName = "Historical group",
      .canonicalConstraintPayload = "[]",
      .courseKey = keyB,
  };
  std::string error;
  assert(helper.RecoverCourseRecords(definitions, {&evidence, 1}, error));
  assert(error.empty());
  db = openDatabase(path);
  assert(queryText(db.get(), "SELECT course_key FROM course_replays WHERE id=" +
                                 std::to_string(uniqueId)) == uniqueKey);
  assert(queryText(db.get(), "SELECT course_key FROM course_replays WHERE id=" +
                                 std::to_string(evidenceId)) == keyB);
  assert(queryText(db.get(), "SELECT course_key FROM course_replays WHERE id=" +
                                 std::to_string(ambiguousId))
             .empty());
  assert(queryText(db.get(),
                   "SELECT course_id || '|' || course_name || '|' || "
                   "course_group_name || '|' || constraint_json || '|' || "
                   "total_charts FROM course_replays WHERE id=" +
                       std::to_string(evidenceId)) == historicalTuple);
  db.reset();

  const std::array mismatchedEvidence = {
      CourseScoreEvidence{.legacyCourseId = 94,
                          .totalCharts = 2,
                          .courseName = "Mismatch",
                          .courseGroupName = "Historical group",
                          .canonicalConstraintPayload = "[]",
                          .courseKey = keyA},
      CourseScoreEvidence{.legacyCourseId = 93,
                          .totalCharts = 2,
                          .courseName = "Different",
                          .courseGroupName = "Historical group",
                          .canonicalConstraintPayload = "[]",
                          .courseKey = keyA},
      CourseScoreEvidence{.legacyCourseId = 93,
                          .totalCharts = 2,
                          .courseName = "Mismatch",
                          .courseGroupName = "Different",
                          .canonicalConstraintPayload = "[]",
                          .courseKey = keyA},
      CourseScoreEvidence{.legacyCourseId = 93,
                          .totalCharts = 2,
                          .courseName = "Mismatch",
                          .courseGroupName = "Historical group",
                          .canonicalConstraintPayload = "[\"no_speed\"]",
                          .courseKey = keyA},
      CourseScoreEvidence{.legacyCourseId = 93,
                          .totalCharts = 3,
                          .courseName = "Mismatch",
                          .courseGroupName = "Historical group",
                          .canonicalConstraintPayload = "[]",
                          .courseKey = keyA},
  };
  for (const auto &mismatch : mismatchedEvidence) {
    error.clear();
    assert(helper.RecoverCourseRecords(definitions, {&mismatch, 1}, error));
    assert(error.empty());
    db = openDatabase(path);
    assert(
        queryText(db.get(), "SELECT course_key FROM course_replays WHERE id=" +
                                std::to_string(mismatchId))
            .empty());
    db.reset();
  }

  const std::array ambiguousEvidence = {
      CourseScoreEvidence{.legacyCourseId = 92,
                          .totalCharts = 2,
                          .courseName = "Ambiguous",
                          .courseGroupName = "Historical group",
                          .canonicalConstraintPayload = "[]",
                          .courseKey = keyA},
      CourseScoreEvidence{.legacyCourseId = 92,
                          .totalCharts = 2,
                          .courseName = "Ambiguous",
                          .courseGroupName = "Historical group",
                          .canonicalConstraintPayload = "[]",
                          .courseKey = keyB},
  };
  error.clear();
  assert(helper.RecoverCourseRecords(definitions, ambiguousEvidence, error));
  assert(error.empty());
  db = openDatabase(path);
  assert(queryText(db.get(), "SELECT course_key FROM course_replays WHERE id=" +
                                 std::to_string(ambiguousId))
             .empty());
}

void testCourseReplayRecoveryRollsBackAndNestsInCallerTransaction(
    const std::filesystem::path &root) {
  const auto path = root / "course-key-recovery-rollback" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  const std::vector<course_identity::ChartIdentity> firstCharts = {
      {.sha256 = file_checksum::sha256("rollback-stage-a")}};
  const std::vector<course_identity::ChartIdentity> secondCharts = {
      {.sha256 = file_checksum::sha256("rollback-stage-b")}};
  const std::string firstKey =
      course_identity::makeCourseKey(firstCharts, "[]");
  const std::string secondKey =
      course_identity::makeCourseKey(secondCharts, "[]");
  const std::vector<course_identity::Definition> definitions = {
      {.courseId = 801,
       .courseKey = firstKey,
       .constraintJson = "[]",
       .charts = firstCharts},
      {.courseId = 802,
       .courseKey = secondKey,
       .constraintJson = "[]",
       .charts = secondCharts},
  };

  const auto save = [&](int legacyId, const std::string &name,
                        const std::string &key) {
    CourseReplayData replay;
    replay.courseId = legacyId;
    replay.courseKey = key;
    replay.courseName = name;
    replay.constraintJson = "[]";
    replay.completedCharts = 1;
    replay.totalCharts = 1;
    replay.provenance = sampleProvenance("rollback-" + name);
    replay.stages.push_back(
        {.replay = sampleReplay(root, "rollback-stage-" + name)});
    const auto id = helper.SaveCourseReplay(replay);
    assert(id.has_value());
    return *id;
  };
  const int firstId = save(801, "a", firstKey);
  const int secondId = save(802, "b", secondKey);
  helper.Shutdown();

  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE course_replays SET course_key=''");
  execOrAbort(db.get(),
              "CREATE TRIGGER fail_second_course_recovery "
              "BEFORE UPDATE OF course_key ON course_replays WHEN OLD.id=" +
                  std::to_string(secondId) +
                  " BEGIN SELECT RAISE(ABORT, 'forced recovery failure'); END");
  db.reset();

  std::string error;
  assert(!helper.RecoverCourseRecords(definitions, {}, error));
  assert(!error.empty());
  helper.Shutdown();
  db = openDatabase(path);
  assert(queryText(db.get(), "SELECT course_key FROM course_replays WHERE id=" +
                                 std::to_string(firstId))
             .empty());
  assert(queryText(db.get(), "SELECT course_key FROM course_replays WHERE id=" +
                                 std::to_string(secondId))
             .empty());
  execOrAbort(db.get(), "DROP TRIGGER fail_second_course_recovery");
  db.reset();
  error.clear();
  assert(helper.RecoverCourseRecords(definitions, {}, error));
  assert(error.empty());
  helper.Shutdown();
  db = openDatabase(path);
  assert(queryText(db.get(), "SELECT course_key FROM course_replays WHERE id=" +
                                 std::to_string(firstId)) == firstKey);
  assert(queryText(db.get(), "SELECT course_key FROM course_replays WHERE id=" +
                                 std::to_string(secondId)) == secondKey);
}

ir::IrOutboxDraft sampleIrDraft(std::string attemptId, std::int64_t createdAt,
                                std::string payload = R"({"score":123})") {
  return {
      .providerId = "tachi",
      .attemptId = std::move(attemptId),
      .chartMd5 = std::string(32, 'b'),
      .chartSha256 = std::string(64, 'a'),
      .payloadJson = std::move(payload),
      .rulesetProof =
          {
              .rulesetId = "lr2",
              .rulesetRevision = 3,
              .validationFingerprint = std::string(64, 'c'),
          },
      .createdAtUnixMillis = createdAt,
  };
}

ir::IrOutboxDraft
automaticIrDraft(const result_persistence::ChartResultAttempt &attempt,
                 std::string providerId, std::int64_t createdAt) {
  return {
      .providerId = std::move(providerId),
      .attemptId = attempt.attemptId,
      .chartMd5 = attempt.score.chartMd5,
      .chartSha256 = attempt.score.chartSha256,
      .payloadJson = R"({"score":123})",
      .rulesetProof =
          {
              .rulesetId = "test-rules",
              .rulesetRevision = 1,
              .validationFingerprint = std::string(64, 'd'),
          },
      .createdAtUnixMillis = createdAt,
  };
}

void testStageChartResultAtomicallyStagesIrDrafts(
    const std::filesystem::path &root) {
  const auto path = root / "ir-stage-atomic" / "replay.db";
  ReplayRepository helper(path);
  const auto attempt = sampleChartAttempt(root, "ir-stage-atomic", 81);
  const std::array drafts{
      automaticIrDraft(attempt, "tachi", 10'000),
      automaticIrDraft(attempt, "archive_readonly", 10'001),
  };

  const auto staged = helper.StageChartResult(attempt, drafts);
  assert(staged.status == result_persistence::StageStatus::Staged);
  assert(staged.receipt && staged.receipt->scorePending);
  helper.Shutdown();
  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == 1);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 1);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM ir_outbox") == 2);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM ir_outbox WHERE state=0 AND "
                  "local_result_ready=0 AND next_request_user_intent=0") == 2);
  db.reset();
  const auto inactiveCounts = helper.CountIrOutbox("tachi");
  assert(inactiveCounts.storageAvailable && inactiveCounts.total == 0 &&
         inactiveCounts.pending == 0);
  assert(helper.ListDueIrOutbox(std::numeric_limits<std::int64_t>::max())
             .entries.empty());

  const auto visible = helper.EnqueueReadyIrOutboxDraft(
      sampleIrDraft("123e4567-e89b-42d3-a456-426614174081", 10'002), false);
  assert(visible.entry);
  assert(helper.RetryAllIrOutbox("tachi", 10'100).affectedRows == 1);
  helper.Shutdown();
  db = openDatabase(path);
  assert(queryInt(db.get(),
                  "SELECT next_request_user_intent FROM ir_outbox WHERE "
                  "provider_id='tachi' AND attempt_id='" +
                      attempt.attemptId + "'") == 0);
  assert(queryInt(db.get(),
                  "SELECT next_request_user_intent FROM ir_outbox WHERE "
                  "provider_id='tachi' AND "
                  "attempt_id='123e4567-e89b-42d3-a456-426614174081'") == 1);
  db.reset();

  const auto repeated = helper.StageChartResult(attempt, drafts);
  assert(repeated.status == result_persistence::StageStatus::AlreadyStaged);
  auto changed = drafts;
  changed[0].payloadJson = R"({"score":999})";
  assert(helper.StageChartResult(attempt, changed).status ==
         result_persistence::StageStatus::IntegrityConflict);
  assert(helper
             .StageChartResult(
                 attempt, std::span<const ir::IrOutboxDraft>(drafts).first(1))
             .status == result_persistence::StageStatus::IntegrityConflict);

  const auto invalidPath = root / "ir-stage-invalid" / "replay.db";
  ReplayRepository invalid(invalidPath);
  assert(invalid.EnsureSchema());
  const auto invalidAttempt = sampleChartAttempt(root, "ir-stage-invalid", 82);
  auto duplicateProviders = std::array{
      automaticIrDraft(invalidAttempt, "tachi", 20'000),
      automaticIrDraft(invalidAttempt, "tachi", 20'001),
  };
  assert(invalid.StageChartResult(invalidAttempt, duplicateProviders).status ==
         result_persistence::StageStatus::IntegrityConflict);
  auto mismatched = automaticIrDraft(invalidAttempt, "tachi", 20'002);
  mismatched.attemptId = "123e4567-e89b-42d3-a456-426614174099";
  assert(invalid.StageChartResult(invalidAttempt, {&mismatched, 1}).status ==
         result_persistence::StageStatus::IntegrityConflict);
  auto malformed = automaticIrDraft(invalidAttempt, "tachi", 20'003);
  malformed.payloadJson = "{bad-json";
  assert(invalid.StageChartResult(invalidAttempt, {&malformed, 1}).status ==
         result_persistence::StageStatus::IntegrityConflict);
  invalid.Shutdown();
  db = openDatabase(invalidPath);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == 0);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 0);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM ir_outbox") == 0);

  const auto failurePath = root / "ir-stage-rollback" / "replay.db";
  ReplayRepository failing(failurePath);
  assert(failing.EnsureSchema());
  failing.Shutdown();
  db = openDatabase(failurePath);
  execOrAbort(
      db.get(),
      "CREATE TRIGGER fail_second_ir_draft BEFORE INSERT ON ir_outbox "
      "WHEN NEW.provider_id='archive_readonly' BEGIN SELECT RAISE(ABORT, "
      "'forced IR draft failure'); END");
  db.reset();
  const auto failedAttempt = sampleChartAttempt(root, "ir-stage-rollback", 83);
  const std::array failedDrafts{
      automaticIrDraft(failedAttempt, "tachi", 30'000),
      automaticIrDraft(failedAttempt, "archive_readonly", 30'001),
  };
  assert(failing.StageChartResult(failedAttempt, failedDrafts).status ==
         result_persistence::StageStatus::StorageFailure);
  failing.Shutdown();
  db = openDatabase(failurePath);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == 0);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 0);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM ir_outbox") == 0);
}

void testAcknowledgementActivatesIrAtomically(
    const std::filesystem::path &root) {
  const auto path = root / "ir-activate-atomic" / "replay.db";
  ReplayRepository helper(path);
  const auto attempt = sampleChartAttempt(root, "ir-activate-atomic", 84);
  const auto draft = automaticIrDraft(attempt, "tachi", 40'000);
  const auto staged = helper.StageChartResult(attempt, {&draft, 1});
  assert(staged.status == result_persistence::StageStatus::Staged &&
         staged.receipt);
  helper.Shutdown();
  auto db = openDatabase(path);
  execOrAbort(
      db.get(),
      "CREATE TRIGGER fail_ir_activation BEFORE UPDATE OF local_result_ready "
      "ON ir_outbox BEGIN SELECT RAISE(ABORT, 'forced activation failure'); "
      "END");
  db.reset();

  const auto failed = helper.AcknowledgePendingChartScoreAndActivateIr(
      attempt.attemptId, staged.receipt->replayId);
  assert(failed.status ==
         result_persistence::AcknowledgeStatus::StorageFailure);
  helper.Shutdown();
  db = openDatabase(path);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 1);
  assert(queryInt(db.get(), "SELECT local_result_ready FROM ir_outbox") == 0);
  execOrAbort(db.get(), "DROP TRIGGER fail_ir_activation");
  db.reset();

  const auto activated = helper.AcknowledgePendingChartScoreAndActivateIr(
      attempt.attemptId, staged.receipt->replayId);
  assert(activated.status ==
         result_persistence::AcknowledgeStatus::Acknowledged);
  helper.Shutdown();
  db = openDatabase(path);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes") == 0);
  assert(queryInt(db.get(), "SELECT local_result_ready FROM ir_outbox") == 1);
  db.reset();
  assert(helper.ListDueIrOutbox(std::numeric_limits<std::int64_t>::max())
             .entries.size() == 1);
  assert(helper
             .AcknowledgePendingChartScoreAndActivateIr(
                 attempt.attemptId, staged.receipt->replayId)
             .status ==
         result_persistence::AcknowledgeStatus::AlreadyAcknowledged);
}

void testVersion5MigrationAddsIrOutbox(const std::filesystem::path &root) {
  const auto path = root / "ir-outbox-migration" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  helper.Shutdown();

  auto db = openDatabase(path);
  execOrAbort(db.get(), "DROP TABLE IF EXISTS ir_outbox");
  execOrAbort(db.get(), "PRAGMA user_version=5");
  db.reset();

  assert(helper.EnsureSchema());
  helper.Shutdown();
  db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
  assert(tableExists(db.get(), "ir_outbox"));
  assert(indexExists(db.get(), "idx_ir_outbox_due"));
  assert(indexExists(db.get(), "idx_ir_outbox_attempt"));
  for (const std::string_view column :
       {"id", "provider_id", "attempt_id", "chart_md5", "chart_sha256",
        "payload_json", "state", "local_result_ready", "request_attempt_count",
        "consecutive_failure_count", "remote_poll_count", "next_attempt_at_ms",
        "next_request_user_intent", "remote_job_id", "remote_origin",
        "last_error_code", "last_error_message", "created_at_ms",
        "updated_at_ms", "completed_at_ms", "ruleset_id",
        "ruleset_revision", "validation_fingerprint"}) {
    assert(columnExists(db.get(), "ir_outbox", std::string(column)));
  }
  SqliteStatementHandle foreignKeys;
  assert(prepareSqliteStatement(db.get(), "PRAGMA foreign_key_list(ir_outbox)",
                                foreignKeys) == SQLITE_OK);
  assert(sqlite3_step(foreignKeys.get()) == SQLITE_DONE);
  assert(!columnExists(db.get(), "ir_outbox", "api_key") &&
         !columnExists(db.get(), "ir_outbox", "authorization"));

  const auto malformedPath = root / "ir-outbox-malformed" / "replay.db";
  ReplayRepository malformed(malformedPath);
  assert(malformed.EnsureSchema());
  malformed.Shutdown();
  auto malformedDb = openDatabase(malformedPath);
  execOrAbort(malformedDb.get(), "DROP INDEX idx_ir_outbox_due");
  const std::string malformedSchema = schemaSnapshot(malformedDb.get());
  malformedDb.reset();
  assert(!malformed.EnsureSchema());
  malformedDb = openDatabase(malformedPath);
  assert(queryInt(malformedDb.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
  assert(schemaSnapshot(malformedDb.get()) == malformedSchema);
}

void testVersion6MigrationBlocksRowsWithoutRulesetProof(
    const std::filesystem::path &root) {
  const auto path = root / "ir-outbox-proof-migration" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  helper.Shutdown();

  auto db = openDatabase(path);
  execOrAbort(db.get(), "DROP TABLE ir_outbox");
  execOrAbort(
      db.get(),
      "CREATE TABLE ir_outbox ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,provider_id TEXT NOT NULL,"
      "attempt_id TEXT NOT NULL,chart_md5 TEXT,chart_sha256 TEXT NOT NULL,"
      "payload_json TEXT NOT NULL,state INTEGER NOT NULL,"
      "local_result_ready INTEGER NOT NULL DEFAULT 0,"
      "request_attempt_count INTEGER NOT NULL DEFAULT 0,"
      "consecutive_failure_count INTEGER NOT NULL DEFAULT 0,"
      "next_attempt_at_ms INTEGER,"
      "next_request_user_intent INTEGER NOT NULL DEFAULT 0,"
      "remote_job_id TEXT,remote_origin TEXT,last_error_code TEXT,"
      "last_error_message TEXT,created_at_ms INTEGER NOT NULL,"
      "updated_at_ms INTEGER NOT NULL,completed_at_ms INTEGER,"
      "UNIQUE(provider_id, attempt_id),CHECK (local_result_ready IN (0, 1)),"
      "CHECK (next_request_user_intent IN (0, 1)),"
      "CHECK ((remote_job_id IS NULL AND remote_origin IS NULL) OR "
      "(remote_job_id IS NOT NULL AND remote_origin IS NOT NULL)))");
  execOrAbort(db.get(),
              "CREATE INDEX idx_ir_outbox_due ON ir_outbox("
              "local_result_ready, state, next_attempt_at_ms, id)");
  execOrAbort(db.get(),
              "CREATE INDEX idx_ir_outbox_attempt ON "
              "ir_outbox(provider_id, attempt_id)");
  execOrAbort(
      db.get(),
      "INSERT INTO ir_outbox(provider_id,attempt_id,chart_md5,chart_sha256,"
      "payload_json,state,local_result_ready,created_at_ms,updated_at_ms) "
      "VALUES('tachi','123e4567-e89b-42d3-a456-426614174099','" +
          std::string(32, 'b') + "','" + std::string(64, 'a') +
          "','{\"score\":123}',0,1,1000,1000)");
  execOrAbort(db.get(), "PRAGMA user_version=6");
  db.reset();

  assert(helper.EnsureSchema());
  const auto loaded = helper.LoadIrOutbox(
      "tachi", "123e4567-e89b-42d3-a456-426614174099");
  assert(loaded.status == ir::IrOutboxReadStatus::Found && loaded.entry);
  assert(loaded.entry->rulesetProof ==
         ir::IrRulesetProof{.rulesetId = "legacy-unknown"});
  assert(loaded.entry->state == ir::IrOutboxState::BlockedConfiguration);
  assert(loaded.entry->lastErrorCode == "legacy_ruleset_proof_missing");
  assert(loaded.entry->lastErrorMessage ==
         "Submission blocked because this queued score predates ruleset "
         "proof.");
  assert(loaded.entry->payloadJson == R"({"score":123})");
  assert(helper.ListDueIrOutbox(10'000).entries.empty());
  assert(helper.RetryIrOutbox(loaded.entry->id, 10'000).status ==
         ir::IrOutboxMutationStatus::Invalid);
  assert(helper.RetryAllIrOutbox("tachi", 10'000).affectedRows == 0);
  assert(helper.UnblockIrOutbox("tachi", 10'000).affectedRows == 0);
  helper.Shutdown();
  db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
  assert(columnExists(db.get(), "ir_outbox", "ruleset_id"));
  assert(columnExists(db.get(), "ir_outbox", "ruleset_revision"));
  assert(columnExists(db.get(), "ir_outbox", "validation_fingerprint"));
}

void testVersion7MigrationAddsIrRemotePollCount(
    const std::filesystem::path &root) {
  const auto path = root / "ir-outbox-poll-count-migration" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  helper.Shutdown();

  auto db = openDatabase(path);
  execOrAbort(db.get(), "DROP TABLE ir_outbox");
  execOrAbort(
      db.get(),
      "CREATE TABLE ir_outbox ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,provider_id TEXT NOT NULL,"
      "attempt_id TEXT NOT NULL,chart_md5 TEXT,chart_sha256 TEXT NOT NULL,"
      "payload_json TEXT NOT NULL,ruleset_id TEXT NOT NULL,"
      "ruleset_revision INTEGER NOT NULL,"
      "validation_fingerprint TEXT NOT NULL,state INTEGER NOT NULL,"
      "local_result_ready INTEGER NOT NULL DEFAULT 0,"
      "request_attempt_count INTEGER NOT NULL DEFAULT 0,"
      "consecutive_failure_count INTEGER NOT NULL DEFAULT 0,"
      "next_attempt_at_ms INTEGER,"
      "next_request_user_intent INTEGER NOT NULL DEFAULT 0,"
      "remote_job_id TEXT,remote_origin TEXT,last_error_code TEXT,"
      "last_error_message TEXT,created_at_ms INTEGER NOT NULL,"
      "updated_at_ms INTEGER NOT NULL,completed_at_ms INTEGER,"
      "UNIQUE(provider_id, attempt_id),CHECK (local_result_ready IN (0, 1)),"
      "CHECK (next_request_user_intent IN (0, 1)),"
      "CHECK ((remote_job_id IS NULL AND remote_origin IS NULL) OR "
      "(remote_job_id IS NOT NULL AND remote_origin IS NOT NULL)))");
  execOrAbort(db.get(),
              "CREATE INDEX idx_ir_outbox_due ON ir_outbox("
              "local_result_ready, state, next_attempt_at_ms, id)");
  execOrAbort(db.get(),
              "CREATE INDEX idx_ir_outbox_attempt ON "
              "ir_outbox(provider_id, attempt_id)");
  execOrAbort(
      db.get(),
      "INSERT INTO ir_outbox(provider_id,attempt_id,chart_md5,chart_sha256,"
      "payload_json,ruleset_id,ruleset_revision,validation_fingerprint,state,"
      "local_result_ready,next_attempt_at_ms,remote_job_id,remote_origin,"
      "created_at_ms,updated_at_ms) VALUES('tachi',"
      "'123e4567-e89b-42d3-a456-426614174098','" +
          std::string(32, 'b') + "','" + std::string(64, 'a') +
          "','{\"score\":123}','lr2',1,'" + std::string(64, 'c') +
          "',2,1,1200,'job-migrated','https://boku.tachi.ac',1000,1000)");
  execOrAbort(db.get(), "PRAGMA user_version=7");
  db.reset();

  assert(helper.EnsureSchema());
  const auto loaded = helper.LoadIrOutbox(
      "tachi", "123e4567-e89b-42d3-a456-426614174098");
  assert(loaded.status == ir::IrOutboxReadStatus::Found && loaded.entry);
  assert(loaded.entry->state == ir::IrOutboxState::AwaitingRemoteResult &&
         loaded.entry->remoteJobId == "job-migrated" &&
         loaded.entry->remotePollCount == 0);
  helper.Shutdown();
  db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
  assert(columnExists(db.get(), "ir_outbox", "remote_poll_count"));
}

void testCurrentVersionRejectsMalformedRulesetProofSchema(
    const std::filesystem::path &root) {
  const auto path = root / "ir-outbox-proof-malformed" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  helper.Shutdown();

  auto db = openDatabase(path);
  std::string tableSql = queryText(
      db.get(),
      "SELECT sql FROM sqlite_master WHERE type='table' AND name='ir_outbox'");
  const std::string exact = "validation_fingerprint TEXT NOT NULL";
  const auto proofColumn = tableSql.find(exact);
  assert(proofColumn != std::string::npos);
  tableSql.replace(proofColumn, exact.size(),
                   "validation_fingerprint TEXT");
  execOrAbort(db.get(), "DROP TABLE ir_outbox");
  execOrAbort(db.get(), tableSql);
  execOrAbort(db.get(),
              "CREATE INDEX idx_ir_outbox_due ON ir_outbox("
              "local_result_ready, state, next_attempt_at_ms, id)");
  execOrAbort(db.get(),
              "CREATE INDEX idx_ir_outbox_attempt ON "
              "ir_outbox(provider_id, attempt_id)");
  const std::string before = schemaSnapshot(db.get());
  db.reset();

  assert(!helper.EnsureSchema());
  db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
  assert(schemaSnapshot(db.get()) == before);
}

constexpr const char *kExpectedIrSubmissionReceiptsTableSql =
    "CREATE TABLE ir_submission_receipts ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "provider_id TEXT NOT NULL,"
    "server_origin TEXT NOT NULL,"
    "replay_id INTEGER NOT NULL,"
    "attempt_id TEXT NOT NULL,"
    "chart_md5 TEXT,"
    "chart_sha256 TEXT NOT NULL,"
    "remote_user_id INTEGER,"
    "remote_chart_id TEXT,"
    "remote_score_id TEXT,"
    "confirmation_source INTEGER NOT NULL,"
    "observed_in_snapshot INTEGER NOT NULL DEFAULT 0,"
    "confirmed_at_ms INTEGER NOT NULL,"
    "UNIQUE(provider_id, server_origin, replay_id),"
    "CHECK(observed_in_snapshot IN (0, 1)),"
    "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE"
    ")";

constexpr const char *kExpectedIrSubmissionReceiptsAttemptIndexSql =
    "CREATE INDEX idx_ir_submission_receipts_attempt ON "
    "ir_submission_receipts(provider_id, server_origin, attempt_id)";

constexpr const char *kExpectedIrSubmissionReceiptsRemoteScoreIndexSql =
    "CREATE INDEX idx_ir_submission_receipts_remote_score ON "
    "ir_submission_receipts(provider_id, server_origin, remote_score_id)";

void assertExactIrSubmissionReceiptSchema(sqlite3 *db) {
  assert(queryText(
             db,
             "SELECT sql FROM sqlite_master WHERE type='table' AND "
             "name='ir_submission_receipts'") ==
         kExpectedIrSubmissionReceiptsTableSql);
  assert(queryText(
             db,
             "SELECT sql FROM sqlite_master WHERE type='index' AND "
             "name='idx_ir_submission_receipts_attempt'") ==
         kExpectedIrSubmissionReceiptsAttemptIndexSql);
  assert(queryText(
             db,
             "SELECT sql FROM sqlite_master WHERE type='index' AND "
             "name='idx_ir_submission_receipts_remote_score'") ==
         kExpectedIrSubmissionReceiptsRemoteScoreIndexSql);
  assert(indexColumns(db, "idx_ir_submission_receipts_attempt") ==
         std::vector<std::string>({"provider_id", "server_origin",
                                   "attempt_id"}));
  assert(indexColumns(db, "idx_ir_submission_receipts_remote_score") ==
         std::vector<std::string>({"provider_id", "server_origin",
                                   "remote_score_id"}));
  assert(!columnExists(db, "ir_submission_receipts", "api_key"));
  assert(!columnExists(db, "ir_submission_receipts", "authorization"));
  assert(!columnExists(db, "ir_submission_receipts", "payload_json"));

  SqliteStatementHandle foreignKeys;
  assert(prepareSqliteStatement(
             db, "PRAGMA foreign_key_list(ir_submission_receipts)",
             foreignKeys) == SQLITE_OK);
  assert(sqlite3_step(foreignKeys.get()) == SQLITE_ROW);
  assert(queryText(db,
                   "SELECT \"table\" || '|' || \"from\" || '|' || "
                   "\"to\" || '|' || on_delete FROM "
                   "pragma_foreign_key_list('ir_submission_receipts')") ==
         "replays|replay_id|id|CASCADE");
  assert(sqlite3_step(foreignKeys.get()) == SQLITE_DONE);
}

constexpr const char *kExpectedIrRemoteScoresTableSql =
    "CREATE TABLE ir_remote_scores ("
    "provider_id TEXT NOT NULL,"
    "server_origin TEXT NOT NULL,"
    "remote_score_id TEXT NOT NULL,"
    "remote_user_id INTEGER NOT NULL,"
    "game TEXT NOT NULL,"
    "remote_chart_id TEXT NOT NULL,"
    "chart_md5 TEXT NOT NULL,"
    "chart_sha256 TEXT NOT NULL,"
    "title TEXT NOT NULL,"
    "artist TEXT NOT NULL,"
    "difficulty TEXT,"
    "level TEXT,"
    "level_number REAL,"
    "note_count INTEGER NOT NULL,"
    "score INTEGER NOT NULL,"
    "lamp_rank INTEGER NOT NULL,"
    "service TEXT NOT NULL,"
    "time_achieved_ms INTEGER,"
    "time_added_ms INTEGER NOT NULL,"
    "pgreat INTEGER,great INTEGER,good INTEGER,bad INTEGER,poor INTEGER,"
    "early_pgreat INTEGER,late_pgreat INTEGER,"
    "early_great INTEGER,late_great INTEGER,"
    "early_good INTEGER,late_good INTEGER,"
    "early_bad INTEGER,late_bad INTEGER,"
    "early_poor INTEGER,late_poor INTEGER,"
    "fast INTEGER,slow INTEGER,max_combo INTEGER,bad_points INTEGER,"
    "final_gauge REAL,"
    "gauge_history_json TEXT,"
    "random_mode TEXT,gauge_mode TEXT,input_device TEXT,client TEXT,"
    "sync_generation INTEGER NOT NULL,"
    "PRIMARY KEY(provider_id,server_origin,remote_score_id),"
    "CHECK(game IN ('bms-7k','bms-14k')),"
    "CHECK(remote_user_id > 0),"
    "CHECK(length(chart_md5)=32 AND chart_md5=lower(chart_md5) AND "
    "chart_md5 NOT GLOB '*[^0-9a-f]*'),"
    "CHECK(length(chart_sha256)=64 AND chart_sha256=lower(chart_sha256) AND "
    "chart_sha256 NOT GLOB '*[^0-9a-f]*'),"
    "CHECK(note_count >= 0 AND score >= 0 AND score <= note_count * 2),"
    "CHECK(lamp_rank IN (0,100,150,200,300,400,500,600)),"
    "CHECK(time_achieved_ms IS NULL OR time_achieved_ms > 0),"
    "CHECK(time_added_ms > 0),"
    "CHECK(pgreat IS NULL OR pgreat >= 0),"
    "CHECK(great IS NULL OR great >= 0),"
    "CHECK(good IS NULL OR good >= 0),"
    "CHECK(bad IS NULL OR bad >= 0),"
    "CHECK(poor IS NULL OR poor >= 0),"
    "CHECK(early_pgreat IS NULL OR early_pgreat >= 0),"
    "CHECK(late_pgreat IS NULL OR late_pgreat >= 0),"
    "CHECK(early_great IS NULL OR early_great >= 0),"
    "CHECK(late_great IS NULL OR late_great >= 0),"
    "CHECK(early_good IS NULL OR early_good >= 0),"
    "CHECK(late_good IS NULL OR late_good >= 0),"
    "CHECK(early_bad IS NULL OR early_bad >= 0),"
    "CHECK(late_bad IS NULL OR late_bad >= 0),"
    "CHECK(early_poor IS NULL OR early_poor >= 0),"
    "CHECK(late_poor IS NULL OR late_poor >= 0),"
    "CHECK(fast IS NULL OR fast >= 0),"
    "CHECK(slow IS NULL OR slow >= 0),"
    "CHECK(max_combo IS NULL OR max_combo >= 0),"
    "CHECK(bad_points IS NULL OR bad_points >= 0),"
    "CHECK(final_gauge IS NULL OR (final_gauge >= 0 AND final_gauge <= 100)),"
    "CHECK(sync_generation > 0)"
    ")";

constexpr const char *kExpectedIrRemoteScoresSha256IndexSql =
    "CREATE INDEX idx_ir_remote_scores_chart_sha256 ON "
    "ir_remote_scores(provider_id,server_origin,chart_sha256)";

constexpr const char *kExpectedIrRemoteScoresChartIdIndexSql =
    "CREATE INDEX idx_ir_remote_scores_remote_chart_id ON "
    "ir_remote_scores(provider_id,server_origin,remote_chart_id)";

void assertExactIrRemoteScoreSchema(sqlite3 *db) {
  assert(queryText(
             db,
             "SELECT sql FROM sqlite_master WHERE type='table' AND "
             "name='ir_remote_scores'") == kExpectedIrRemoteScoresTableSql);
  assert(queryText(
             db,
             "SELECT sql FROM sqlite_master WHERE type='index' AND "
             "name='idx_ir_remote_scores_chart_sha256'") ==
         kExpectedIrRemoteScoresSha256IndexSql);
  assert(queryText(
             db,
             "SELECT sql FROM sqlite_master WHERE type='index' AND "
             "name='idx_ir_remote_scores_remote_chart_id'") ==
         kExpectedIrRemoteScoresChartIdIndexSql);
  assert(indexColumns(db, "idx_ir_remote_scores_chart_sha256") ==
         std::vector<std::string>(
             {"provider_id", "server_origin", "chart_sha256"}));
  assert(indexColumns(db, "idx_ir_remote_scores_remote_chart_id") ==
         std::vector<std::string>(
             {"provider_id", "server_origin", "remote_chart_id"}));
  for (const char *forbidden : {"api_key", "authorization", "credential",
                                "raw_response", "payload_json"}) {
    assert(!columnExists(db, "ir_remote_scores", forbidden));
  }
}

void testFreshDatabaseCreatesIrRemoteScores(
    const std::filesystem::path &root) {
  const auto path = root / "fresh-ir-remote-scores" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  helper.Shutdown();

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") == 10);
  assertExactIrRemoteScoreSchema(db.get());
}

void testVersion9MigrationAddsIrRemoteScores(
    const std::filesystem::path &root) {
  const auto path = root / "version-9-ir-remote-scores" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  helper.Shutdown();

  auto db = openDatabase(path);
  execOrAbort(db.get(), "DROP TABLE ir_remote_scores");
  execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
  execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('version-9-kept')");
  execOrAbort(db.get(), "PRAGMA user_version=9");
  db.reset();

  assert(helper.EnsureSchema());
  helper.Shutdown();
  db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") == 10);
  assert(queryText(db.get(), "SELECT value FROM sentinel") ==
         "version-9-kept");
  assertExactIrRemoteScoreSchema(db.get());
}

void testCurrentVersionRejectsMalformedIrRemoteScoreSchema(
    const std::filesystem::path &root) {
  const auto path = root / "malformed-ir-remote-scores" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  helper.Shutdown();

  auto db = openDatabase(path);
  execOrAbort(db.get(), "DROP TABLE ir_remote_scores");
  execOrAbort(db.get(),
              "CREATE TABLE ir_remote_scores(sentinel TEXT NOT NULL)");
  execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
  execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('unchanged')");
  const auto before = schemaSnapshot(db.get());
  db.reset();

  assert(!helper.EnsureSchema());
  db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") == 10);
  assert(schemaSnapshot(db.get()) == before);
}

void testFreshDatabaseCreatesIrSubmissionReceipts(
    const std::filesystem::path &root) {
  const auto path = root / "fresh-ir-submission-receipts" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  helper.Shutdown();

  auto db = openDatabase(path);
  assert(ReplayRepository::kCurrentSchemaVersion == 10);
  assert(queryInt(db.get(), "PRAGMA user_version") == 10);
  assertExactIrSubmissionReceiptSchema(db.get());
}

void testVersion8MigrationAddsIrSubmissionReceipts(
    const std::filesystem::path &root) {
  const auto path = root / "version-8-ir-submission-receipts" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  helper.Shutdown();

  auto db = openDatabase(path);
  execOrAbort(db.get(), "DROP TABLE IF EXISTS ir_submission_receipts");
  execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
  execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('version-8-kept')");
  execOrAbort(db.get(), "PRAGMA user_version=8");
  db.reset();

  assert(helper.EnsureSchema());
  helper.Shutdown();
  db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") == 10);
  assert(queryText(db.get(), "SELECT value FROM sentinel") ==
         "version-8-kept");
  assertExactIrSubmissionReceiptSchema(db.get());
}

void testVersion8MigrationRejectsMalformedExistingOutbox(
    const std::filesystem::path &root) {
  const auto path = root / "version-8-malformed-existing-outbox" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  helper.Shutdown();

  auto db = openDatabase(path);
  execOrAbort(db.get(), "DROP TABLE ir_submission_receipts");
  execOrAbort(db.get(), "DROP INDEX idx_ir_outbox_due");
  execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
  execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('unchanged')");
  execOrAbort(db.get(), "PRAGMA user_version=8");
  db.reset();

  expectReplayMigrationRejectedWithoutMutation(path);
}

void testCurrentVersionRejectsMalformedIrSubmissionReceiptSchema(
    const std::filesystem::path &root) {
  const auto path = root / "malformed-ir-submission-receipts" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  helper.Shutdown();

  auto db = openDatabase(path);
  execOrAbort(db.get(), "DROP TABLE IF EXISTS ir_submission_receipts");
  execOrAbort(db.get(),
              "CREATE TABLE ir_submission_receipts(sentinel TEXT NOT NULL)");
  execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT NOT NULL)");
  execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('unchanged')");
  execOrAbort(db.get(), "PRAGMA user_version=" +
                            std::to_string(
                                ReplayRepository::kCurrentSchemaVersion));
  db.reset();

  expectReplayMigrationRejectedWithoutMutation(path);
}

void testIrOutboxInsertClaimAndDeliveryTransitions(
    const std::filesystem::path &root) {
  const auto path = root / "ir-outbox-transitions" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  const auto replayAttempt =
      sampleChartAttempt(root, "ir-outbox-transitions", 101);
  assert(helper.StageChartResult(replayAttempt, {}).status ==
         result_persistence::StageStatus::Staged);
  const auto draft = automaticIrDraft(replayAttempt, "tachi", 1'000);
  auto missingProof = draft;
  missingProof.rulesetProof = {};
  assert(helper.EnqueueReadyIrOutboxDraft(missingProof, true).status ==
         ir::IrOutboxInsertStatus::Invalid);
  auto partialProof = draft;
  partialProof.rulesetProof.validationFingerprint.clear();
  assert(helper.EnqueueReadyIrOutboxDraft(partialProof, true).status ==
         ir::IrOutboxInsertStatus::Invalid);
  const auto inserted = helper.EnqueueReadyIrOutboxDraft(draft, true);
  assert(inserted.status == ir::IrOutboxInsertStatus::Inserted);
  assert(inserted.entry && inserted.entry->payloadJson == draft.payloadJson &&
         inserted.entry->chartMd5 == draft.chartMd5 &&
         inserted.entry->chartSha256 == draft.chartSha256 &&
         inserted.entry->rulesetProof == draft.rulesetProof &&
         inserted.entry->state == ir::IrOutboxState::Pending &&
         inserted.entry->localResultReady &&
         inserted.entry->nextRequestUserIntent);
  const std::int64_t rowId = inserted.entry->id;

  const auto duplicate = helper.EnqueueReadyIrOutboxDraft(draft, true);
  assert(duplicate.status == ir::IrOutboxInsertStatus::AlreadyExists);
  auto conflictingDraft = draft;
  conflictingDraft.payloadJson = R"({"score":999})";
  assert(helper.EnqueueReadyIrOutboxDraft(conflictingDraft, true).status ==
         ir::IrOutboxInsertStatus::IntegrityConflict);
  conflictingDraft = draft;
  conflictingDraft.rulesetProof.validationFingerprint = std::string(64, 'e');
  assert(helper.EnqueueReadyIrOutboxDraft(conflictingDraft, true).status ==
         ir::IrOutboxInsertStatus::IntegrityConflict);

  const auto read = helper.LoadIrOutbox(draft.providerId, draft.attemptId);
  assert(read.status == ir::IrOutboxReadStatus::Found && read.entry &&
         *read.entry == *inserted.entry);
  const auto secondReady = helper.EnqueueReadyIrOutboxDraft(
      sampleIrDraft("123e4567-e89b-42d3-a456-426614174002", 1'001), false);
  assert(secondReady.entry);
  const auto blockedBeforeClaim = helper.EnqueueReadyIrOutboxDraft(
      sampleIrDraft("123e4567-e89b-42d3-a456-426614174003", 1'002), true);
  assert(blockedBeforeClaim.entry);
  assert(helper
             .BlockIrOutboxConfiguration(
                 blockedBeforeClaim.entry->id, ir::IrOutboxState::Pending,
                 "missing_api_key", "API key required", 1'050)
             .status == ir::IrOutboxMutationStatus::Updated);
  auto blockedBeforeClaimLoaded =
      helper.LoadIrOutbox(blockedBeforeClaim.entry->providerId,
                          blockedBeforeClaim.entry->attemptId);
  assert(blockedBeforeClaimLoaded.entry &&
         blockedBeforeClaimLoaded.entry->state ==
             ir::IrOutboxState::BlockedConfiguration &&
         blockedBeforeClaimLoaded.entry->nextRequestUserIntent &&
         blockedBeforeClaimLoaded.entry->requestAttemptCount == 0);
  assert(helper.UnblockIrOutbox("tachi", 1'060).affectedRows == 1);
  blockedBeforeClaimLoaded =
      helper.LoadIrOutbox(blockedBeforeClaim.entry->providerId,
                          blockedBeforeClaim.entry->attemptId);
  assert(blockedBeforeClaimLoaded.entry &&
         blockedBeforeClaimLoaded.entry->state == ir::IrOutboxState::Pending &&
         blockedBeforeClaimLoaded.entry->nextRequestUserIntent);
  assert(helper.DiscardIrOutbox(blockedBeforeClaim.entry->id).affectedRows ==
         1);
  const auto due = helper.ListDueIrOutbox(1'100);
  assert(due.status == ir::IrOutboxBatchStatus::Loaded &&
         due.entries.size() == 2 && due.entries.front().id == rowId &&
         due.entries.back().id == secondReady.entry->id);
  assert(helper.DiscardIrOutbox(secondReady.entry->id).affectedRows == 1);

  const auto claim =
      helper.ClaimIrOutbox(rowId, ir::IrOutboxState::Pending, 1'200);
  assert(claim.status == ir::IrOutboxClaimStatus::Claimed && claim.entry &&
         claim.consumedUserIntent &&
         claim.entry->state == ir::IrOutboxState::Uploading &&
         claim.entry->requestAttemptCount == 1 &&
         !claim.entry->nextRequestUserIntent);
  assert(
      helper.ClaimIrOutbox(rowId, ir::IrOutboxState::Pending, 1'201).status ==
      ir::IrOutboxClaimStatus::StateMismatch);

  const auto transient = helper.ApplyIrOutboxDelivery({
      .rowId = rowId,
      .nextState = ir::IrOutboxState::Pending,
      .consecutiveFailureCount = 1,
      .nextAttemptAtUnixMillis = 2'000,
      .lastErrorCode = "transport",
      .lastErrorMessage = std::string(700, 'x'),
      .updatedAtUnixMillis = 1'300,
  });
  assert(transient.status == ir::IrOutboxMutationStatus::Updated);
  auto loaded = helper.LoadIrOutbox(draft.providerId, draft.attemptId);
  assert(loaded.entry && loaded.entry->state == ir::IrOutboxState::Pending &&
         loaded.entry->consecutiveFailureCount == 1 &&
         loaded.entry->nextAttemptAtUnixMillis == 2'000 &&
         loaded.entry->lastErrorMessage.size() == ir::kMaximumDiagnosticBytes);
  assert(helper.ListDueIrOutbox(1'999).entries.empty());

  assert(
      helper.ClaimIrOutbox(rowId, ir::IrOutboxState::Pending, 2'000).status ==
      ir::IrOutboxClaimStatus::Claimed);
  assert(helper
             .ApplyIrOutboxDelivery({
                 .rowId = rowId,
                 .nextState = ir::IrOutboxState::BlockedConfiguration,
                 .consecutiveFailureCount = 0,
                 .lastErrorCode = "authentication_required",
                 .lastErrorMessage = "API key required",
                 .updatedAtUnixMillis = 2'001,
             })
             .status == ir::IrOutboxMutationStatus::Updated);
  loaded = helper.LoadIrOutbox(draft.providerId, draft.attemptId);
  assert(loaded.entry &&
         loaded.entry->state == ir::IrOutboxState::BlockedConfiguration);

  assert(helper.RetryIrOutbox(rowId, 2'100).status ==
         ir::IrOutboxMutationStatus::Updated);
  loaded = helper.LoadIrOutbox(draft.providerId, draft.attemptId);
  assert(loaded.entry && loaded.entry->state == ir::IrOutboxState::Pending &&
         loaded.entry->nextAttemptAtUnixMillis == 2'100 &&
         loaded.entry->nextRequestUserIntent &&
         loaded.entry->lastErrorCode.empty());

  assert(
      helper.ClaimIrOutbox(rowId, ir::IrOutboxState::Pending, 2'100).status ==
      ir::IrOutboxClaimStatus::Claimed);
  assert(helper
             .ApplyIrOutboxDelivery({
                 .rowId = rowId,
                 .nextState = ir::IrOutboxState::AwaitingRemoteResult,
                 .consecutiveFailureCount = 0,
                 .remotePollCount = 2,
                 .nextAttemptAtUnixMillis = 3'000,
                 .remoteJobId = "job-123",
                 .remoteOrigin = "https://boku.tachi.ac",
                 .updatedAtUnixMillis = 2'200,
             })
             .status == ir::IrOutboxMutationStatus::Updated);
  loaded = helper.LoadIrOutbox(draft.providerId, draft.attemptId);
  assert(loaded.entry &&
         loaded.entry->state == ir::IrOutboxState::AwaitingRemoteResult &&
         loaded.entry->remotePollCount == 2 &&
         loaded.entry->remoteJobId == "job-123" &&
         loaded.entry->remoteOrigin == "https://boku.tachi.ac");

  assert(helper
             .BlockIrOutboxConfiguration(
                 rowId, ir::IrOutboxState::AwaitingRemoteResult,
                 "authentication_failed", "API key rejected", 2'300)
             .status == ir::IrOutboxMutationStatus::Updated);
  loaded = helper.LoadIrOutbox(draft.providerId, draft.attemptId);
  assert(loaded.entry &&
         loaded.entry->state == ir::IrOutboxState::BlockedConfiguration &&
         loaded.entry->remotePollCount == 2 &&
         loaded.entry->remoteJobId == "job-123" &&
         loaded.entry->remoteOrigin == "https://boku.tachi.ac" &&
         !loaded.entry->nextRequestUserIntent);
  assert(helper.RetryIrOutbox(rowId, 2'301).status ==
         ir::IrOutboxMutationStatus::Updated);
  loaded = helper.LoadIrOutbox(draft.providerId, draft.attemptId);
  assert(loaded.entry &&
         loaded.entry->state == ir::IrOutboxState::AwaitingRemoteResult &&
         loaded.entry->remotePollCount == 2 &&
         loaded.entry->remoteJobId == "job-123" &&
         loaded.entry->remoteOrigin == "https://boku.tachi.ac" &&
         !loaded.entry->nextRequestUserIntent);

  const auto pollClaim = helper.ClaimIrOutbox(
      rowId, ir::IrOutboxState::AwaitingRemoteResult, 3'000);
  assert(pollClaim.status == ir::IrOutboxClaimStatus::Claimed &&
         !pollClaim.consumedUserIntent && pollClaim.entry &&
         pollClaim.entry->requestAttemptCount == 4);
  assert(helper
             .ApplyIrOutboxDelivery({
                 .rowId = rowId,
                 .nextState = ir::IrOutboxState::AwaitingRemoteResult,
                 .consecutiveFailureCount = 0,
                 .remotePollCount = 3,
                 .nextAttemptAtUnixMillis = 4'000,
                 .remoteJobId = "job-123",
                 .remoteOrigin = "https://boku.tachi.ac",
                 .updatedAtUnixMillis = 3'001,
             })
             .status == ir::IrOutboxMutationStatus::Updated);

  assert(
      helper
          .ClaimIrOutbox(rowId, ir::IrOutboxState::AwaitingRemoteResult, 4'000)
          .status == ir::IrOutboxClaimStatus::Claimed);
  assert(helper
             .ApplyIrOutboxDelivery({
                 .rowId = rowId,
                 .nextState = ir::IrOutboxState::Succeeded,
                 .consecutiveFailureCount = 0,
                 .remotePollCount = 0,
                 .updatedAtUnixMillis = 4'001,
                 .completedAtUnixMillis = 4'001,
                 .successfulReceipt =
                     ir::IrSuccessfulReceiptDraft{
                         .serverOrigin = "https://boku.tachi.ac",
                         .remoteUserId = 42,
                         .remoteScoreId = "Tscore",
                         .confirmedAtUnixMillis = 4'001,
                     },
             })
             .status == ir::IrOutboxMutationStatus::Updated);
  loaded = helper.LoadIrOutbox(draft.providerId, draft.attemptId);
  assert(loaded.entry && loaded.entry->state == ir::IrOutboxState::Succeeded &&
         loaded.entry->remotePollCount == 0 &&
         loaded.entry->completedAtUnixMillis == 4'001 &&
         loaded.entry->remoteJobId.empty());
  assert(helper.PurgeSucceededIrOutbox(4'002).affectedRows == 1);
  assert(helper.LoadIrOutbox(draft.providerId, draft.attemptId).status ==
         ir::IrOutboxReadStatus::NotFound);
}

struct ClaimedCanonicalIrAttempt {
  std::string attemptId;
  std::string chartMd5;
  std::string chartSha256;
  bms_parser::ChartMeta chartMeta;
  int replayId = 0;
  std::int64_t rowId = 0;
};

ClaimedCanonicalIrAttempt stageActivateAndClaimIrAttempt(
    ReplayRepository &helper, const std::filesystem::path &root,
    std::string_view fixtureName, int suffix,
    std::string_view providerId = "tachi") {
  const auto attempt =
      sampleChartAttempt(root, std::string(fixtureName), suffix);
  const auto draft = automaticIrDraft(attempt, std::string(providerId), 1'000);
  const auto staged = helper.StageChartResult(attempt, {&draft, 1});
  assert(staged.status == result_persistence::StageStatus::Staged &&
         staged.receipt);
  assert(helper
             .AcknowledgePendingChartScoreAndActivateIr(attempt.attemptId,
                                                       staged.receipt->replayId)
             .status == result_persistence::AcknowledgeStatus::Acknowledged);
  const auto outbox = helper.LoadIrOutbox(providerId, attempt.attemptId);
  assert(outbox.status == ir::IrOutboxReadStatus::Found && outbox.entry);
  assert(helper
             .ClaimIrOutbox(outbox.entry->id, ir::IrOutboxState::Pending,
                            1'500)
             .status == ir::IrOutboxClaimStatus::Claimed);
  return {.attemptId = attempt.attemptId,
          .chartMd5 = outbox.entry->chartMd5,
          .chartSha256 = outbox.entry->chartSha256,
          .chartMeta = attempt.replay.chartMeta,
          .replayId = staged.receipt->replayId,
          .rowId = outbox.entry->id};
}

ir::IrOutboxDeliveryUpdate
successfulDelivery(std::int64_t rowId,
                   std::string serverOrigin = "https://boku.tachi.ac") {
  return {
      .rowId = rowId,
      .nextState = ir::IrOutboxState::Succeeded,
      .consecutiveFailureCount = 0,
      .remotePollCount = 0,
      .lastErrorCode = {},
      .lastErrorMessage = {},
      .updatedAtUnixMillis = 2'000,
      .completedAtUnixMillis = 2'000,
      .successfulReceipt =
          ir::IrSuccessfulReceiptDraft{
              .serverOrigin = std::move(serverOrigin),
              .remoteUserId = 42,
              .remoteScoreId = "Tscore",
              .confirmedAtUnixMillis = 2'000,
          },
  };
}

void testLoadIrReconciliationCandidatesReturnsCanonicalScopedEvidence(
    const std::filesystem::path &root) {
  const auto path = root / "ir-reconciliation-candidates" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  const auto represented = stageActivateAndClaimIrAttempt(
      helper, root, "ir-reconciliation-represented", 130);
  assert(helper.ApplyIrOutboxDelivery(successfulDelivery(represented.rowId))
             .status == ir::IrOutboxMutationStatus::Updated);
  const auto representedAttempt =
      sampleChartAttempt(root, "ir-reconciliation-represented", 130);
  const auto otherProviderDraft =
      automaticIrDraft(representedAttempt, "other", 2'100);
  assert(helper.EnqueueReadyIrOutboxDraft(otherProviderDraft, false).status ==
         ir::IrOutboxInsertStatus::Inserted);

  const auto otherOrigin = stageActivateAndClaimIrAttempt(
      helper, root, "ir-reconciliation-other-origin", 132);
  assert(helper
             .ApplyIrOutboxDelivery(successfulDelivery(
                 otherOrigin.rowId, "https://other.example"))
             .status == ir::IrOutboxMutationStatus::Updated);

  auto modified =
      sampleChartAttempt(root, "ir-reconciliation-modified", 131);
  modified.replay.provenance.playback = {
      .percent = 75, .mode = audio::PlaybackMode::PitchShift};
  modified.replay.provenance.eligibility = ScoreEligibility::Modified;
  modified.replay.clearType = kClearTypeAssistedEasyClearRank;
  modified.score.clearType = kClearTypeAssistedEasyClearRank;
  modified.score.provenance = modified.replay.provenance;
  modified.payloadFingerprint =
      result_persistence::payloadFingerprint(modified.replay, modified.score);
  const auto modifiedStage = helper.StageChartResult(modified, {});
  assert(modifiedStage.status == result_persistence::StageStatus::Staged &&
         modifiedStage.receipt);
  assert(helper
             .AcknowledgePendingChartScoreAndActivateIr(
                 modified.attemptId, modifiedStage.receipt->replayId)
             .status == result_persistence::AcknowledgeStatus::Acknowledged);

  helper.Shutdown();
  auto db = openDatabase(path);
  execOrAbort(db.get(),
              "UPDATE ir_outbox SET payload_json='"
              "{\"meta\":{\"game\":\"bms\",\"playtype\":\"7K\"},"
              "\"scores\":[]}' WHERE id=" +
                  std::to_string(represented.rowId));
  db.reset();

  const auto invalid = helper.LoadIrReconciliationCandidates(
      "tachi", "HTTPS://BOKU.TACHI.AC:443/");
  assert(invalid.status == ir::IrReconciliationReadOutcome::Status::Invalid &&
         invalid.candidates.empty());

  const auto loaded = helper.LoadIrReconciliationCandidates(
      "tachi", "https://boku.tachi.ac");
  assert(loaded.status == ir::IrReconciliationReadOutcome::Status::Loaded);
  assert(loaded.diagnostic.empty());
  assert(loaded.candidates.size() == 3);
  const auto representedCandidate = std::ranges::find(
      loaded.candidates, represented.replayId,
      &ir::IrLocalReceiptCandidate::replayId);
  assert(representedCandidate != loaded.candidates.end());
  assert(representedCandidate->attemptId == represented.attemptId);
  assert(representedCandidate->keyMode == 7);
  assert(representedCandidate->chartMd5 == represented.chartMd5);
  assert(representedCandidate->chartSha256 == represented.chartSha256);
  assert(representedCandidate->score == 91);
  assert(representedCandidate->lampRank == kClearTypeHardClearRank);
  assert(representedCandidate->eligible);
  assert(representedCandidate->currentReceipt &&
         representedCandidate->currentReceipt->providerId == "tachi" &&
         representedCandidate->currentReceipt->serverOrigin ==
             "https://boku.tachi.ac" &&
         representedCandidate->currentReceipt->remoteScoreId == "Tscore");
  assert(representedCandidate->outboxRowId == represented.rowId);
  assert(representedCandidate->outboxState == ir::IrOutboxState::Succeeded);

  const auto modifiedCandidate = std::ranges::find(
      loaded.candidates, modifiedStage.receipt->replayId,
      &ir::IrLocalReceiptCandidate::replayId);
  assert(modifiedCandidate != loaded.candidates.end());
  assert(!modifiedCandidate->eligible);
  assert(!modifiedCandidate->currentReceipt);
  assert(!modifiedCandidate->outboxRowId);
  assert(!modifiedCandidate->outboxState);

  const auto otherOriginCandidate = std::ranges::find(
      loaded.candidates, otherOrigin.replayId,
      &ir::IrLocalReceiptCandidate::replayId);
  assert(otherOriginCandidate != loaded.candidates.end());
  assert(!otherOriginCandidate->currentReceipt);
  assert(!otherOriginCandidate->outboxRowId);
  assert(!otherOriginCandidate->outboxState);
  const auto retainedOtherOriginReceipt = helper.LoadIrSubmissionReceipt(
      "tachi", "https://other.example", otherOrigin.attemptId);
  assert(retainedOtherOriginReceipt.status == ir::IrReceiptReadStatus::Found &&
         retainedOtherOriginReceipt.receipt &&
         retainedOtherOriginReceipt.receipt->remoteScoreId == "Tscore");
  const auto retainedOtherOriginOutbox =
      helper.LoadIrOutbox("tachi", otherOrigin.attemptId);
  assert(retainedOtherOriginOutbox.status == ir::IrOutboxReadStatus::Found &&
         retainedOtherOriginOutbox.entry &&
         retainedOtherOriginOutbox.entry->id == otherOrigin.rowId &&
         retainedOtherOriginOutbox.entry->state ==
             ir::IrOutboxState::Succeeded);
}

void testLoadIrReconciliationCandidatesSkipsCorruptionWithBoundedDiagnostic(
    const std::filesystem::path &root) {
  const auto path = root / "ir-reconciliation-corrupt" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  const auto valid =
      sampleChartAttempt(root, "ir-reconciliation-valid", 133);
  const auto corrupt =
      sampleChartAttempt(root, "ir-reconciliation-corrupt", 134);
  const auto validStage = helper.StageChartResult(valid, {});
  const auto corruptStage = helper.StageChartResult(corrupt, {});
  assert(validStage.receipt && corruptStage.receipt);
  assert(helper
             .AcknowledgePendingChartScoreAndActivateIr(
                 valid.attemptId, validStage.receipt->replayId)
             .status == result_persistence::AcknowledgeStatus::Acknowledged);
  assert(helper
             .AcknowledgePendingChartScoreAndActivateIr(
                 corrupt.attemptId, corruptStage.receipt->replayId)
             .status == result_persistence::AcknowledgeStatus::Acknowledged);
  helper.Shutdown();
  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE replays SET provenance_json='{' WHERE id=" +
                            std::to_string(corruptStage.receipt->replayId));
  db.reset();

  const auto loaded = helper.LoadIrReconciliationCandidates(
      "tachi", "https://boku.tachi.ac");
  assert(loaded.status == ir::IrReconciliationReadOutcome::Status::Loaded);
  assert(loaded.candidates.size() == 1);
  assert(loaded.candidates.front().replayId == validStage.receipt->replayId);
  assert(!loaded.diagnostic.empty());
  assert(loaded.diagnostic.size() <= ir::kMaximumDiagnosticBytes);
  assert(loaded.diagnostic.find("inspected=2") != std::string::npos);
  assert(loaded.diagnostic.find("rejected=1") != std::string::npos);
}

void testLoadIrReconciliationCandidatesStorageFailureReturnsNoPartialRows(
    const std::filesystem::path &root) {
  const auto path = root / "ir-reconciliation-storage-failure" / "replay.db";
  ReplayRepository helper(path);
  const auto attempt =
      sampleChartAttempt(root, "ir-reconciliation-storage-failure", 135);
  const auto staged = helper.StageChartResult(attempt, {});
  assert(staged.receipt);
  assert(helper
             .AcknowledgePendingChartScoreAndActivateIr(
                 attempt.attemptId, staged.receipt->replayId)
             .status == result_persistence::AcknowledgeStatus::Acknowledged);
  helper.Shutdown();
  auto db = openDatabase(path);
  execOrAbort(db.get(), "DROP TABLE ir_submission_receipts");
  db.reset();

  const auto loaded = helper.LoadIrReconciliationCandidates(
      "tachi", "https://boku.tachi.ac");
  assert(loaded.status ==
         ir::IrReconciliationReadOutcome::Status::StorageFailure);
  assert(loaded.candidates.empty());
  assert(!loaded.diagnostic.empty());
}

void testLoadIrReconciliationCandidatesUsesOneReadSnapshot(
    const std::filesystem::path &root) {
  const auto path = root / "ir-reconciliation-snapshot" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  const auto represented = stageActivateAndClaimIrAttempt(
      helper, root, "ir-reconciliation-snapshot", 136);
  assert(helper.ApplyIrOutboxDelivery(successfulDelivery(represented.rowId))
             .status == ir::IrOutboxMutationStatus::Updated);
  helper.Shutdown();

  constexpr int duplicateCount = 4'000;
  auto db = openDatabase(path);
  execOrAbort(
      db.get(),
      "WITH RECURSIVE seq(value) AS (SELECT 1 UNION ALL SELECT value+1 "
      "FROM seq WHERE value<" +
          std::to_string(duplicateCount) +
          ") INSERT INTO replays("
          "chart_path,chart_md5,chart_sha256,chart_title,chart_artist,ln_mode,"
          "gauge_type,gauge_auto_shift,final_score,max_combo,final_gauge,"
          "clear_type,assist_option,ruleset_version,eligibility,"
          "provenance_json,attempt_id,attempt_fingerprint) SELECT "
          "chart_path,chart_md5,chart_sha256,chart_title,chart_artist,ln_mode,"
          "gauge_type,gauge_auto_shift,final_score,max_combo,final_gauge,"
          "clear_type,assist_option,ruleset_version,eligibility,"
          "provenance_json,printf('20000000-0000-4000-8000-%012x',value),"
          "NULL FROM replays,seq WHERE id=" +
          std::to_string(represented.replayId));
  execOrAbort(
      db.get(),
      "INSERT INTO ir_submission_receipts("
      "provider_id,server_origin,replay_id,attempt_id,chart_md5,chart_sha256,"
      "remote_user_id,remote_chart_id,remote_score_id,confirmation_source,"
      "observed_in_snapshot,confirmed_at_ms) SELECT "
      "'tachi','https://boku.tachi.ac',id,attempt_id,chart_md5,chart_sha256,"
      "42,'snapshot-chart',printf('snapshot-score-%d',id),0,0,2000 "
      "FROM replays WHERE id<>" +
          std::to_string(represented.replayId));
  db.reset();
  assert(helper.EnsureSchema());

  ir::IrReconciliationReadOutcome loaded;
  std::thread reader([&] {
    loaded = helper.LoadIrReconciliationCandidates(
        "tachi", "https://boku.tachi.ac");
  });
  bool observedRead = false;
  for (int attempt = 0; attempt < 100'000; ++attempt) {
    if (ReplayRepository::HasActiveReads()) {
      observedRead = true;
      break;
    }
    std::this_thread::yield();
  }
  assert(observedRead);
  db = openDatabase(path);
  sqlite3_busy_timeout(db.get(), 5'000);
  execOrAbort(db.get(), "DELETE FROM ir_submission_receipts");
  db.reset();
  reader.join();

  assert(loaded.status == ir::IrReconciliationReadOutcome::Status::Loaded);
  assert(loaded.candidates.size() ==
         static_cast<std::size_t>(duplicateCount + 1));
  const auto receiptCount = std::ranges::count_if(
      loaded.candidates,
      [](const auto &candidate) { return candidate.currentReceipt.has_value(); });
  assert(receiptCount == 0 || receiptCount == duplicateCount + 1);
}

void testLoadIrReconciliationCandidatesKeepsUnknownOutboxKeyMode(
    const std::filesystem::path &root) {
  const auto path = root / "ir-reconciliation-unknown-mode" / "replay.db";
  ReplayRepository helper(path);
  const auto attempt =
      sampleChartAttempt(root, "ir-reconciliation-unknown-mode", 137);
  const auto draft = automaticIrDraft(attempt, "tachi", 1'000);
  const auto staged = helper.StageChartResult(attempt, {&draft, 1});
  assert(staged.receipt);
  assert(helper
             .AcknowledgePendingChartScoreAndActivateIr(
                 attempt.attemptId, staged.receipt->replayId)
             .status == result_persistence::AcknowledgeStatus::Acknowledged);
  const auto outbox = helper.LoadIrOutbox("tachi", attempt.attemptId);
  assert(outbox.entry);
  helper.Shutdown();
  auto db = openDatabase(path);
  execOrAbort(db.get(),
              "UPDATE ir_outbox SET payload_json='"
              "{\"meta\":{\"game\":7,\"playtype\":[]}}' WHERE id=" +
                  std::to_string(outbox.entry->id));
  db.reset();

  const auto loaded = helper.LoadIrReconciliationCandidates(
      "tachi", "https://boku.tachi.ac");
  assert(loaded.status == ir::IrReconciliationReadOutcome::Status::Loaded);
  assert(loaded.candidates.size() == 1);
  assert(loaded.candidates.front().keyMode == 0);
  assert(loaded.candidates.front().outboxRowId == outbox.entry->id);
}

void assertClaimedWithoutReceipt(ReplayRepository &helper,
                                 const ClaimedCanonicalIrAttempt &fixture) {
  const auto outbox = helper.LoadIrOutbox("tachi", fixture.attemptId);
  assert(outbox.status == ir::IrOutboxReadStatus::Found && outbox.entry &&
         outbox.entry->state == ir::IrOutboxState::Uploading);
  assert(helper
             .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                      fixture.attemptId)
             .status == ir::IrReceiptReadStatus::NotFound);
}

void testIrOutboxSuccessCommitsReceiptAtomically(
    const std::filesystem::path &root) {
  {
    const auto path = root / "ir-receipt-success" / "replay.db";
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
    const auto fixture = stageActivateAndClaimIrAttempt(
        helper, root, "ir-receipt-success", 102);

    const auto applied =
        helper.ApplyIrOutboxDelivery(successfulDelivery(fixture.rowId));
    assert(applied.status == ir::IrOutboxMutationStatus::Updated);
    const auto outbox = helper.LoadIrOutbox("tachi", fixture.attemptId);
    assert(outbox.status == ir::IrOutboxReadStatus::Found && outbox.entry &&
           outbox.entry->state == ir::IrOutboxState::Succeeded);
    const auto receipt = helper.LoadIrSubmissionReceipt(
        "tachi", "https://boku.tachi.ac", fixture.attemptId);
    assert(receipt.status == ir::IrReceiptReadStatus::Found &&
           receipt.receipt && receipt.receipt->providerId == "tachi" &&
           receipt.receipt->serverOrigin == "https://boku.tachi.ac" &&
           receipt.receipt->replayId == fixture.replayId &&
           receipt.receipt->attemptId == fixture.attemptId &&
           receipt.receipt->chartMd5 == fixture.chartMd5 &&
           receipt.receipt->chartSha256 == fixture.chartSha256 &&
           receipt.receipt->remoteUserId == 42 &&
           receipt.receipt->remoteScoreId == "Tscore" &&
           receipt.receipt->source ==
               ir::IrReceiptConfirmationSource::Submission &&
           !receipt.receipt->observedInSnapshot &&
           receipt.receipt->confirmedAtUnixMillis == 2'000);

    helper.Shutdown();
    auto db = openDatabase(path);
    execOrAbort(db.get(), "PRAGMA foreign_keys=ON");
    execOrAbort(db.get(), "DELETE FROM replays WHERE id=" +
                              std::to_string(fixture.replayId));
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM ir_submission_receipts") ==
           0);
  }

  {
    ReplayRepository helper(root / "ir-receipt-missing" / "replay.db");
    assert(helper.EnsureSchema());
    const auto fixture = stageActivateAndClaimIrAttempt(
        helper, root, "ir-receipt-missing", 103);
    auto update = successfulDelivery(fixture.rowId);
    update.successfulReceipt.reset();
    assert(helper.ApplyIrOutboxDelivery(update).status ==
           ir::IrOutboxMutationStatus::Invalid);
    assertClaimedWithoutReceipt(helper, fixture);
  }

  {
    ReplayRepository helper(root / "ir-receipt-missing-replay" / "replay.db");
    assert(helper.EnsureSchema());
    const auto draft =
        sampleIrDraft("123e4567-e89b-42d3-a456-426614174104", 1'000);
    const auto inserted = helper.EnqueueReadyIrOutboxDraft(draft, false);
    assert(inserted.status == ir::IrOutboxInsertStatus::Inserted &&
           inserted.entry);
    assert(helper
               .ClaimIrOutbox(inserted.entry->id, ir::IrOutboxState::Pending,
                              1'500)
               .status == ir::IrOutboxClaimStatus::Claimed);
    assert(helper
               .ApplyIrOutboxDelivery(successfulDelivery(inserted.entry->id))
               .status == ir::IrOutboxMutationStatus::StorageFailure);
    const ClaimedCanonicalIrAttempt fixture{
        .attemptId = draft.attemptId, .rowId = inserted.entry->id};
    assertClaimedWithoutReceipt(helper, fixture);
  }

  {
    ReplayRepository helper(root / "ir-receipt-invalid-origin" / "replay.db");
    assert(helper.EnsureSchema());
    const auto fixture = stageActivateAndClaimIrAttempt(
        helper, root, "ir-receipt-invalid-origin", 105);
    auto update = successfulDelivery(fixture.rowId);
    update.successfulReceipt->serverOrigin =
        "HTTPS://BOKU.TACHI.AC:443/";
    assert(helper.ApplyIrOutboxDelivery(update).status ==
           ir::IrOutboxMutationStatus::Invalid);
    assertClaimedWithoutReceipt(helper, fixture);
  }

  {
    const auto path = root / "ir-receipt-trigger-rollback" / "replay.db";
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
    const auto fixture = stageActivateAndClaimIrAttempt(
        helper, root, "ir-receipt-trigger-rollback", 106);
    helper.Shutdown();
    auto db = openDatabase(path);
    execOrAbort(
        db.get(),
        "CREATE TRIGGER fail_ir_receipt BEFORE INSERT ON "
        "ir_submission_receipts BEGIN SELECT RAISE(ABORT, "
        "'forced receipt failure'); END");
    db.reset();

    assert(helper
               .ApplyIrOutboxDelivery(successfulDelivery(fixture.rowId))
               .status == ir::IrOutboxMutationStatus::StorageFailure);
    assertClaimedWithoutReceipt(helper, fixture);
  }
}

void testClearIrSubmissionReceiptsIsOriginScoped(
    const std::filesystem::path &root) {
  const auto path = root / "ir-receipt-clear" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  const auto target = stageActivateAndClaimIrAttempt(
      helper, root, "ir-receipt-clear-target", 107);
  const auto otherOrigin = stageActivateAndClaimIrAttempt(
      helper, root, "ir-receipt-clear-origin", 108);
  const auto otherProvider = stageActivateAndClaimIrAttempt(
      helper, root, "ir-receipt-clear-provider", 109, "other");
  const auto unfinishedAttempt =
      sampleChartAttempt(root, "ir-receipt-clear-unfinished", 110);
  const auto unfinishedDraft =
      automaticIrDraft(unfinishedAttempt, "tachi", 1'000);
  const auto unfinishedStage =
      helper.StageChartResult(unfinishedAttempt, {&unfinishedDraft, 1});
  assert(unfinishedStage.status == result_persistence::StageStatus::Staged &&
         unfinishedStage.receipt);
  assert(helper
             .AcknowledgePendingChartScoreAndActivateIr(
                 unfinishedAttempt.attemptId, unfinishedStage.receipt->replayId)
             .status == result_persistence::AcknowledgeStatus::Acknowledged);
  const auto legacyAttempt =
      sampleChartAttempt(root, "ir-receipt-clear-legacy", 111);
  const auto legacyDraft = automaticIrDraft(legacyAttempt, "tachi", 1'000);
  const auto legacyStage =
      helper.StageChartResult(legacyAttempt, {&legacyDraft, 1});
  assert(legacyStage.status == result_persistence::StageStatus::Staged &&
         legacyStage.receipt);
  assert(helper
             .AcknowledgePendingChartScoreAndActivateIr(
                 legacyAttempt.attemptId, legacyStage.receipt->replayId)
             .status == result_persistence::AcknowledgeStatus::Acknowledged);
  const auto legacyOutbox =
      helper.LoadIrOutbox("tachi", legacyAttempt.attemptId);
  assert(legacyOutbox.entry);
  assert(helper
             .ClaimIrOutbox(legacyOutbox.entry->id,
                            ir::IrOutboxState::Pending, 1'500)
             .status == ir::IrOutboxClaimStatus::Claimed);
  assert(helper.ApplyIrOutboxDelivery(successfulDelivery(target.rowId)).status ==
         ir::IrOutboxMutationStatus::Updated);
  assert(helper
             .ApplyIrOutboxDelivery(successfulDelivery(
                 otherOrigin.rowId, "https://other.example"))
             .status == ir::IrOutboxMutationStatus::Updated);
  assert(helper
             .ApplyIrOutboxDelivery(successfulDelivery(otherProvider.rowId))
             .status == ir::IrOutboxMutationStatus::Updated);

  helper.Shutdown();
  {
    auto db = openDatabase(path);
    execOrAbort(db.get(),
                "UPDATE ir_outbox SET state=5,completed_at_ms=2000,"
                "updated_at_ms=2000 WHERE id=" +
                    std::to_string(legacyOutbox.entry->id));
  }

  const auto before = helper.ListReplays(target.chartMeta, 0, "tachi",
                                         "https://boku.tachi.ac");
  assert(before.size() == 1 && before.front().hasIrReceipt &&
         ir::resolveIrRecordState(
             {.eligible = true,
              .hasReceipt = before.front().hasIrReceipt,
              .outboxState = before.front().requestedIrOutboxState}) ==
             ir::IrRecordState::Uploaded);

  assert(helper
             .ClearIrSubmissionReceipts("tachi",
                                        "HTTPS://BOKU.TACHI.AC:443/")
             .status == ir::IrOutboxMutationStatus::Invalid);
  assert(helper
             .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                      target.attemptId)
             .status == ir::IrReceiptReadStatus::Found);

  const auto cleared = helper.ClearIrSubmissionReceipts(
      "tachi", "https://boku.tachi.ac");
  assert(cleared.status == ir::IrOutboxMutationStatus::Updated &&
         cleared.affectedRows == 2);
  assert(helper
             .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                      target.attemptId)
             .status == ir::IrReceiptReadStatus::NotFound);
  assert(helper
             .LoadIrSubmissionReceipt("tachi", "https://other.example",
                                      otherOrigin.attemptId)
             .status == ir::IrReceiptReadStatus::Found);
  assert(helper
             .LoadIrSubmissionReceipt("other", "https://boku.tachi.ac",
                                      otherProvider.attemptId)
             .status == ir::IrReceiptReadStatus::Found);
  assert(helper.LoadIrOutbox("tachi", target.attemptId).status ==
         ir::IrOutboxReadStatus::NotFound);
  assert(helper.LoadIrOutbox("tachi", otherOrigin.attemptId).status ==
         ir::IrOutboxReadStatus::Found);
  assert(helper.LoadIrOutbox("other", otherProvider.attemptId).status ==
         ir::IrOutboxReadStatus::Found);
  assert(helper.LoadIrOutbox("tachi", unfinishedAttempt.attemptId).status ==
         ir::IrOutboxReadStatus::Found);
  const auto retainedLegacy =
      helper.LoadIrOutbox("tachi", legacyAttempt.attemptId);
  assert(retainedLegacy.entry &&
         retainedLegacy.entry->state == ir::IrOutboxState::Succeeded);

  const auto after = helper.ListReplays(target.chartMeta, 0, "tachi",
                                        "https://boku.tachi.ac");
  assert(after.size() == 1 && !after.front().hasIrReceipt &&
         !after.front().requestedIrOutboxState &&
         ir::resolveIrRecordState(
             {.eligible = true,
              .hasReceipt = after.front().hasIrReceipt,
              .outboxState = after.front().requestedIrOutboxState}) ==
             ir::IrRecordState::Eligible);
  const auto legacySummary = helper.ListReplays(
      legacyAttempt.replay.chartMeta, 0, "tachi", "https://boku.tachi.ac");
  assert(legacySummary.size() == 1 && !legacySummary.front().hasIrReceipt &&
         legacySummary.front().requestedIrOutboxState ==
             ir::IrOutboxState::Succeeded &&
         ir::resolveIrRecordState(
             {.eligible = true,
              .hasReceipt = legacySummary.front().hasIrReceipt,
              .outboxState = legacySummary.front().requestedIrOutboxState}) ==
             ir::IrRecordState::Uploaded);
  assert(helper
             .ClearIrSubmissionReceipts("tachi", "https://boku.tachi.ac")
             .status == ir::IrOutboxMutationStatus::NotFound);

  helper.Shutdown();
  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == 5);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM ir_outbox") == 4);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM ir_submission_receipts") ==
         2);
}

void testClearIrSubmissionReceiptsRollsBackBothDeletes(
    const std::filesystem::path &root) {
  const auto path = root / "ir-receipt-clear-rollback" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  const auto target = stageActivateAndClaimIrAttempt(
      helper, root, "ir-receipt-clear-rollback", 112);
  assert(helper.ApplyIrOutboxDelivery(successfulDelivery(target.rowId)).status ==
         ir::IrOutboxMutationStatus::Updated);
  helper.Shutdown();
  {
    auto db = openDatabase(path);
    execOrAbort(
        db.get(),
        "CREATE TRIGGER fail_ir_receipt_clear BEFORE DELETE ON "
        "ir_submission_receipts BEGIN SELECT RAISE(ABORT,"
        "'forced receipt clear failure'); END");
  }

  const auto cleared = helper.ClearIrSubmissionReceipts(
      "tachi", "https://boku.tachi.ac");
  assert(cleared.status == ir::IrOutboxMutationStatus::StorageFailure);
  assert(helper.LoadIrOutbox("tachi", target.attemptId).status ==
         ir::IrOutboxReadStatus::Found);
  assert(helper
             .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                      target.attemptId)
             .status == ir::IrReceiptReadStatus::Found);
}

ir::IrRemoteScore sampleIrRemoteScore(std::string remoteScoreId,
                                      char md5Digit, char sha256Digit,
                                      int score = 150) {
  ir::IrRemoteScore remote;
  remote.remoteUserId = 42;
  remote.game = "bms-7k";
  remote.remoteScoreId = std::move(remoteScoreId);
  remote.remoteChartId = "remote-chart";
  remote.chartMd5 = std::string(32, md5Digit);
  remote.chartSha256 = std::string(64, sha256Digit);
  remote.title = "Remote title";
  remote.artist = "Remote artist";
  remote.service = "Bokutachi";
  remote.difficulty = "ANOTHER";
  remote.level = "12";
  remote.levelNumber = 12.5;
  remote.noteCount = 100;
  remote.score = score;
  remote.lampRank = kClearTypeNormalClearRank;
  remote.timeAchievedUnixMillis = 900;
  remote.timeAddedUnixMillis = 1'000;
  remote.judgements.pGreat = 60;
  remote.judgements.great = 30;
  remote.judgements.good = 5;
  remote.judgements.bad = 3;
  remote.judgements.poor = 2;
  remote.timing.earlyPGreat = 20;
  remote.timing.latePGreat = 40;
  remote.timing.earlyGreat = 10;
  remote.timing.lateGreat = 20;
  remote.timing.earlyGood = 2;
  remote.timing.lateGood = 3;
  remote.timing.earlyBad = 1;
  remote.timing.lateBad = 2;
  remote.timing.earlyPoor = 0;
  remote.timing.latePoor = 2;
  remote.fast = 11;
  remote.slow = 12;
  remote.maxCombo = 80;
  remote.badPoints = 7;
  remote.finalGauge = 78.5f;
  remote.gaugeHistory = {0.0f, std::nullopt, 100.0f};
  remote.random = "RANDOM";
  remote.gauge = "HARD";
  remote.inputDevice = "keyboard";
  remote.client = "client";
  return remote;
}

ir::IrRemoteSnapshotMutation sampleIrRemoteMutation(
    std::string providerId, std::string serverOrigin,
    std::int64_t synchronizedAtUnixMillis,
    std::vector<ir::IrRemoteScore> scores) {
  return {
      .providerId = std::move(providerId),
      .serverOrigin = std::move(serverOrigin),
      .synchronizedAtUnixMillis = synchronizedAtUnixMillis,
      .scores = std::move(scores),
  };
}

std::vector<std::string> remoteScoreIds(
    ReplayRepository &helper, std::string_view providerId = "tachi",
    std::string_view serverOrigin = "https://boku.tachi.ac") {
  const auto loaded = helper.ListIrRemoteScores(providerId, serverOrigin);
  assert(loaded.status == ir::IrRemoteScoreReadOutcome::Status::Loaded);
  const auto &scores = loaded.scores;
  std::vector<std::string> result;
  result.reserve(scores.size());
  for (const auto &score : scores) {
    result.push_back(score.remoteScoreId);
  }
  return result;
}

ir::IrSubmissionReceipt snapshotReceipt(
    const ClaimedCanonicalIrAttempt &fixture,
    std::string remoteScoreId = "remote-new") {
  return {
      .id = 0,
      .providerId = "tachi",
      .serverOrigin = "https://boku.tachi.ac",
      .replayId = fixture.replayId,
      .attemptId = fixture.attemptId,
      .chartMd5 = fixture.chartMd5,
      .chartSha256 = fixture.chartSha256,
      .remoteUserId = 42,
      .remoteChartId = "remote-chart",
      .remoteScoreId = std::move(remoteScoreId),
      .source = ir::IrReceiptConfirmationSource::Snapshot,
      .observedInSnapshot = true,
      .confirmedAtUnixMillis = 3'000,
  };
}

void testApplyIrRemoteSnapshotReplacesOneOriginAtomically(
    const std::filesystem::path &root) {
  const auto path = root / "ir-remote-atomic-replace" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  auto retained = sampleIrRemoteScore("remote-retained", 'a', 'b');
  auto removed = sampleIrRemoteScore("remote-removed", 'c', 'd', 140);
  auto initial = sampleIrRemoteMutation(
      "tachi", "https://boku.tachi.ac", 2'000, {retained, removed});
  const auto initialOutcome = helper.ApplyIrRemoteSnapshot(initial);
  assert(initialOutcome.status ==
             ir::IrRemoteSnapshotApplyOutcome::Status::Applied &&
         initialOutcome.remoteScoreCount == 2 &&
         initialOutcome.remoteScoresAdded == 2 &&
         initialOutcome.remoteScoresRemoved == 0);

  auto otherOriginScore =
      sampleIrRemoteScore("other-origin-score", 'e', 'f');
  assert(helper
             .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                 "tachi", "https://other.example", 2'000,
                 {otherOriginScore}))
             .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
  auto otherProviderScore =
      sampleIrRemoteScore("other-provider-score", '1', '2');
  assert(helper
             .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                 "other", "https://boku.tachi.ac", 2'000,
                 {otherProviderScore}))
             .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);

  const auto settled = stageActivateAndClaimIrAttempt(
      helper, root, "ir-remote-settled", 113);
  const auto purged = stageActivateAndClaimIrAttempt(
      helper, root, "ir-remote-purged", 114);
  const auto stale = stageActivateAndClaimIrAttempt(
      helper, root, "ir-remote-stale", 115);
  assert(helper.ApplyIrOutboxDelivery(successfulDelivery(purged.rowId)).status ==
         ir::IrOutboxMutationStatus::Updated);
  assert(helper.ApplyIrOutboxDelivery(successfulDelivery(stale.rowId)).status ==
         ir::IrOutboxMutationStatus::Updated);
  const auto staleReceipt = helper.LoadIrSubmissionReceipt(
      "tachi", "https://boku.tachi.ac", stale.attemptId);
  assert(staleReceipt.receipt);

  helper.Shutdown();
  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE ir_outbox SET state=0 WHERE id=" +
                            std::to_string(settled.rowId));
  execOrAbort(db.get(),
              "UPDATE ir_submission_receipts SET observed_in_snapshot=1 "
              "WHERE id=" +
                  std::to_string(staleReceipt.receipt->id));
  const auto previousGeneration = queryInt(
      db.get(),
      "SELECT MAX(sync_generation) FROM ir_remote_scores WHERE "
      "provider_id='tachi' AND server_origin='https://boku.tachi.ac'");
  db.reset();

  retained.title = "Updated remote title";
  retained.timeAchievedUnixMillis.reset();
  retained.difficulty.reset();
  retained.levelNumber.reset();
  retained.judgements.good = 0;
  retained.timing.earlyPoor.reset();
  retained.finalGauge.reset();
  retained.gaugeHistory.clear();
  auto added = sampleIrRemoteScore("remote-new", '3', '4', 160);
  auto mutation = sampleIrRemoteMutation(
      "tachi", "https://boku.tachi.ac", 1'500, {retained, added});
  mutation.upsertedReceipts.push_back(snapshotReceipt(settled));
  mutation.deletedReceiptIds.push_back(staleReceipt.receipt->id);
  mutation.settledOutboxRowIds.push_back(settled.rowId);
  mutation.purgedSucceededOutboxRowIds.push_back(purged.rowId);

  const auto applied = helper.ApplyIrRemoteSnapshot(mutation);
  assert(applied.status ==
             ir::IrRemoteSnapshotApplyOutcome::Status::Applied &&
         applied.remoteScoreCount == 2 && applied.remoteScoresAdded == 1 &&
         applied.remoteScoresRemoved == 1 && applied.receiptsUpserted == 1 &&
         applied.receiptsDeleted == 1 && applied.outboxRowsSettled == 2 &&
         applied.ambiguousReceiptsPreserved == 1);

  const auto listed = helper.ListIrRemoteScores(
      "tachi", "https://boku.tachi.ac");
  assert(listed.status == ir::IrRemoteScoreReadOutcome::Status::Loaded &&
         listed.scores.size() == 2 &&
         listed.scores[0].remoteScoreId == "remote-new" &&
         listed.scores[1].remoteScoreId == "remote-retained");
  const auto &roundTripped = listed.scores[0];
  assert(roundTripped.remoteUserId == 42 &&
         roundTripped.game == "bms-7k" &&
         roundTripped.remoteChartId == "remote-chart" &&
         roundTripped.difficulty == "ANOTHER" &&
         roundTripped.level == "12" &&
         roundTripped.levelNumber == 12.5 &&
         roundTripped.timeAchievedUnixMillis == 900 &&
         roundTripped.judgements.pGreat == 60 &&
         roundTripped.timing.earlyPoor == 0 && roundTripped.fast == 11 &&
         roundTripped.finalGauge == 78.5f &&
         roundTripped.gaugeHistory ==
             std::vector<std::optional<float>>(
                 {0.0f, std::nullopt, 100.0f}) &&
         roundTripped.random == "RANDOM" && roundTripped.gauge == "HARD" &&
         roundTripped.inputDevice == "keyboard" &&
         roundTripped.client == "client");
  const auto &nullableRoundTripped = listed.scores[1];
  assert(!nullableRoundTripped.timeAchievedUnixMillis &&
         !nullableRoundTripped.difficulty &&
         !nullableRoundTripped.levelNumber &&
         nullableRoundTripped.judgements.good == 0 &&
         !nullableRoundTripped.timing.earlyPoor &&
         !nullableRoundTripped.finalGauge &&
         nullableRoundTripped.gaugeHistory.empty());
  assert(remoteScoreIds(helper, "tachi", "https://other.example") ==
         std::vector<std::string>({"other-origin-score"}));
  assert(remoteScoreIds(helper, "other", "https://boku.tachi.ac") ==
         std::vector<std::string>({"other-provider-score"}));
  assert(helper.LoadIrOutbox("tachi", settled.attemptId).status ==
         ir::IrOutboxReadStatus::NotFound);
  assert(helper.LoadIrOutbox("tachi", purged.attemptId).status ==
         ir::IrOutboxReadStatus::NotFound);
  assert(helper.LoadIrOutbox("tachi", stale.attemptId).status ==
         ir::IrOutboxReadStatus::Found);
  assert(helper
             .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                      settled.attemptId)
             .status == ir::IrReceiptReadStatus::Found);
  assert(helper
             .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                      stale.attemptId)
             .status == ir::IrReceiptReadStatus::NotFound);

  helper.Shutdown();
  db = openDatabase(path);
  assert(queryInt(db.get(),
                  "SELECT MIN(sync_generation)=MAX(sync_generation) FROM "
                  "ir_remote_scores WHERE provider_id='tachi' AND "
                  "server_origin='https://boku.tachi.ac'") == 1);
  assert(queryInt(db.get(),
                  "SELECT MAX(sync_generation) FROM ir_remote_scores WHERE "
                  "provider_id='tachi' AND "
                  "server_origin='https://boku.tachi.ac'") >
         previousGeneration);
  assert(queryText(db.get(),
                   "SELECT gauge_history_json FROM ir_remote_scores WHERE "
                   "remote_score_id='remote-new'") ==
         "[0.0,null,100.0]");
}

void testSnapshotReceiptCannotAuthorizeSucceededOutboxPurge(
    const std::filesystem::path &root) {
  const auto path =
      root / "ir-snapshot-receipt-cannot-own-delivery" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  const auto delivered = stageActivateAndClaimIrAttempt(
      helper, root, "ir-snapshot-receipt-cannot-own-delivery", 141);
  assert(helper
             .ApplyIrOutboxDelivery(
                 successfulDelivery(delivered.rowId,
                                    "https://other.example"))
             .status == ir::IrOutboxMutationStatus::Updated);

  auto unrelatedOriginMutation = sampleIrRemoteMutation(
      "tachi", "https://boku.tachi.ac", 3'000, {});
  unrelatedOriginMutation.upsertedReceipts.push_back(
      snapshotReceipt(delivered, "snapshot-score"));
  unrelatedOriginMutation.purgedSucceededOutboxRowIds.push_back(
      delivered.rowId);
  assert(helper.ApplyIrRemoteSnapshot(unrelatedOriginMutation).status ==
         ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure);
  assert(helper
             .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                      delivered.attemptId)
             .status == ir::IrReceiptReadStatus::NotFound);
  assert(helper
             .LoadIrSubmissionReceipt("tachi", "https://other.example",
                                      delivered.attemptId)
             .status == ir::IrReceiptReadStatus::Found);
  assert(helper.LoadIrOutbox("tachi", delivered.attemptId).status ==
         ir::IrOutboxReadStatus::Found);

  auto owningOriginMutation = sampleIrRemoteMutation(
      "tachi", "https://other.example", 4'000, {});
  owningOriginMutation.purgedSucceededOutboxRowIds.push_back(delivered.rowId);
  assert(helper.ApplyIrRemoteSnapshot(owningOriginMutation).status ==
         ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
  assert(helper.LoadIrOutbox("tachi", delivered.attemptId).status ==
         ir::IrOutboxReadStatus::NotFound);
}

void testApplyIrRemoteSnapshotValidatesBeforeTransaction(
    const std::filesystem::path &root) {
  const auto path = root / "ir-remote-invalid" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  auto original = sampleIrRemoteScore("original", 'a', 'b');
  assert(helper
             .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                 "tachi", "https://boku.tachi.ac", 1'000, {original}))
             .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);

  auto missingMd5 = sampleIrRemoteScore("invalid", 'c', 'd');
  missingMd5.chartMd5.clear();
  auto invalid = sampleIrRemoteMutation(
      "tachi", "https://boku.tachi.ac", 2'000, {missingMd5});
  assert(helper.ApplyIrRemoteSnapshot(invalid).status ==
         ir::IrRemoteSnapshotApplyOutcome::Status::Invalid);
  auto missingSha256 = sampleIrRemoteScore("invalid-sha", 'c', 'd');
  missingSha256.chartSha256.clear();
  invalid = sampleIrRemoteMutation(
      "tachi", "https://boku.tachi.ac", 2'000, {missingSha256});
  assert(helper.ApplyIrRemoteSnapshot(invalid).status ==
         ir::IrRemoteSnapshotApplyOutcome::Status::Invalid);
  invalid = sampleIrRemoteMutation("tachi", "HTTPS://BOKU.TACHI.AC:443/",
                                   2'000, {original});
  assert(helper.ApplyIrRemoteSnapshot(invalid).status ==
         ir::IrRemoteSnapshotApplyOutcome::Status::Invalid);
  assert(remoteScoreIds(helper) == std::vector<std::string>({"original"}));
}

void testApplyIrRemoteSnapshotRollsBackEveryMutationStage(
    const std::filesystem::path &root) {
  {
    const auto path = root / "ir-remote-reject-active-settle" / "replay.db";
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
    auto original = sampleIrRemoteScore("original", 'a', 'b');
    assert(helper
               .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                   "tachi", "https://boku.tachi.ac", 1'000, {original}))
               .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
    const auto active = stageActivateAndClaimIrAttempt(
        helper, root, "ir-remote-reject-active-settle", 116);
    auto replacement = sampleIrRemoteScore("replacement", 'c', 'd');
    auto mutation = sampleIrRemoteMutation(
        "tachi", "https://boku.tachi.ac", 2'000, {replacement});
    mutation.upsertedReceipts.push_back(snapshotReceipt(active));
    mutation.settledOutboxRowIds.push_back(active.rowId);
    assert(helper.ApplyIrRemoteSnapshot(mutation).status ==
           ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure);
    assert(remoteScoreIds(helper) == std::vector<std::string>({"original"}));
    assert(helper.LoadIrOutbox("tachi", active.attemptId).status ==
           ir::IrOutboxReadStatus::Found);
    assert(helper
               .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                        active.attemptId)
               .status == ir::IrReceiptReadStatus::NotFound);
  }

  {
    const auto path = root / "ir-remote-fail-insert" / "replay.db";
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
    auto original = sampleIrRemoteScore("original", 'a', 'b');
    assert(helper
               .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                   "tachi", "https://boku.tachi.ac", 1'000, {original}))
               .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
    helper.Shutdown();
    auto db = openDatabase(path);
    execOrAbort(db.get(),
                "CREATE TRIGGER fail_remote_insert BEFORE INSERT ON "
                "ir_remote_scores BEGIN SELECT RAISE(ABORT,'forced remote "
                "insert failure'); END");
    db.reset();
    auto replacement = sampleIrRemoteScore("replacement", 'c', 'd');
    assert(helper
               .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                   "tachi", "https://boku.tachi.ac", 2'000, {replacement}))
               .status ==
           ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure);
    assert(remoteScoreIds(helper) == std::vector<std::string>({"original"}));
  }

  {
    const auto path = root / "ir-remote-fail-receipt-upsert" / "replay.db";
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
    auto original = sampleIrRemoteScore("original", 'a', 'b');
    assert(helper
               .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                   "tachi", "https://boku.tachi.ac", 1'000, {original}))
               .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
    const auto target = stageActivateAndClaimIrAttempt(
        helper, root, "ir-remote-fail-receipt-upsert", 127);
    helper.Shutdown();
    auto db = openDatabase(path);
    execOrAbort(db.get(),
                "CREATE TRIGGER fail_receipt_upsert BEFORE INSERT ON "
                "ir_submission_receipts BEGIN SELECT RAISE(ABORT,'forced "
                "receipt upsert failure'); END");
    db.reset();
    auto replacement = sampleIrRemoteScore("replacement", 'c', 'd');
    auto mutation = sampleIrRemoteMutation(
        "tachi", "https://boku.tachi.ac", 2'000, {replacement});
    mutation.upsertedReceipts.push_back(snapshotReceipt(target));
    assert(helper.ApplyIrRemoteSnapshot(mutation).status ==
           ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure);
    assert(remoteScoreIds(helper) == std::vector<std::string>({"original"}));
    assert(helper
               .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                        target.attemptId)
               .status == ir::IrReceiptReadStatus::NotFound);
  }

  {
    const auto path = root / "ir-remote-fail-receipt-delete" / "replay.db";
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
    auto original = sampleIrRemoteScore("original", 'a', 'b');
    assert(helper
               .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                   "tachi", "https://boku.tachi.ac", 1'000, {original}))
               .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
    const auto target = stageActivateAndClaimIrAttempt(
        helper, root, "ir-remote-fail-receipt-delete", 128);
    assert(helper.ApplyIrOutboxDelivery(successfulDelivery(target.rowId)).status ==
           ir::IrOutboxMutationStatus::Updated);
    const auto receipt = helper.LoadIrSubmissionReceipt(
        "tachi", "https://boku.tachi.ac", target.attemptId);
    assert(receipt.receipt);
    helper.Shutdown();
    auto db = openDatabase(path);
    execOrAbort(db.get(),
                "CREATE TRIGGER fail_receipt_delete BEFORE DELETE ON "
                "ir_submission_receipts BEGIN SELECT RAISE(ABORT,'forced "
                "receipt delete failure'); END");
    db.reset();
    auto replacement = sampleIrRemoteScore("replacement", 'c', 'd');
    auto mutation = sampleIrRemoteMutation(
        "tachi", "https://boku.tachi.ac", 2'000, {replacement});
    mutation.deletedReceiptIds.push_back(receipt.receipt->id);
    assert(helper.ApplyIrRemoteSnapshot(mutation).status ==
           ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure);
    assert(remoteScoreIds(helper) == std::vector<std::string>({"original"}));
    assert(helper
               .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                        target.attemptId)
               .status == ir::IrReceiptReadStatus::Found);
  }

  {
    const auto path = root / "ir-remote-fail-outbox-settle" / "replay.db";
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
    auto original = sampleIrRemoteScore("original", 'a', 'b');
    assert(helper
               .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                   "tachi", "https://boku.tachi.ac", 1'000, {original}))
               .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
    const auto target = stageActivateAndClaimIrAttempt(
        helper, root, "ir-remote-fail-outbox-settle", 129);
    helper.Shutdown();
    auto db = openDatabase(path);
    execOrAbort(db.get(), "UPDATE ir_outbox SET state=0 WHERE id=" +
                              std::to_string(target.rowId));
    execOrAbort(db.get(),
                "CREATE TRIGGER fail_outbox_settle BEFORE DELETE ON ir_outbox "
                "BEGIN SELECT RAISE(ABORT,'forced outbox settle failure'); "
                "END");
    db.reset();
    auto replacement = sampleIrRemoteScore("replacement", 'c', 'd');
    auto mutation = sampleIrRemoteMutation(
        "tachi", "https://boku.tachi.ac", 2'000, {replacement});
    mutation.upsertedReceipts.push_back(snapshotReceipt(target));
    mutation.settledOutboxRowIds.push_back(target.rowId);
    assert(helper.ApplyIrRemoteSnapshot(mutation).status ==
           ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure);
    assert(remoteScoreIds(helper) == std::vector<std::string>({"original"}));
    assert(helper.LoadIrOutbox("tachi", target.attemptId).status ==
           ir::IrOutboxReadStatus::Found);
    assert(helper
               .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                        target.attemptId)
               .status == ir::IrReceiptReadStatus::NotFound);
  }
}

void testIrRemoteScoreReadRejectsMalformedGaugeHistory(
    const std::filesystem::path &root) {
  const auto path = root / "ir-remote-malformed-history" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  auto score = sampleIrRemoteScore("malformed-history", 'a', 'b');
  assert(helper
             .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                 "tachi", "https://boku.tachi.ac", 1'000, {score}))
             .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
  helper.Shutdown();
  auto db = openDatabase(path);
  execOrAbort(db.get(),
              "UPDATE ir_remote_scores SET gauge_history_json='[true]' "
              "WHERE remote_score_id='malformed-history'");
  db.reset();
  const auto malformed =
      helper.ListIrRemoteScores("tachi", "https://boku.tachi.ac");
  assert(malformed.status == ir::IrRemoteScoreReadOutcome::Status::Invalid &&
         malformed.scores.empty() && !malformed.diagnostic.empty() &&
         malformed.diagnostic.size() <=
             ir::kMaximumIrRemoteScoreDiagnosticBytes);
}

void testIrRemoteScoreReadRejectsMixedGenerations(
    const std::filesystem::path &root) {
  const auto path = root / "ir-remote-mixed-generation" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  auto first = sampleIrRemoteScore("first", 'a', 'b');
  auto second = sampleIrRemoteScore("second", 'c', 'd');
  assert(helper
             .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                 "tachi", "https://boku.tachi.ac", 1'000, {first, second}))
             .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
  helper.Shutdown();
  auto db = openDatabase(path);
  execOrAbort(db.get(),
              "UPDATE ir_remote_scores SET sync_generation=sync_generation+1 "
              "WHERE remote_score_id='second'");
  db.reset();
  const auto mixed =
      helper.ListIrRemoteScores("tachi", "https://boku.tachi.ac");
  assert(mixed.status == ir::IrRemoteScoreReadOutcome::Status::Invalid &&
         mixed.scores.empty() && !mixed.diagnostic.empty() &&
         mixed.diagnostic.size() <=
             ir::kMaximumIrRemoteScoreDiagnosticBytes);
}

void testIrRemoteScoreReadDistinguishesEmptyInvalidAndStorageFailure(
    const std::filesystem::path &root) {
  const auto path = root / "ir-remote-read-status" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  const auto empty =
      helper.ListIrRemoteScores("tachi", "https://boku.tachi.ac");
  assert(empty.status == ir::IrRemoteScoreReadOutcome::Status::Loaded &&
         empty.scores.empty() && empty.diagnostic.empty());

  const auto invalid =
      helper.ListIrRemoteScores("tachi", "HTTPS://BOKU.TACHI.AC:443/");
  assert(invalid.status == ir::IrRemoteScoreReadOutcome::Status::Invalid &&
         invalid.scores.empty() && !invalid.diagnostic.empty() &&
         invalid.diagnostic.size() <=
             ir::kMaximumIrRemoteScoreDiagnosticBytes);

  helper.Shutdown();
  auto db = openDatabase(path);
  execOrAbort(db.get(), "DROP TABLE ir_remote_scores");
  db.reset();
  const auto unavailable =
      helper.ListIrRemoteScores("tachi", "https://boku.tachi.ac");
  assert(unavailable.status ==
             ir::IrRemoteScoreReadOutcome::Status::StorageFailure &&
         unavailable.scores.empty() && !unavailable.diagnostic.empty() &&
         unavailable.diagnostic.size() <=
             ir::kMaximumIrRemoteScoreDiagnosticBytes);
}

void testClearIrAccountEvidenceIsAtomicAndOriginScoped(
    const std::filesystem::path &root) {
  const auto path = root / "ir-account-evidence-clear" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());
  auto targetScore = sampleIrRemoteScore("target-score", 'a', 'b');
  auto otherScore = sampleIrRemoteScore("other-score", 'c', 'd');
  assert(helper
             .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                 "tachi", "https://boku.tachi.ac", 1'000, {targetScore}))
             .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
  assert(helper
             .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                 "tachi", "https://other.example", 1'000, {otherScore}))
             .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
  const auto completed = stageActivateAndClaimIrAttempt(
      helper, root, "ir-account-evidence-completed", 119);
  const auto otherOrigin = stageActivateAndClaimIrAttempt(
      helper, root, "ir-account-evidence-other", 120);
  const auto unfinished = stageActivateAndClaimIrAttempt(
      helper, root, "ir-account-evidence-unfinished", 121);
  const auto legacy = stageActivateAndClaimIrAttempt(
      helper, root, "ir-account-evidence-legacy", 122);
  assert(helper.ApplyIrOutboxDelivery(successfulDelivery(completed.rowId)).status ==
         ir::IrOutboxMutationStatus::Updated);
  assert(helper
             .ApplyIrOutboxDelivery(successfulDelivery(
                 otherOrigin.rowId, "https://other.example"))
             .status == ir::IrOutboxMutationStatus::Updated);
  helper.Shutdown();
  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE ir_outbox SET state=0 WHERE id=" +
                            std::to_string(unfinished.rowId));
  execOrAbort(db.get(),
              "UPDATE ir_outbox SET state=5,completed_at_ms=2000 WHERE id=" +
                  std::to_string(legacy.rowId));
  const auto replayCount = queryInt(db.get(), "SELECT COUNT(*) FROM replays");
  db.reset();

  assert(helper
             .ClearIrAccountEvidence("tachi", "HTTPS://BOKU.TACHI.AC:443/")
             .status == ir::IrOutboxMutationStatus::Invalid);
  const auto cleared = helper.ClearIrAccountEvidence(
      "tachi", "https://boku.tachi.ac");
  assert(cleared.status == ir::IrOutboxMutationStatus::Updated &&
         cleared.affectedRows == 3);
  const auto targetMirror =
      helper.ListIrRemoteScores("tachi", "https://boku.tachi.ac");
  assert(targetMirror.status ==
             ir::IrRemoteScoreReadOutcome::Status::Loaded &&
         targetMirror.scores.empty());
  assert(remoteScoreIds(helper, "tachi", "https://other.example") ==
         std::vector<std::string>({"other-score"}));
  assert(helper
             .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                      completed.attemptId)
             .status == ir::IrReceiptReadStatus::NotFound);
  assert(helper
             .LoadIrSubmissionReceipt("tachi", "https://other.example",
                                      otherOrigin.attemptId)
             .status == ir::IrReceiptReadStatus::Found);
  assert(helper.LoadIrOutbox("tachi", completed.attemptId).status ==
         ir::IrOutboxReadStatus::NotFound);
  assert(helper.LoadIrOutbox("tachi", unfinished.attemptId).status ==
         ir::IrOutboxReadStatus::Found);
  assert(helper.LoadIrOutbox("tachi", legacy.attemptId).status ==
         ir::IrOutboxReadStatus::Found);
  const auto remoteOnlyClear =
      helper.ClearIrRemoteScores("tachi", "https://other.example");
  assert(remoteOnlyClear.status == ir::IrOutboxMutationStatus::Updated &&
         remoteOnlyClear.affectedRows == 1);
  const auto otherMirror =
      helper.ListIrRemoteScores("tachi", "https://other.example");
  assert(otherMirror.status ==
             ir::IrRemoteScoreReadOutcome::Status::Loaded &&
         otherMirror.scores.empty());
  assert(helper
             .LoadIrSubmissionReceipt("tachi", "https://other.example",
                                      otherOrigin.attemptId)
             .status == ir::IrReceiptReadStatus::Found);
  assert(helper.ClearIrRemoteScores("tachi", "https://other.example").status ==
         ir::IrOutboxMutationStatus::NotFound);
  helper.Shutdown();
  db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM replays") == replayCount);
}

void testClearIrAccountEvidenceRollsBackEveryDelete(
    const std::filesystem::path &root) {
  for (int failureStage = 0; failureStage < 3; ++failureStage) {
    const char *fixtureName = failureStage == 0
                                  ? "ir-account-clear-fail-outbox"
                              : failureStage == 1
                                  ? "ir-account-clear-fail-receipt"
                                  : "ir-account-clear-fail-remote";
    const auto path = root / fixtureName / "replay.db";
    ReplayRepository helper(path);
    assert(helper.EnsureSchema());
    auto score = sampleIrRemoteScore("target-score", 'a', 'b');
    assert(helper
               .ApplyIrRemoteSnapshot(sampleIrRemoteMutation(
                   "tachi", "https://boku.tachi.ac", 1'000, {score}))
               .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
    const auto completed = stageActivateAndClaimIrAttempt(
        helper, root, fixtureName, 123 + failureStage);
    assert(helper.ApplyIrOutboxDelivery(successfulDelivery(completed.rowId)).status ==
           ir::IrOutboxMutationStatus::Updated);
    helper.Shutdown();
    auto db = openDatabase(path);
    if (failureStage == 0) {
      execOrAbort(db.get(),
                  "CREATE TRIGGER fail_account_outbox_delete BEFORE DELETE ON "
                  "ir_outbox BEGIN SELECT RAISE(ABORT,'forced outbox delete "
                  "failure'); END");
    } else if (failureStage == 1) {
      execOrAbort(db.get(),
                  "CREATE TRIGGER fail_account_receipt_delete BEFORE DELETE "
                  "ON ir_submission_receipts BEGIN SELECT "
                  "RAISE(ABORT,'forced receipt delete failure'); END");
    } else {
      execOrAbort(db.get(),
                  "CREATE TRIGGER fail_account_remote_delete BEFORE DELETE ON "
                  "ir_remote_scores BEGIN SELECT RAISE(ABORT,'forced remote "
                  "delete failure'); END");
    }
    db.reset();

    assert(helper
               .ClearIrAccountEvidence("tachi", "https://boku.tachi.ac")
               .status == ir::IrOutboxMutationStatus::StorageFailure);
    assert(remoteScoreIds(helper) ==
           std::vector<std::string>({"target-score"}));
    assert(helper
               .LoadIrSubmissionReceipt("tachi", "https://boku.tachi.ac",
                                        completed.attemptId)
               .status == ir::IrReceiptReadStatus::Found);
    assert(helper.LoadIrOutbox("tachi", completed.attemptId).status ==
           ir::IrOutboxReadStatus::Found);
  }
}

void testIrOutboxRecoveryCountsRetryAndValidation(
    const std::filesystem::path &root) {
  const auto path = root / "ir-outbox-recovery" / "replay.db";
  ReplayRepository helper(path);
  assert(helper.EnsureSchema());

  const auto first = helper.EnqueueReadyIrOutboxDraft(
      sampleIrDraft("123e4567-e89b-42d3-a456-426614174010", 1'000), false);
  const auto second = helper.EnqueueReadyIrOutboxDraft(
      sampleIrDraft("123e4567-e89b-42d3-a456-426614174011", 1'001), false);
  const auto third = helper.EnqueueReadyIrOutboxDraft(
      sampleIrDraft("123e4567-e89b-42d3-a456-426614174012", 1'002), false);
  assert(first.entry && second.entry && third.entry);

  assert(
      helper.ClaimIrOutbox(first.entry->id, ir::IrOutboxState::Pending, 1'100)
          .status == ir::IrOutboxClaimStatus::Claimed);
  assert(
      helper.ClaimIrOutbox(second.entry->id, ir::IrOutboxState::Pending, 1'100)
          .status == ir::IrOutboxClaimStatus::Claimed);
  assert(helper
             .ApplyIrOutboxDelivery({
                 .rowId = second.entry->id,
                 .nextState = ir::IrOutboxState::AwaitingRemoteResult,
                 .nextAttemptAtUnixMillis = 1'200,
                 .remoteJobId = "job-recover",
                 .remoteOrigin = "https://boku.tachi.ac",
                 .updatedAtUnixMillis = 1'101,
             })
             .status == ir::IrOutboxMutationStatus::Updated);
  assert(helper
             .ClaimIrOutbox(second.entry->id,
                            ir::IrOutboxState::AwaitingRemoteResult, 1'200)
             .status == ir::IrOutboxClaimStatus::Claimed);
  assert(
      helper.ClaimIrOutbox(third.entry->id, ir::IrOutboxState::Pending, 1'100)
          .status == ir::IrOutboxClaimStatus::Claimed);
  assert(helper
             .ApplyIrOutboxDelivery({
                 .rowId = third.entry->id,
                 .nextState = ir::IrOutboxState::FailedPermanent,
                 .lastErrorCode = "invalid_payload",
                 .lastErrorMessage = "provider rejected payload",
                 .updatedAtUnixMillis = 1'101,
             })
             .status == ir::IrOutboxMutationStatus::Updated);

  assert(helper.RecoverStaleIrOutbox(1'300).affectedRows == 2);
  auto loadedFirst =
      helper.LoadIrOutbox("tachi", "123e4567-e89b-42d3-a456-426614174010");
  auto loadedSecond =
      helper.LoadIrOutbox("tachi", "123e4567-e89b-42d3-a456-426614174011");
  assert(loadedFirst.entry &&
         loadedFirst.entry->state == ir::IrOutboxState::Pending);
  assert(loadedSecond.entry &&
         loadedSecond.entry->state == ir::IrOutboxState::AwaitingRemoteResult &&
         loadedSecond.entry->remoteJobId == "job-recover");

  assert(helper.RetryAllIrOutbox("tachi", 1'400).affectedRows == 3);
  auto loadedThird =
      helper.LoadIrOutbox("tachi", "123e4567-e89b-42d3-a456-426614174012");
  loadedSecond =
      helper.LoadIrOutbox("tachi", "123e4567-e89b-42d3-a456-426614174011");
  assert(loadedThird.entry &&
         loadedThird.entry->state == ir::IrOutboxState::Pending &&
         loadedThird.entry->nextRequestUserIntent);
  assert(loadedSecond.entry &&
         loadedSecond.entry->state == ir::IrOutboxState::AwaitingRemoteResult &&
         loadedSecond.entry->remoteJobId == "job-recover" &&
         loadedSecond.entry->remoteOrigin == "https://boku.tachi.ac" &&
         !loadedSecond.entry->nextRequestUserIntent);

  assert(
      helper.ClaimIrOutbox(third.entry->id, ir::IrOutboxState::Pending, 1'400)
          .status == ir::IrOutboxClaimStatus::Claimed);
  assert(helper
             .ApplyIrOutboxDelivery({
                 .rowId = third.entry->id,
                 .nextState = ir::IrOutboxState::BlockedConfiguration,
                 .updatedAtUnixMillis = 1'401,
             })
             .status == ir::IrOutboxMutationStatus::Updated);
  assert(helper.UnblockIrOutbox("tachi", 1'500).affectedRows == 1);
  loadedThird =
      helper.LoadIrOutbox("tachi", "123e4567-e89b-42d3-a456-426614174012");
  assert(loadedThird.entry && !loadedThird.entry->nextRequestUserIntent &&
         loadedThird.entry->state == ir::IrOutboxState::Pending);

  const auto counts = helper.CountIrOutbox("tachi");
  assert(counts.storageAvailable && counts.pending == 2 &&
         counts.awaitingRemoteResult == 1 && counts.total == 3);
  assert(helper.DiscardIrOutbox(first.entry->id).affectedRows == 1);
  assert(helper.CountIrOutbox("tachi").total == 2);

  helper.Shutdown();
  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE ir_outbox SET state=99 WHERE id=" +
                            std::to_string(third.entry->id));
  db.reset();
  assert(helper.LoadIrOutbox("tachi", "123e4567-e89b-42d3-a456-426614174012")
             .status == ir::IrOutboxReadStatus::IntegrityConflict);
  std::string clearError;
  assert(helper.ClearIrOutbox(clearError));
  assert(clearError.empty());
  assert(helper.CountIrOutbox("tachi").total == 0);
}

void testExistingListLimits(const std::filesystem::path &root) {
  const auto path = root / "limits" / "replay.db";
  ReplayRepository helper(path);
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
  assert(helper.ListCourseReplays({.legacyCourseId = 7}).size() == 100);
  const auto allCourse = helper.ListCourseReplays({.legacyCourseId = 7}, 0);
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
  testReplayResultRecordMetadata(root);
  testInvalidNewProvenanceFailsLoad(root);
  testInvalidProvenanceRejectsReplayWrites(root);
  testInvalidReplayWritesDoNotCreateDatabase(root);
  testUnmatchableAndOversizedReplayWritesLeaveNoRows(root);
  testChartSummariesOmitInvalidProvenanceAndCountValidLimit(root);
  testCourseSummariesOmitInvalidProvenanceAndCountValidLimit(root);
  testCourseSummariesOmitInvalidLinkedStages(root);
  testFutureVersionRejectsWithoutSchemaMutation(root);
  testFutureVersionCurrentPlusOneIsRejected(root);
  testFutureReplayWritesPreservePersistentDatabaseState(root);
  testFutureReplayPreflightPreservesRawDatabaseFamily(root);
  testReplayPreflightRejectsMalformedStatesAndAllowsCreation(root);
  testEquivalentReplayAliasesRetainValidatedSession(root);
  testReplayOwnedOpenRejectsRecoveryAndConfigurationRaces(root);
  testCourseKeyMigrationStageFailureRollsBack(root);
  testCourseStageStepErrorsFailListsAndFullLoad(root);
  testReplayHydrationStepErrorsFailWholeLoad(root);
  testLargeWalReplayPreflightPreservesFamily(root);
  testChartSummaryValidationAndDetailsShareWalSnapshot(root);
  testCourseSummaryValidationAndDetailsShareWalSnapshot(root);
  testLimitedSummaryScansHaveFiniteCorruptBudget(root);
  testVersion3To4BackfillsOnlyCompleteDurableCourseReplays(root);
  testPartialCourseReplayStoresFullKeyAndRejectsInvalidShape(root);
  testCourseStagePreparationValidatesBeforeMetadataFallback(root);
  testCourseReplayLookupMergesKeyAndBlankLegacyRows(root);
  testCourseReplayLookupInspectsInt64MaxFirstPage(root);
  testCourseReplayLookupRejectsOutOfRangeIdsBeforeHydration(root);
  testCourseReplayRecoveryUsesPrefixThenExactScoreEvidence(root);
  testCourseReplayRecoveryRollsBackAndNestsInCallerTransaction(root);
  testStageChartResultAtomicallyStagesIrDrafts(root);
  testAcknowledgementActivatesIrAtomically(root);
  testVersion5MigrationAddsIrOutbox(root);
  testVersion6MigrationBlocksRowsWithoutRulesetProof(root);
  testVersion7MigrationAddsIrRemotePollCount(root);
  testCurrentVersionRejectsMalformedRulesetProofSchema(root);
  testFreshDatabaseCreatesIrSubmissionReceipts(root);
  testVersion8MigrationAddsIrSubmissionReceipts(root);
  testVersion8MigrationRejectsMalformedExistingOutbox(root);
  testCurrentVersionRejectsMalformedIrSubmissionReceiptSchema(root);
  testFreshDatabaseCreatesIrRemoteScores(root);
  testVersion9MigrationAddsIrRemoteScores(root);
  testCurrentVersionRejectsMalformedIrRemoteScoreSchema(root);
  testIrOutboxInsertClaimAndDeliveryTransitions(root);
  testIrOutboxSuccessCommitsReceiptAtomically(root);
  testLoadIrReconciliationCandidatesReturnsCanonicalScopedEvidence(root);
  testLoadIrReconciliationCandidatesSkipsCorruptionWithBoundedDiagnostic(root);
  testLoadIrReconciliationCandidatesStorageFailureReturnsNoPartialRows(root);
  testLoadIrReconciliationCandidatesUsesOneReadSnapshot(root);
  testLoadIrReconciliationCandidatesKeepsUnknownOutboxKeyMode(root);
  testClearIrSubmissionReceiptsIsOriginScoped(root);
  testClearIrSubmissionReceiptsRollsBackBothDeletes(root);
  testApplyIrRemoteSnapshotReplacesOneOriginAtomically(root);
  testSnapshotReceiptCannotAuthorizeSucceededOutboxPurge(root);
  testApplyIrRemoteSnapshotValidatesBeforeTransaction(root);
  testApplyIrRemoteSnapshotRollsBackEveryMutationStage(root);
  testIrRemoteScoreReadRejectsMalformedGaugeHistory(root);
  testIrRemoteScoreReadRejectsMixedGenerations(root);
  testIrRemoteScoreReadDistinguishesEmptyInvalidAndStorageFailure(root);
  testClearIrAccountEvidenceIsAtomicAndOriginScoped(root);
  testClearIrAccountEvidenceRollsBackEveryDelete(root);
  testIrOutboxRecoveryCountsRetryAndValidation(root);
  testVersion4MigrationAddsResultOutbox(root);
  testVersion4MarkerAcceptsExactVersion5ResultOutbox(root);
  testVersion4MarkerAcceptsStructurallyExactFormattedArtifacts(root);
  testVersion4MarkerAcceptsExactMixedCaseIdentifiers(root);
  testCurrentVersionAcceptsExactMixedCaseIdentifiersOnCachedPaths(root);
  testVersion4MarkerRejectsPartialOrMalformedVersion5Artifacts(root);
  testCurrentVersionRejectsMalformedVersion5Artifacts(root);
  testVersion4OutboxCreationFailureRollsBackBaseRepairs(root);
  testLegacyReplayRowsRemainRepeatableWithNullAttemptId(root);
  testStageChartResultIsAtomicAndReturnsTimestamp(root);
  testIdenticalAttemptIsIdempotent(root);
  testRestageRejectsCorruptRetainedOutbox(root);
  testChangedPayloadForSameAttemptConflicts(root);
  testStageRejectsSemanticResultConflicts(root);
  testStageAcceptsStandard9KeysGaugeMaximum(root);
  testStageAcceptsNonLongNoteChartMode(root);
  testStageAcceptsUnforcedLongNoteChartMode(root);
  testStageAcceptsChargeNoteJudgementsAboveNominalNoteCount(root);
  testAcknowledgedAttemptRemainsIdempotentByFingerprint(root);
  testOutboxInsertFailureRollsBackReplayAndChildren(root);
  testPendingReadsDistinguishMissingFailureAndConflict(root);
  testRecoverySnapshotKeepsMalformedRowsAndContinues(root);
  testRecoverySnapshotPrioritizesNeverAttemptedRows(root);
  testPendingSemanticConflictsAreRetainedByAcknowledgement(root);
  testPendingBatchHardCapsAt256(root);
  testMalformedPendingIdentitiesCanRotate(root);
  testExistingListLimits(root);

  std::filesystem::remove_all(root);
  std::cout << "replay database helper tests passed\n";
  return 0;
}
