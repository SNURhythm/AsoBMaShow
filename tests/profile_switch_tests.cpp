#include "../src/AppSettingsStore.h"
#include "../src/repositories/ChartRepository.h"
#include "../src/CoursePlaySession.h"
#include "../src/ProfileDatabaseActivity.h"
#include "../src/ProfileDatabaseTools.h"
#include "../src/ProfileSessionCoordinator.h"
#include "../src/repositories/ReplayRepository.h"
#include "../src/ResultPersistenceCoordinator.h"
#include "../src/repositories/ScoreRepository.h"
#include "../src/repositories/SqliteRAII.h"
#include "../src/Utils.h"
#include "../src/input/InputProfileStore.h"
#include "../src/scene/MainMenuProfileSelections.h"
#include "../src/sqlite3.h"
#include "support/ReplaySchema10Fixture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
int failures = 0;

static_assert(
    !std::is_move_constructible_v<ScoreRepository::PreparedScoreQueryDatabase>);
static_assert(
    !std::is_move_assignable_v<ScoreRepository::PreparedScoreQueryDatabase>);

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool replaceDifficultyTableJson(ChartRepository::Session &session,
                                const std::string &headerJson,
                                const std::string &dataJson,
                                const std::string &sourceUrl) {
  std::string errorMessage;
  const auto document =
      difficulty_table::Parse(headerJson, dataJson, sourceUrl, errorMessage);
  if (!document.has_value()) {
    std::cerr << "Difficulty table parse failure: " << errorMessage << '\n';
    return false;
  }
  return session.ReplaceDifficultyTable(*document);
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-profile-switch-" + std::to_string(nonce) + "-" +
             std::to_string(sequence.fetch_add(1)));
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

#if !TARGET_OS_WINDOWS
class ScopedHomeOverride {
public:
  explicit ScopedHomeOverride(const std::filesystem::path &path) {
    if (const char *home = std::getenv("HOME")) {
      previousHome_ = home;
    }
    expect(setenv("HOME", path.string().c_str(), 1) == 0,
           "temporary HOME override installs");
  }

  ~ScopedHomeOverride() {
    if (previousHome_.has_value()) {
      setenv("HOME", previousHome_->c_str(), 1);
    } else {
      unsetenv("HOME");
    }
  }

private:
  std::optional<std::string> previousHome_;
};
#endif

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool execute(sqlite3 *database, const std::string &sql) {
  char *rawError = nullptr;
  const int result =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &rawError);
  if (result != SQLITE_OK) {
    std::cerr << "SQL failure: " << (rawError == nullptr ? "unknown" : rawError)
              << '\n';
    sqlite3_free(rawError);
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

int queryInt(sqlite3 *database, const std::string &sql) {
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) !=
      SQLITE_OK) {
    return 0;
  }
  const int value = sqlite3_step(statement) == SQLITE_ROW
                        ? sqlite3_column_int(statement, 0)
                        : 0;
  sqlite3_finalize(statement);
  return value;
}

std::string queryString(sqlite3 *database, const std::string &sql) {
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) !=
      SQLITE_OK) {
    return {};
  }
  const std::string value = sqlite3_step(statement) == SQLITE_ROW
                                ? sqliteColumnString(statement, 0)
                                : std::string();
  sqlite3_finalize(statement);
  return value;
}

void setDatabaseVersion(const std::filesystem::path &path, int version,
                        std::string_view label) {
  Database database = openDatabase(path);
  expect(database != nullptr, std::string(label) + " database opens");
  if (!database) {
    return;
  }
  expect(
      execute(database.get(), "PRAGMA user_version=" + std::to_string(version)),
      std::string(label) + " database version updates");
}

void seedValidationPolicyMarker(const std::filesystem::path &path,
                                std::string_view value,
                                std::string_view label) {
  Database database = openDatabase(path);
  expect(database != nullptr, std::string(label) + " marker database opens");
  if (!database) {
    return;
  }
  expect(execute(database.get(),
                 "CREATE TABLE profile_use_marker(value TEXT);"
                 "INSERT INTO profile_use_marker(value) VALUES ('" +
                     std::string(value) + "')"),
         std::string(label) + " marker row inserts");
}

std::string queryDatabaseString(const std::filesystem::path &path,
                                const std::string &sql,
                                std::string_view label) {
  Database database = openDatabase(path);
  expect(database != nullptr, std::string(label) + " database opens");
  return database ? queryString(database.get(), sql) : std::string{};
}

struct BlockingStatementTrace {
  std::mutex mutex;
  std::condition_variable condition;
  std::string fragment;
  bool entered = false;
  bool released = false;
};

struct SqlStatementTrace {
  std::vector<std::string> statements;
};

int collectSqlStatement(unsigned traceType, void *rawContext, void *statement,
                        void *) {
  if (traceType != SQLITE_TRACE_STMT) {
    return 0;
  }
  const char *sql = sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
  if (sql != nullptr) {
    static_cast<SqlStatementTrace *>(rawContext)->statements.emplace_back(sql);
  }
  return 0;
}

SqlStatementTrace *activeSqlStatementTrace = nullptr;

int installSqlStatementTrace(sqlite3 *database, char **,
                             const sqlite3_api_routines *) {
  if (activeSqlStatementTrace == nullptr) {
    return SQLITE_OK;
  }
  return sqlite3_trace_v2(database, SQLITE_TRACE_STMT, collectSqlStatement,
                          activeSqlStatementTrace);
}

class ScopedSqlStatementTrace {
public:
  explicit ScopedSqlStatementTrace(SqlStatementTrace &trace) {
    activeSqlStatementTrace = &trace;
    sqlite3_reset_auto_extension();
    installed_ = sqlite3_auto_extension(reinterpret_cast<void (*)()>(
                     installSqlStatementTrace)) == SQLITE_OK;
  }

  ~ScopedSqlStatementTrace() {
    sqlite3_reset_auto_extension();
    activeSqlStatementTrace = nullptr;
  }

  [[nodiscard]] bool installed() const { return installed_; }

private:
  bool installed_ = false;
};

BlockingStatementTrace *activeBlockingTrace = nullptr;

int blockMatchingStatement(unsigned traceType, void *rawContext,
                           void *statement, void *) {
  if (traceType != SQLITE_TRACE_STMT) {
    return 0;
  }
  auto &trace = *static_cast<BlockingStatementTrace *>(rawContext);
  const char *sql = sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
  if (sql == nullptr ||
      std::string_view(sql).find(trace.fragment) == std::string_view::npos) {
    return 0;
  }
  std::unique_lock lock(trace.mutex);
  if (trace.entered) {
    return 0;
  }
  trace.entered = true;
  trace.condition.notify_all();
  trace.condition.wait(lock, [&trace] { return trace.released; });
  return 0;
}

int installBlockingStatementTrace(sqlite3 *database, char **,
                                  const sqlite3_api_routines *) {
  if (activeBlockingTrace == nullptr) {
    return SQLITE_OK;
  }
  return sqlite3_trace_v2(database, SQLITE_TRACE_STMT, blockMatchingStatement,
                          activeBlockingTrace);
}

class ScopedBlockingStatementTrace {
public:
  explicit ScopedBlockingStatementTrace(std::string fragment) {
    trace_.fragment = std::move(fragment);
    activeBlockingTrace = &trace_;
    sqlite3_reset_auto_extension();
    installed_ = sqlite3_auto_extension(reinterpret_cast<void (*)()>(
                     installBlockingStatementTrace)) == SQLITE_OK;
  }

  ~ScopedBlockingStatementTrace() {
    release();
    sqlite3_reset_auto_extension();
    activeBlockingTrace = nullptr;
  }

  [[nodiscard]] bool installed() const { return installed_; }

  bool waitUntilEntered() {
    std::unique_lock lock(trace_.mutex);
    return trace_.condition.wait_for(lock, std::chrono::seconds(5),
                                     [this] { return trace_.entered; });
  }

  void release() {
    {
      std::lock_guard lock(trace_.mutex);
      trace_.released = true;
    }
    trace_.condition.notify_all();
  }

private:
  BlockingStatementTrace trace_;
  bool installed_ = false;
};

constexpr std::string_view kChartSha =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kChartMd5 = "0123456789abcdef0123456789abcdef";

void seedReplayMigrationChartMetadata(const std::filesystem::path &path) {
  Database database = openDatabase(path);
  expect(database != nullptr, "replay migration chart database opens");
  if (!database) {
    return;
  }
  expect(
      execute(database.get(),
              "CREATE TABLE chart_meta(path TEXT PRIMARY KEY,md5 TEXT NOT NULL,"
              "sha256 TEXT NOT NULL,keys INTEGER,ln_mode INTEGER,"
              "total_long_notes INTEGER,total_backspin_notes INTEGER,"
              "total_notes INTEGER);"
              "INSERT INTO chart_meta(path,md5,sha256,keys,ln_mode,"
              "total_long_notes,total_backspin_notes,total_notes) VALUES("
              "'chart.bms','" +
                  std::string(kChartMd5) + "','" + std::string(kChartSha) +
                  "',7,0,0,0,1)"),
      "replay migration chart metadata is authoritative");
}

void seedScore(const std::filesystem::path &path, int clearType, int score) {
  Database database = openDatabase(path);
  expect(database != nullptr, "score seed database opens");
  if (!database) {
    return;
  }
  expect(
      execute(database.get(),
              "INSERT INTO scores (chart_path,chart_md5,chart_sha256,ln_mode,"
              "chart_title,chart_artist,score,max_score,max_combo,"
              "combo_break,pgreat,great,good,bad,poor,kpoor,fast,slow,"
              "final_gauge,clear_type) VALUES ('chart.bms','" +
                  std::string(kChartMd5) + "','" + std::string(kChartSha) +
                  "',0,'Chart','Artist'," + std::to_string(score) +
                  ",2000,50,1,10,9,8,7,6,5,4,3,0.75," +
                  std::to_string(clearType) + ")"),
      "score row inserts");
}

void seedChartScore(const std::filesystem::path &path,
                    std::string_view chartPath, std::string_view chartMd5,
                    std::string_view chartSha256, int longNoteMode,
                    int clearRank, int score) {
  Database database = openDatabase(path);
  expect(database != nullptr, "chart-matrix score seed database opens");
  if (!database) {
    return;
  }
  expect(
      execute(database.get(),
              "INSERT INTO scores (chart_path,chart_md5,chart_sha256,ln_mode,"
              "chart_title,chart_artist,score,max_score,max_combo,"
              "combo_break,pgreat,great,good,bad,poor,kpoor,fast,slow,"
              "final_gauge,clear_type) VALUES ('" +
                  std::string(chartPath) + "','" + std::string(chartMd5) +
                  "','" + std::string(chartSha256) + "'," +
                  std::to_string(longNoteMode) + ",'Chart','Artist'," +
                  std::to_string(score) + ",1000,50,1,10,9,8,7,6,5,4,3,0.75," +
                  std::to_string(clearRank) + ")"),
      "chart-matrix score row inserts");
}

void seedCompactReplayResult(const std::filesystem::path &path, int score,
                             std::int64_t playedAtUnixMillis) {
  Database database = openDatabase(path);
  expect(database != nullptr, "compact replay seed database opens");
  if (!database) {
    return;
  }
  const std::string provenance =
      serializeScoreProvenance(ScoreProvenance::Legacy());
  expect(
      execute(
          database.get(),
          "INSERT INTO chart_results(attempt_id,chart_path,chart_md5,"
          "chart_sha256,chart_title,chart_artist,key_mode,long_note_mode,score,"
          "max_score,max_combo,combo_break,p_great,great,good,bad,poor,k_poor,"
          "fast,slow,final_gauge,clear_type,adopted_gauge_type,"
          "gauge_history_json,"
          "judgement_timing_json,provenance_json,result_fingerprint,"
          "played_at_unix_ms) VALUES('switch-" +
              std::to_string(playedAtUnixMillis) + "','chart.bms','" +
              std::string(kChartMd5) + "','" + std::string(kChartSha) +
              "','Chart','Artist',7,0," + std::to_string(score) + "," +
              std::to_string(score) +
              ",1,0,1,0,0,0,0,0,0,0,75.0,300,2,'[75.0]',NULL," +
              replay_schema10_fixture::quote(provenance) + ",'" +
              std::string(64, 'f') + "'," +
              std::to_string(playedAtUnixMillis) + ")"),
      "compact replay result row inserts");
}

void replaceWithSchema10Replays(const std::filesystem::path &path,
                                std::span<const int> scores,
                                std::string_view label) {
  std::error_code error;
  std::filesystem::remove(path, error);
  expect(!error, std::string(label) + " old replay database removes");
  if (error) {
    return;
  }
  Database database = openDatabase(path);
  expect(database != nullptr, std::string(label) + " v10 database opens");
  if (!database) {
    return;
  }
  try {
    replay_schema10_fixture::createExactSchema(database.get());
    const std::string provenance =
        serializeScoreProvenance(ScoreProvenance::Legacy());
    for (std::size_t index = 0; index < scores.size(); ++index) {
      const std::string attemptId =
          "123e4567-e89b-42d3-a456-4266141741" +
          std::to_string(index + 10);
      const std::string createdAt =
          "2026-07-11 00:00:0" + std::to_string(index);
      replay_schema10_fixture::insertSimpleChart(
          database.get(), static_cast<std::int64_t>(index + 1), kChartMd5,
          kChartSha, createdAt, scores[index], provenance, attemptId, 0);
      replay_schema10_fixture::insertSimplePendingChart(
          database.get(), static_cast<std::int64_t>(index + 1), kChartMd5,
          kChartSha, createdAt, scores[index], provenance, attemptId, 0);
    }
  } catch (const std::exception &exception) {
    expect(false, std::string(label) + " v10 fixture creates: " +
                      exception.what());
  }
}

std::string firstBindingId(const InputProfile &profile) {
  return profile.bindings.empty() ? std::string() : profile.bindings.front().id;
}

struct SwitchFixture {
  TempDirectory temp;
  std::vector<std::string> ids = {
      "11111111-1111-4111-8111-111111111111",
      "22222222-2222-4222-8222-222222222222",
  };
  std::size_t nextId = 0;
  bool failBootstrap = false;
  bool failSettingsSave = false;
  bool failInputSave = false;
  bool failScoreBind = false;
  bool failReplayBind = false;
  bool failInputApply = false;
  bool failRefreshOnce = false;
  bool failServicePause = false;
  bool failServiceActivate = false;
  bool throwRecoveryStd = false;
  bool throwRecoveryNonStd = false;
  std::optional<std::string> blocker;
  ProfileSwitchBlockers operationBlockers;
  int refreshCount = 0;
  int recoveryCalls = 0;
  int servicePauseCalls = 0;
  int serviceActivateCalls = 0;
  int serviceRestoreCalls = 0;
  bool recoveryObservedTargetBindings = false;
  bool recoveryNestedGuardCompleted = false;
  bool pauseObservedOldBindings = false;
  bool activationObservedTargetState = false;
  bool restoreObservedOldState = false;
  result_persistence::RecoverySummary recoverySummary;
  std::vector<std::string> switchEvents;
  std::vector<std::string> inputReplacementEvents;
  std::vector<input::GyroscopeTurntableConfig> runtimeGyroscopeApplications;
  std::function<void()> refreshAction;
  std::function<void()> recoveryAction;
  std::filesystem::path appliedInputPath;
  InputProfile currentInput = makeDefaultInputProfile();
  input::GyroscopeTurntableConfig runtimeGyroscopeConfig;
  PlayerProfileManager manager;
  std::string firstId;
  std::string secondId;
  PlayerProfilePaths firstPaths;
  PlayerProfilePaths secondPaths;
  AppSettings currentSettings;
  ScoreRepository score;
  ReplayRepository replay;
  std::unique_ptr<ChartRepository> persistentCharts;
  std::optional<ChartRepository::Session> persistentChartSession;
  ProfileSessionCoordinator coordinator;

  SwitchFixture()
      : manager(temp.path(), makeManagerDependencies()), score(), replay(),
        coordinator(
            manager, score, replay,
            [this]() {
              if (blocker) {
                return blocker;
              }
              return operationBlockers.firstReason();
            },
            [this](const std::filesystem::path &path, std::string &error) {
              return applyInput(path, error);
            },
            [this](const std::filesystem::path &path) { restoreInput(path); },
            [this]() { refreshCaches(); }, makeSwitchDependencies()) {
    const ProfileResult initialized = manager.Initialize();
    expect(initialized.ok(),
           "switch fixture initializes: " + initialized.message);
    if (!initialized.ok()) {
      return;
    }
    firstId = manager.activeProfile().id;
    const ProfileResult created = manager.createProfile("Second");
    expect(created.ok() && created.profile.has_value(),
           "switch fixture creates second profile: " + created.message);
    if (!created.profile) {
      return;
    }
    secondId = created.profile->id;
    firstPaths = manager.pathsFor(firstId);
    secondPaths = manager.pathsFor(secondId);

    AppSettings firstSettings;
    firstSettings.audioOffsetMs = -17;
    firstSettings.selectedGameplayRuleset = "lr2";
    firstSettings.selectedGaugeType = "gas";
    firstSettings.selectedPlayOption = "MIRROR";
    firstSettings.selectedLnMode = "LN";
    firstSettings.selectedAssistOption = "DRAG";
    firstSettings.selectedPacemakerTarget = "A";
    firstSettings.sanitize();
    AppSettings secondSettings;
    secondSettings.audioOffsetMs = 42;
    secondSettings.selectedGameplayRuleset = "beatoraja";
    secondSettings.selectedGaugeType = "hard";
    secondSettings.selectedPlayOption = "R-RANDOM";
    secondSettings.selectedLnMode = "HCN";
    secondSettings.selectedAssistOption = "OFF";
    secondSettings.selectedPacemakerTarget = "MAX-";
    secondSettings.sanitize();
    std::string error;
    expect(
        AppSettingsStore::Save(firstPaths.settingsJson, firstSettings, error),
        "first settings seed saves: " + error);
    expect(
        AppSettingsStore::Save(secondPaths.settingsJson, secondSettings, error),
        "second settings seed saves: " + error);

    InputProfile firstInput = makeDefaultInputProfile();
    InputProfile secondInput = makeDefaultInputProfile();
    firstInput.gyroscopeTurntable = {.stepAngleDegrees = 4,
                                     .releaseDelayMs = 250};
    secondInput.gyroscopeTurntable = {.stepAngleDegrees = 8,
                                      .releaseDelayMs = 500};
    if (!firstInput.bindings.empty()) {
      firstInput.bindings.front().id = "first-profile-binding";
    }
    if (!secondInput.bindings.empty()) {
      secondInput.bindings.front().id = "second-profile-binding";
    }
    expect(
        InputProfileStore::saveAtomic(firstPaths.inputJson, firstInput, error),
        "first input seed saves: " + error);
    expect(InputProfileStore::saveAtomic(secondPaths.inputJson, secondInput,
                                         error),
           "second input seed saves: " + error);

    seedScore(firstPaths.scoresDb, kClearTypeEasyClearRank, 500);
    seedScore(secondPaths.scoresDb, kClearTypeHardClearRank, 1500);
    seedCompactReplayResult(firstPaths.replaysDb, 500, 500);
    seedCompactReplayResult(secondPaths.replaysDb, 1500, 1500);
    seedCompactReplayResult(secondPaths.replaysDb, 1600, 1600);

    currentSettings = firstSettings;
    currentInput = firstInput;
    runtimeGyroscopeConfig = firstInput.gyroscopeTurntable;
    appliedInputPath = firstPaths.inputJson;
    score.SetDatabasePath(firstPaths.scoresDb);
    replay.SetDatabasePath(firstPaths.replaysDb);
    const auto migrationChartDatabase = temp.path() / "migration-chart.db";
    seedReplayMigrationChartMetadata(migrationChartDatabase);
    replay.SetChartDatabasePath(migrationChartDatabase);
    switchEvents.clear();
  }

  PlayerProfileManagerDependencies makeManagerDependencies() {
    PlayerProfileManagerDependencies dependencies;
    dependencies.generateUuid = [this] { return ids.at(nextId++); };
    dependencies.utcNow = [] { return "2026-07-11T12:34:56Z"; };
    dependencies.beforeMigrationPhase = [this](ProfileMigrationPhase phase,
                                               std::string &error) {
      if (phase == ProfileMigrationPhase::WriteBootstrap) {
        switchEvents.emplace_back("commit");
      }
      if (phase == ProfileMigrationPhase::WriteBootstrap && failBootstrap) {
        error = "injected bootstrap failure";
        return false;
      }
      return true;
    };
    return dependencies;
  }

  ProfileSessionDependencies makeSwitchDependencies() {
    ProfileSessionDependencies dependencies;
    dependencies.saveSettings = [this](const std::filesystem::path &path,
                                       const AppSettings &settings,
                                       std::string &error) {
      if (failSettingsSave) {
        error = "injected settings save failure";
        return false;
      }
      return AppSettingsStore::Save(path, settings, error);
    };
    dependencies.saveInput = [this](const std::filesystem::path &path,
                                    std::string &error) {
      if (failInputSave) {
        error = "injected input save failure";
        return false;
      }
      return InputProfileStore::saveAtomic(path, currentInput, error);
    };
    dependencies.bindScore = [this](ScoreRepository &helper,
                                    const std::filesystem::path &path,
                                    std::string &error) {
      if (failScoreBind) {
        error = "injected score bind failure";
        return false;
      }
      const bool bound = helper.BindDatabasePath(path, error);
      if (bound) {
        switchEvents.emplace_back("bind-score");
      }
      return bound;
    };
    dependencies.bindReplay = [this](ReplayRepository &helper,
                                     const std::filesystem::path &path,
                                     std::string &error) {
      if (failReplayBind) {
        error = "injected replay bind failure";
        return false;
      }
      const bool bound = helper.BindDatabasePath(path, error);
      if (bound) {
        switchEvents.emplace_back("bind-replay");
      }
      return bound;
    };
    dependencies.recoverPendingResults = [this] {
      ++recoveryCalls;
      switchEvents.emplace_back("recover-results");
      recoveryObservedTargetBindings =
          score.GetDatabasePath() == secondPaths.scoresDb &&
          replay.GetDatabasePath() == secondPaths.replaysDb;
      {
        profile_database_activity::WriteGuard nestedRecoveryGuard;
        recoveryNestedGuardCompleted = ScoreRepository::HasActiveWrites() &&
                                       ReplayRepository::HasActiveWrites();
      }
      if (recoveryAction) {
        recoveryAction();
      }
      if (throwRecoveryStd) {
        throw std::runtime_error(
            "attempt-private: /private/profile/replays.db");
      }
      if (throwRecoveryNonStd) {
        throw 17;
      }
      return recoverySummary;
    };
    dependencies.beforeInputReplacement = [this]() {
      inputReplacementEvents.emplace_back("before");
    };
    dependencies.pauseProfileServices = [this](std::string &error) {
      ++servicePauseCalls;
      switchEvents.emplace_back("pause-services");
      pauseObservedOldBindings = manager.activeProfile().id == firstId;
      if (failServicePause) {
        error = "injected service pause failure";
        return false;
      }
      return true;
    };
    dependencies.activateProfileServices =
        [this](std::string_view profileId, const AppSettings &activeSettings,
               std::string &error) {
          ++serviceActivateCalls;
          switchEvents.emplace_back("activate-services");
          activationObservedTargetState =
              profileId == secondId && manager.activeProfile().id == secondId &&
              score.GetDatabasePath() == secondPaths.scoresDb &&
              replay.GetDatabasePath() == secondPaths.replaysDb &&
              activeSettings.audioOffsetMs == 42;
          if (failServiceActivate) {
            error = "injected service activation failure";
            return false;
          }
          return true;
        };
    dependencies.restoreProfileServices =
        [this](std::string_view profileId, const AppSettings &activeSettings,
               std::string &) {
          ++serviceRestoreCalls;
          switchEvents.emplace_back("restore-services");
          restoreObservedOldState = profileId == firstId &&
                                    manager.activeProfile().id == firstId &&
                                    activeSettings.audioOffsetMs == -17;
          return true;
        };
    return dependencies;
  }

  bool applyInput(const std::filesystem::path &path, std::string &error) {
    switchEvents.emplace_back("apply-input");
    inputReplacementEvents.emplace_back("apply");
    const auto loaded = InputProfileStore::load(path);
    if (loaded.status != InputProfileLoadStatus::Loaded) {
      error = "unable to apply input profile";
      return false;
    }
    currentInput = loaded.profile;
    appliedInputPath = path;
    runtimeGyroscopeConfig = currentInput.gyroscopeTurntable;
    runtimeGyroscopeApplications.push_back(runtimeGyroscopeConfig);
    if (failInputApply) {
      error = "injected input apply failure";
      return false;
    }
    return true;
  }

  void restoreInput(const std::filesystem::path &path) {
    inputReplacementEvents.emplace_back("restore");
    const auto loaded = InputProfileStore::load(path);
    if (loaded.status == InputProfileLoadStatus::Loaded) {
      currentInput = loaded.profile;
      appliedInputPath = path;
      runtimeGyroscopeConfig = currentInput.gyroscopeTurntable;
      runtimeGyroscopeApplications.push_back(runtimeGyroscopeConfig);
    }
  }

  void refreshCaches() {
    switchEvents.emplace_back("refresh-caches");
    ++refreshCount;
    if (failRefreshOnce) {
      failRefreshOnce = false;
      throw std::runtime_error("injected cache refresh failure");
    }
    if (refreshAction) {
      refreshAction();
    }
    if (persistentChartSession) {
      auto prepared = score.PrepareScoreQueryDatabase(*persistentChartSession);
      if (const auto &error = prepared.error()) {
        throw std::runtime_error(*error);
      }
    }
  }

  bool openPersistentChartSession(std::string_view label) {
    persistentCharts = std::make_unique<ChartRepository>(
        temp.path() / (std::string(label) + "-chart.db"));
    persistentChartSession = persistentCharts->OpenSession(&score);
    return persistentChartSession.has_value();
  }

  [[nodiscard]] int currentClearRank() {
    return score.LoadBestClearRanks().bestRankForStoredKey(kChartSha, 0);
  }

  [[nodiscard]] std::size_t currentReplayCount(int longNoteMode = 0) {
    Database database = openDatabase(replay.GetDatabasePath());
    return database == nullptr
               ? 0
               : static_cast<std::size_t>(queryInt(
                     database.get(),
                     "SELECT count(*) FROM chart_results WHERE chart_sha256='" +
                         std::string(kChartSha) + "' AND long_note_mode=" +
                         std::to_string(longNoteMode)));
  }
};

void expectFirstProfileState(SwitchFixture &fixture,
                             std::string_view bootstrapBefore,
                             std::string_view label) {
  expect(fixture.manager.activeProfile().id == fixture.firstId,
         std::string(label) + " keeps manager active profile");
  expect(fixture.score.GetDatabasePath() == fixture.firstPaths.scoresDb,
         std::string(label) + " restores score database path");
  expect(fixture.replay.GetDatabasePath() == fixture.firstPaths.replaysDb,
         std::string(label) + " restores replay database path");
  expect(fixture.currentSettings.audioOffsetMs == -17 &&
             fixture.currentSettings.selectedGameplayRuleset == "lr2" &&
             fixture.currentSettings.selectedPlayOption == "MIRROR",
         std::string(label) + " restores current settings");
  expect(fixture.appliedInputPath == fixture.firstPaths.inputJson &&
             firstBindingId(fixture.currentInput) == "first-profile-binding" &&
             fixture.runtimeGyroscopeConfig ==
                 input::GyroscopeTurntableConfig{.stepAngleDegrees = 4,
                                                 .releaseDelayMs = 250},
         std::string(label) + " restores current input profile");
  expect(readFile(fixture.temp.path() / "active-profile.json") ==
             bootstrapBefore,
         std::string(label) + " leaves bootstrap-visible state unchanged");
  expect(fixture.currentClearRank() == kClearTypeEasyClearRank,
         std::string(label) + " exposes first profile score cache data");
  expect(fixture.currentReplayCount() == 1,
         std::string(label) + " exposes first profile replay data");
}

void testSuccessfulSwitchIsIsolatedAndPersistsOldState() {
  SwitchFixture fixture;
  if (fixture.firstId.empty() || fixture.secondId.empty()) {
    return;
  }
  const auto chartSentinel = fixture.temp.path() / "db" / "chart.db";
  std::filesystem::create_directories(chartSentinel.parent_path());
  {
    std::ofstream output(chartSentinel, std::ios::binary);
    output << "shared-chart-sentinel";
  }
  const std::uint64_t revisionBefore = fixture.score.GetRevision();
  fixture.currentSettings.audioOffsetMs = -88;
  if (!fixture.currentInput.bindings.empty()) {
    fixture.currentInput.bindings.front().id = "unsaved-first-binding";
  }

  const ProfileSwitchResult switched =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(switched.ok(),
         "successful profile switch completes: " + switched.message);
  expect(fixture.manager.activeProfile().id == fixture.secondId,
         "successful switch commits manager active profile last");
  expect(fixture.score.GetDatabasePath() == fixture.secondPaths.scoresDb &&
             fixture.replay.GetDatabasePath() == fixture.secondPaths.replaysDb,
         "successful switch rebinds score and replay helpers");
  expect(fixture.currentSettings.audioOffsetMs == 42 &&
             fixture.currentSettings.selectedGameplayRuleset == "beatoraja" &&
             fixture.currentSettings.selectedPlayOption == "R-RANDOM",
         "successful switch installs target settings");
  expect(firstBindingId(fixture.currentInput) == "second-profile-binding" &&
             fixture.appliedInputPath == fixture.secondPaths.inputJson &&
             fixture.runtimeGyroscopeConfig ==
                 input::GyroscopeTurntableConfig{.stepAngleDegrees = 8,
                                                 .releaseDelayMs = 500} &&
             fixture.runtimeGyroscopeApplications ==
                 std::vector<input::GyroscopeTurntableConfig>{
                     {.stepAngleDegrees = 8, .releaseDelayMs = 500}},
         "successful switch applies target input profile");
  expect(fixture.currentClearRank() == kClearTypeHardClearRank,
         "score query results change to target profile");
  expect(fixture.currentReplayCount() == 2,
         "replay queries are served by target profile");
  expect(fixture.refreshCount == 1 &&
             fixture.score.GetRevision() > revisionBefore,
         "successful switch refreshes caches and score revision");
  expect(readFile(chartSentinel) == "shared-chart-sentinel" &&
             !std::filesystem::exists(fixture.secondPaths.root / "chart.db"),
         "chart database remains shared and outside profiles");

  const auto savedOldSettings =
      AppSettingsStore::Load(fixture.firstPaths.settingsJson);
  const auto savedOldInput =
      InputProfileStore::load(fixture.firstPaths.inputJson);
  expect(savedOldSettings.status == AppSettingsLoadStatus::Loaded &&
             savedOldSettings.settings.audioOffsetMs == -88,
         "switch saves current settings into the old profile first");
  expect(savedOldInput.status == InputProfileLoadStatus::Loaded &&
             firstBindingId(savedOldInput.profile) == "unsaved-first-binding",
         "switch saves current input into the old profile first");
}

void testTargetRecoveryRunsAfterBothDatabaseBindsBeforeCacheRefresh() {
  SwitchFixture fixture;
  if (fixture.firstId.empty() || fixture.secondId.empty()) {
    return;
  }
  fixture.switchEvents.clear();

  const ProfileSwitchResult result =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);

  expect(result.ok(), "ordered recovery profile switch succeeds");
  expect(fixture.switchEvents ==
             std::vector<std::string>{"pause-services", "bind-score",
                                      "bind-replay", "recover-results",
                                      "apply-input", "refresh-caches", "commit",
                                      "activate-services"},
         "target recovery runs after both binds and before input, caches, and "
         "commit");
  expect(fixture.recoveryCalls == 1 && fixture.recoveryObservedTargetBindings,
         "recovery runs exactly once against both target database bindings");
  expect(fixture.servicePauseCalls == 1 && fixture.pauseObservedOldBindings &&
             fixture.serviceActivateCalls == 1 &&
             fixture.activationObservedTargetState &&
             fixture.serviceRestoreCalls == 0,
         "profile services pause on the source before rebinding and activate "
         "only after the target is committed");
}

void testServicePauseFailureAndActivationRollback() {
  SwitchFixture pauseFailure;
  if (pauseFailure.firstId.empty() || pauseFailure.secondId.empty()) {
    return;
  }
  pauseFailure.failServicePause = true;
  const ProfileSwitchResult blocked = pauseFailure.coordinator.switchTo(
      pauseFailure.secondId, pauseFailure.currentSettings);
  expect(blocked.error == ProfileError::SwitchBlocked &&
             pauseFailure.servicePauseCalls == 1 &&
             pauseFailure.serviceActivateCalls == 0 &&
             pauseFailure.serviceRestoreCalls == 1 &&
             pauseFailure.switchEvents ==
                 std::vector<std::string>{"pause-services", "restore-services"},
         "service pause failure restores the source service snapshot without "
         "binding target databases");
  expect(pauseFailure.score.GetDatabasePath() ==
                 pauseFailure.firstPaths.scoresDb &&
             pauseFailure.replay.GetDatabasePath() ==
                 pauseFailure.firstPaths.replaysDb &&
             pauseFailure.manager.activeProfile().id == pauseFailure.firstId,
         "service pause failure leaves every profile binding on the source");

  SwitchFixture activationFailure;
  if (activationFailure.firstId.empty() || activationFailure.secondId.empty()) {
    return;
  }
  const std::string bootstrapBefore =
      readFile(activationFailure.temp.path() / "active-profile.json");
  activationFailure.failServiceActivate = true;
  const ProfileSwitchResult rolledBack = activationFailure.coordinator.switchTo(
      activationFailure.secondId, activationFailure.currentSettings);
  expect(!rolledBack.ok() && activationFailure.serviceActivateCalls == 1 &&
             activationFailure.activationObservedTargetState &&
             activationFailure.serviceRestoreCalls == 1 &&
             activationFailure.restoreObservedOldState,
         "target service activation failure reactivates the old generation "
         "after restoring source bindings");
  expectFirstProfileState(activationFailure, bootstrapBefore,
                          "service activation failure");
}

void testRecoveryWarningDoesNotRollbackSuccessfulSwitch() {
  SwitchFixture fixture;
  if (fixture.firstId.empty() || fixture.secondId.empty()) {
    return;
  }
  fixture.recoverySummary = {
      .attempted = 2,
      .saved = 1,
      .pending = 1,
      .conflicts = 0,
      .userMessage = std::string(result_persistence::recoveryUserMessage()),
      .diagnostic = "attempt-private: /private/profile/replays.db",
  };

  const ProfileSwitchResult result =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);

  expect(result.ok(), "pending recovery warning keeps switch successful");
  expect(result.message == result_persistence::recoveryUserMessage(),
         "successful switch preserves only the aggregate recovery warning");
  expect(fixture.manager.activeProfile().id == fixture.secondId &&
             fixture.score.GetDatabasePath() == fixture.secondPaths.scoresDb &&
             fixture.replay.GetDatabasePath() == fixture.secondPaths.replaysDb,
         "recovery warning does not roll back valid target bindings");
}

void testRecoveryExceptionBecomesSanitizedWarning() {
  for (const bool nonStandard : {false, true}) {
    SwitchFixture fixture;
    if (fixture.firstId.empty() || fixture.secondId.empty()) {
      continue;
    }
    fixture.throwRecoveryStd = !nonStandard;
    fixture.throwRecoveryNonStd = nonStandard;

    const ProfileSwitchResult result =
        fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);

    expect(result.ok(),
           "recovery callback exception does not roll back profile switch");
    expect(result.message == result_persistence::recoveryUserMessage(),
           "recovery callback exception becomes the sanitized aggregate "
           "warning");
    expect(result.message.find("attempt-private") == std::string::npos &&
               result.message.find("/private/") == std::string::npos,
           "recovery warning excludes raw exception details");
    expect(fixture.manager.activeProfile().id == fixture.secondId &&
               fixture.recoveryCalls == 1,
           "recovery exception still leaves the target profile active");
  }
}

void testRollbackRestoresOldBindingsAfterLaterFailure() {
  SwitchFixture fixture;
  if (fixture.firstId.empty() || fixture.secondId.empty()) {
    return;
  }
  const std::string bootstrapBefore =
      readFile(fixture.temp.path() / "active-profile.json");
  int recoveredTargetResults = 0;
  fixture.recoveryAction = [&] { ++recoveredTargetResults; };
  fixture.failRefreshOnce = true;

  const ProfileSwitchResult result =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);

  expect(!result.ok(), "failure after recovery still aborts profile switch");
  expect(fixture.recoveryCalls == 1 && recoveredTargetResults == 1,
         "target recovery side effect remains committed after later failure");
  expectFirstProfileState(fixture, bootstrapBefore,
                          "post-recovery cache failure");
}

void testRecoveryRunsUnderExistingSwitchGuardWithoutDeadlock() {
  SwitchFixture fixture;
  if (fixture.firstId.empty() || fixture.secondId.empty()) {
    return;
  }

  const ProfileSwitchResult result =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);

  expect(result.ok(), "nested recovery guard profile switch completes");
  expect(fixture.recoveryNestedGuardCompleted,
         "recovery acquires its nested write guard under the switch guard");
}

void testRecoverySkipsSameProfileAndFailedDatabaseBinds() {
  SwitchFixture sameProfile;
  if (sameProfile.firstId.empty()) {
    return;
  }
  const ProfileSwitchResult unchanged = sameProfile.coordinator.switchTo(
      sameProfile.firstId, sameProfile.currentSettings);
  expect(unchanged.ok() && sameProfile.recoveryCalls == 0,
         "same-profile early return does not run recovery");

  for (const bool scoreFailure : {true, false}) {
    SwitchFixture fixture;
    if (fixture.firstId.empty() || fixture.secondId.empty()) {
      continue;
    }
    fixture.failScoreBind = scoreFailure;
    fixture.failReplayBind = !scoreFailure;
    const ProfileSwitchResult result =
        fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
    expect(!result.ok() && fixture.recoveryCalls == 0,
           "failed target database bind never runs recovery");
  }
}

void testSupportedOlderTargetMigratesAtSchemaOwnerBoundary() {
  SwitchFixture fixture;
  if (fixture.firstId.empty() || fixture.secondId.empty()) {
    return;
  }

  seedValidationPolicyMarker(fixture.secondPaths.scoresDb, "score-marker",
                             "supported-older score");
  replaceWithSchema10Replays(fixture.secondPaths.replaysDb,
                             std::array{2, 2},
                             "supported-older replay");
  seedValidationPolicyMarker(fixture.secondPaths.replaysDb, "replay-marker",
                             "supported-older replay");
  setDatabaseVersion(fixture.secondPaths.scoresDb, 5, "supported-older score");

  expect(fixture.manager.validateProfileForActivation(fixture.secondId).ok(),
         "activation preflight admits supported-older databases");
  expect(fixture.manager.validateProfile(fixture.secondId).error ==
             ProfileError::IntegrityFailure,
         "runtime-ready validation rejects supported-older databases before "
         "binding");

  const ProfileSwitchResult switched =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(switched.ok(),
         "supported-older profile switch completes: " + switched.message);

  std::string versionError;
  expect(
      sqliteDatabaseUserVersion(fixture.secondPaths.scoresDb, versionError) ==
          ScoreRepository::kCurrentSchemaVersion,
      "score database owner migrates the supported-older target: " +
          versionError);
  versionError.clear();
  expect(
      sqliteDatabaseUserVersion(fixture.secondPaths.replaysDb, versionError) ==
          ReplayRepository::kCurrentSchemaVersion,
      "replay database owner migrates the supported-older target: " +
          versionError);
  expect(fixture.manager.activeProfile().id == fixture.secondId &&
             fixture.score.GetDatabasePath() == fixture.secondPaths.scoresDb &&
             fixture.replay.GetDatabasePath() == fixture.secondPaths.replaysDb,
         "switch commits the migrated target active after both owners bind");
  expect(fixture.currentClearRank() == kClearTypeHardClearRank &&
             fixture.currentReplayCount(0) == 2,
         "schema-owner migration preserves target scores and replays");
  expect(queryDatabaseString(fixture.secondPaths.scoresDb,
                             "SELECT value FROM profile_use_marker",
                             "migrated score marker") == "score-marker" &&
             queryDatabaseString(fixture.secondPaths.replaysDb,
                                 "SELECT value FROM profile_use_marker",
                                 "migrated replay marker") == "replay-marker",
         "schema-owner migration preserves unrelated marker rows");
  expect(fixture.manager.validateProfile(fixture.secondId).ok(),
         "migrated switched profile is runtime ready");
}

void testEveryDeclaredBlockerRejectsWithoutMutation() {
  for (const std::string reason :
       {"Gameplay is active", "Chart scan/import is active",
        "Replay export is active", "Profile archive work is active"}) {
    SwitchFixture fixture;
    if (fixture.firstId.empty()) {
      continue;
    }
    const std::string bootstrapBefore =
        readFile(fixture.temp.path() / "active-profile.json");
    fixture.blocker = reason;
    const ProfileSwitchResult result =
        fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
    expect(result.error == ProfileError::SwitchBlocked &&
               result.message.find(reason) != std::string::npos,
           reason + " blocker rejects with its reason");
    expect(fixture.refreshCount == 0,
           reason + " blocker performs no cache mutation");
    expectFirstProfileState(fixture, bootstrapBefore, reason);
  }

  SwitchFixture fixture;
  if (fixture.firstId.empty()) {
    return;
  }
  const std::string bootstrapBefore =
      readFile(fixture.temp.path() / "active-profile.json");
  {
    profile_database_activity::WriteGuard activeWrite;
    expect(ScoreRepository::HasActiveWrites() &&
               ReplayRepository::HasActiveWrites(),
           "shared database activity reports a real active write");
    const ProfileSwitchResult result =
        fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
    expect(result.error == ProfileError::SwitchBlocked,
           "active score/replay write gate rejects profile switch");
  }
  expect(fixture.refreshCount == 0,
         "active database write blocker performs no cache mutation");
  expectFirstProfileState(fixture, bootstrapBefore, "database write blocker");
}

void testBackgroundLibraryBlockerSurvivesSceneReplacement() {
  SwitchFixture fixture;
  if (fixture.firstId.empty() || fixture.secondId.empty()) {
    return;
  }
  bool libraryRebuildActive = true;
  fixture.operationBlockers.background = [&]() -> std::optional<std::string> {
    return libraryRebuildActive
               ? std::optional<std::string>(
                     "A chart library scan or import is active.")
               : std::nullopt;
  };
  fixture.operationBlockers.scene = []() -> std::optional<std::string> {
    return std::nullopt;
  };

  auto result =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(result.error == ProfileError::SwitchBlocked &&
             result.message.find("chart library") != std::string::npos,
         "background chart rebuild blocks profile activation from Settings");

  fixture.operationBlockers.scene = []() -> std::optional<std::string> {
    return "Settings scene blocker replacement";
  };
  result =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(result.error == ProfileError::SwitchBlocked &&
             result.message.find("chart library") != std::string::npos,
         "background chart blocker remains authoritative after scene blocker "
         "replacement");

  libraryRebuildActive = false;
  fixture.operationBlockers.scene = {};
  result =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(result.ok(),
         "profile activation proceeds after background rebuild completes");
}

enum class FailureStage {
  SettingsSave,
  InputSave,
  ScoreBind,
  ReplayBind,
  InputApply,
  CacheRefresh,
  Bootstrap,
};

std::string_view failureLabel(FailureStage stage) {
  switch (stage) {
  case FailureStage::SettingsSave:
    return "settings save failure";
  case FailureStage::InputSave:
    return "input save failure";
  case FailureStage::ScoreBind:
    return "score bind failure";
  case FailureStage::ReplayBind:
    return "replay bind failure";
  case FailureStage::InputApply:
    return "input apply failure";
  case FailureStage::CacheRefresh:
    return "cache refresh failure";
  case FailureStage::Bootstrap:
    return "bootstrap commit failure";
  }
  return "unknown failure";
}

void testEveryTransactionalFailureRollsBackAllVisibleState() {
  for (const FailureStage stage :
       {FailureStage::SettingsSave, FailureStage::InputSave,
        FailureStage::ScoreBind, FailureStage::ReplayBind,
        FailureStage::InputApply, FailureStage::CacheRefresh,
        FailureStage::Bootstrap}) {
    SwitchFixture fixture;
    if (fixture.firstId.empty()) {
      continue;
    }
    const std::string bootstrapBefore =
        readFile(fixture.temp.path() / "active-profile.json");
    switch (stage) {
    case FailureStage::SettingsSave:
      fixture.failSettingsSave = true;
      break;
    case FailureStage::InputSave:
      fixture.failInputSave = true;
      break;
    case FailureStage::ScoreBind:
      fixture.failScoreBind = true;
      break;
    case FailureStage::ReplayBind:
      fixture.failReplayBind = true;
      break;
    case FailureStage::InputApply:
      fixture.failInputApply = true;
      break;
    case FailureStage::CacheRefresh:
      fixture.failRefreshOnce = true;
      break;
    case FailureStage::Bootstrap:
      fixture.failBootstrap = true;
      break;
    }

    const ProfileSwitchResult result =
        fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
    expect(!result.ok(), std::string(failureLabel(stage)) + " is reported");
    expect(fixture.refreshCount >= 1,
           std::string(failureLabel(stage)) +
               " refreshes restored caches before returning");
    expectFirstProfileState(fixture, bootstrapBefore, failureLabel(stage));
  }
}

void testRollbackBindFailureClosesTargetAndFailsClosed() {
  for (const bool failScoreRestore : {true, false}) {
    SwitchFixture fixture;
    if (fixture.firstId.empty()) {
      continue;
    }

    bool injectFailure = true;
    fixture.refreshAction = [&]() {
      if (!injectFailure) {
        return;
      }
      injectFailure = false;
      const std::filesystem::path sourcePath =
          failScoreRestore ? fixture.firstPaths.scoresDb
                           : fixture.firstPaths.replaysDb;
      std::error_code filesystemError;
      const bool removed = std::filesystem::remove(sourcePath, filesystemError);
      expect(removed && !filesystemError,
             "rollback failure fixture removes the source database");
      filesystemError.clear();
      const bool directoryCreated =
          std::filesystem::create_directory(sourcePath, filesystemError);
      expect(directoryCreated && !filesystemError,
             "rollback failure fixture replaces the source with a directory");
      throw std::runtime_error("injected post-bind cache failure");
    };

    const ProfileSwitchResult result =
        fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
    const std::string databaseKind = failScoreRestore ? "score" : "replay";
    expect(!result.ok() &&
               result.message.find("unable to restore " + databaseKind +
                                   " database") != std::string::npos,
           "rollback reports the failed " + databaseKind + " rebind");
    expect(fixture.manager.activeProfile().id == fixture.firstId,
           "rollback bind failure leaves the profile manager on the source");
    expect(fixture.score.GetDatabasePath() == fixture.firstPaths.scoresDb &&
               fixture.replay.GetDatabasePath() == fixture.firstPaths.replaysDb,
           "rollback bind failure closes target handles and restores source "
           "paths");
    if (failScoreRestore) {
      expect(fixture.currentClearRank() == kNoClearTypeRank,
             "failed score restoration cannot expose target scores");
    } else {
      expect(fixture.currentReplayCount() == 0,
             "failed replay restoration cannot expose target replays");
    }
  }
}

void testInvalidTargetFailsBeforeSavingOrBinding() {
  SwitchFixture fixture;
  if (fixture.firstId.empty()) {
    return;
  }
  const std::string bootstrapBefore =
      readFile(fixture.temp.path() / "active-profile.json");
  {
    std::ofstream output(fixture.secondPaths.settingsJson,
                         std::ios::binary | std::ios::trunc);
    output << "{\"schemaVersion\":99}\n";
  }
  const std::string oldSettingsBefore =
      readFile(fixture.firstPaths.settingsJson);
  const ProfileSwitchResult result =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(result.error == ProfileError::FutureVersion,
         "future target component fails closed before transaction");
  expect(readFile(fixture.firstPaths.settingsJson) == oldSettingsBefore,
         "invalid target does not save or rewrite the old profile");
  expect(fixture.refreshCount == 0,
         "invalid target does not refresh or mutate caches");
  expectFirstProfileState(fixture, bootstrapBefore, "invalid target");

  SwitchFixture invalidInput;
  if (invalidInput.firstId.empty()) {
    return;
  }
  const std::string inputBootstrapBefore =
      readFile(invalidInput.temp.path() / "active-profile.json");
  {
    std::ofstream output(invalidInput.secondPaths.inputJson,
                         std::ios::binary | std::ios::trunc);
    output << "{\"schemaVersion\":99}\n";
  }
  const std::string oldInputBefore =
      readFile(invalidInput.firstPaths.inputJson);
  const ProfileSwitchResult inputResult = invalidInput.coordinator.switchTo(
      invalidInput.secondId, invalidInput.currentSettings);
  expect(inputResult.error == ProfileError::FutureVersion,
         "future target input fails closed before transaction");
  expect(readFile(invalidInput.firstPaths.inputJson) == oldInputBefore &&
             invalidInput.runtimeGyroscopeApplications.empty(),
         "invalid target input neither saves the source nor changes runtime "
         "gyroscope configuration");
  expectFirstProfileState(invalidInput, inputBootstrapBefore,
                          "invalid input target");
}

void testInputReplacementNotificationPrecedesApplyAndRollback() {
  SwitchFixture successful;
  if (successful.firstId.empty()) {
    return;
  }
  const ProfileSwitchResult switched = successful.coordinator.switchTo(
      successful.secondId, successful.currentSettings);
  expect(switched.ok(),
         "input replacement ordering test switches successfully");
  expect(successful.inputReplacementEvents ==
             std::vector<std::string>{"before", "apply"},
         "successful switch cancels pending input work before applying B");

  SwitchFixture rolledBack;
  if (rolledBack.firstId.empty()) {
    return;
  }
  rolledBack.failInputApply = true;
  const ProfileSwitchResult failed = rolledBack.coordinator.switchTo(
      rolledBack.secondId, rolledBack.currentSettings);
  expect(!failed.ok(), "input replacement rollback test injects a failure");
  expect(rolledBack.inputReplacementEvents ==
             std::vector<std::string>{"before", "apply", "before", "restore"},
         "rollback cancels pending input work before restoring A");
  expect(rolledBack.runtimeGyroscopeApplications ==
                 std::vector<input::GyroscopeTurntableConfig>{
                     {.stepAngleDegrees = 8, .releaseDelayMs = 500},
                     {.stepAngleDegrees = 4, .releaseDelayMs = 250}} &&
             rolledBack.runtimeGyroscopeConfig ==
                 input::GyroscopeTurntableConfig{.stepAngleDegrees = 4,
                                                 .releaseDelayMs = 250},
         "rollback restores the source gyroscope configuration after the "
         "target attempt");
}

void testPersistentScoreAttachmentFailureRollsBackToSource() {
  SwitchFixture fixture;
  if (fixture.firstId.empty()) {
    return;
  }
  expect(fixture.openPersistentChartSession("attachment-failure"),
         "persistent chart session opens");
  if (!fixture.persistentChartSession) {
    return;
  }
  {
    auto initialPrepared = fixture.score.PrepareScoreQueryDatabase(
        *fixture.persistentChartSession);
    expect(!initialPrepared.error().has_value(),
           "persistent chart connection initially attaches profile A");
  }
  const std::string bootstrapBefore =
      readFile(fixture.temp.path() / "active-profile.json");

  bool targetPathSabotaged = false;
  fixture.refreshAction = [&]() {
    if (targetPathSabotaged) {
      return;
    }
    targetPathSabotaged = true;
    const auto backup = fixture.secondPaths.scoresDb.string() + ".backup";
    std::filesystem::rename(fixture.secondPaths.scoresDb, backup);
    std::filesystem::create_directory(fixture.secondPaths.scoresDb);
  };
  const ProfileSwitchResult result =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);

  expect(!result.ok(), "score cache reattach failure aborts profile switch");
  expect(targetPathSabotaged, "target score attach was attempted");
  const auto rollbackScores =
      fixture.score.LoadBestScores(*fixture.persistentChartSession, "score_db");
  const auto rollbackBest = rollbackScores.bestForStoredKey(kChartSha, 0);
  expect(rollbackBest.has_value() && rollbackBest->score == 500,
         "rollback persistent connection queries profile A score data");
  fixture.persistentChartSession.reset();
  fixture.persistentCharts.reset();
  expectFirstProfileState(fixture, bootstrapBefore,
                          "score attachment refresh failure");
}

void testDatabasePathReadsAreIndependentSnapshots() {
  const std::filesystem::path scoreFirst = "profile-one/scores.db";
  const std::filesystem::path scoreSecond = "profile-two/scores.db";
  ScoreRepository score;
  score.SetDatabasePath(scoreFirst);
  const auto &scoreSnapshot = score.GetDatabasePath();
  score.SetDatabasePath(scoreSecond);
  expect(scoreSnapshot == scoreFirst && score.GetDatabasePath() == scoreSecond,
         "score helper path reads are value snapshots, not mutable aliases");

  const std::filesystem::path replayFirst = "profile-one/replays.db";
  const std::filesystem::path replaySecond = "profile-two/replays.db";
  ReplayRepository replay;
  replay.SetDatabasePath(replayFirst);
  const auto &replaySnapshot = replay.GetDatabasePath();
  replay.SetDatabasePath(replaySecond);
  expect(replaySnapshot == replayFirst &&
             replay.GetDatabasePath() == replaySecond,
         "replay helper path reads are value snapshots, not mutable aliases");

  std::atomic<bool> sawUnexpectedPath{false};
  std::thread reader([&]() {
    for (int iteration = 0; iteration < 2000; ++iteration) {
      const auto path = score.GetDatabasePath();
      if (path != scoreFirst && path != scoreSecond) {
        sawUnexpectedPath.store(true, std::memory_order_release);
      }
    }
  });
  for (int iteration = 0; iteration < 2000; ++iteration) {
    score.SetDatabasePath((iteration & 1) == 0 ? scoreFirst : scoreSecond);
  }
  reader.join();
  expect(!sawUnexpectedPath.load(std::memory_order_acquire),
         "concurrent path rebinding exposes only complete path snapshots");
}

void testRetainedMainMenuSelectionsReloadWithoutProfileLeakage() {
  SwitchFixture fixture;
  if (fixture.firstId.empty()) {
    return;
  }
  main_menu_profile::Selections retained =
      main_menu_profile::Selections::fromSettings(fixture.currentSettings);
  fixture.refreshAction = [&]() { retained.reload(fixture.currentSettings); };
  const ProfileSwitchResult switched =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(switched.ok(), "retained MainMenu selection test switches to B");
  expect(retained.ruleset == GameplayRuleset::Beatoraja &&
             retained.gaugeType == GaugeType::Hard &&
             retained.gaugeAutoShift == GaugeAutoShiftMode::BestClear &&
             retained.playOption == "R-RANDOM" &&
             retained.longNoteMode == "HCN" && retained.assistOption == "OFF" &&
             retained.pacemakerTarget == "MAX-",
         "retained MainMenu gameplay selections reload every profile B value");

  AppSettings laterSave;
  retained.applyTo(laterSave);
  expect(laterSave.selectedGameplayRuleset == "beatoraja" &&
             laterSave.selectedGaugeType == "hard" &&
             laterSave.selectedGaugeAutoShiftMode == "best_clear" &&
             laterSave.selectedPlayOption == "R-RANDOM" &&
             laterSave.selectedLnMode == "HCN" &&
             laterSave.selectedAssistOption == "OFF" &&
             laterSave.selectedPacemakerTarget == "MAX-",
         "later retained MainMenu save cannot leak profile A selections");

  SwitchFixture rollbackFixture;
  if (rollbackFixture.firstId.empty()) {
    return;
  }
  main_menu_profile::Selections rollbackSelections =
      main_menu_profile::Selections::fromSettings(
          rollbackFixture.currentSettings);
  rollbackFixture.refreshAction = [&]() {
    rollbackSelections.reload(rollbackFixture.currentSettings);
  };
  rollbackFixture.failBootstrap = true;
  const ProfileSwitchResult rolledBack = rollbackFixture.coordinator.switchTo(
      rollbackFixture.secondId, rollbackFixture.currentSettings);
  expect(!rolledBack.ok(), "retained MainMenu rollback test fails bootstrap");
  expect(rollbackSelections.ruleset == GameplayRuleset::LR2 &&
             rollbackSelections.gaugeType == GaugeType::ExHard &&
             rollbackSelections.gaugeAutoShift ==
                 GaugeAutoShiftMode::BestClear &&
             rollbackSelections.playOption == "MIRROR" &&
             rollbackSelections.longNoteMode == "LN" &&
             rollbackSelections.assistOption == "DRAG" &&
             rollbackSelections.pacemakerTarget == "A",
         "rollback refresh restores every retained profile A selection");
}

void testRealScoreReadBlocksSwitchUntilQueryCompletes() {
  SwitchFixture fixture;
  if (fixture.firstId.empty()) {
    return;
  }
  ScopedBlockingStatementTrace blockedRead(
      "FROM score_sha256_clear_rank_cache");
  expect(blockedRead.installed(), "real score read trace installs");

  int readRank = kNoClearTypeRank;
  std::thread reader([&]() { readRank = fixture.currentClearRank(); });
  const bool readEntered = blockedRead.waitUntilEntered();
  expect(readEntered, "real score cache query reaches deterministic gate");
  if (!readEntered) {
    blockedRead.release();
    reader.join();
    return;
  }
  expect(ScoreRepository::HasActiveReads() &&
             ReplayRepository::HasActiveReads(),
         "shared database activity reports the real active read");

  const auto switchStarted = std::chrono::steady_clock::now();
  const ProfileSwitchResult duringRead =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  const auto switchElapsed = std::chrono::steady_clock::now() - switchStarted;
  expect(duringRead.error == ProfileError::SwitchBlocked,
         "real old-profile score read immediately blocks profile switch");
  expect(switchElapsed < std::chrono::seconds(1),
         "active read rejection is nonblocking");

  blockedRead.release();
  reader.join();
  expect(!ScoreRepository::HasActiveReads() &&
             !ReplayRepository::HasActiveReads(),
         "active read state clears after the query completes");
  expect(readRank == kClearTypeEasyClearRank,
         "old-profile read completes before any successful switch");

  if (duringRead.error == ProfileError::SwitchBlocked) {
    const ProfileSwitchResult afterRead =
        fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
    expect(afterRead.ok(), "profile switch succeeds after real read completes");
    expect(fixture.currentClearRank() == kClearTypeHardClearRank,
           "post-read switch exposes profile B score data");
  }
}

void testScoreAttachmentPreparationOwnsActivePathSnapshot() {
  SwitchFixture fixture;
  if (fixture.firstId.empty()) {
    return;
  }

  ScopedBlockingStatementTrace blockedPrepare("PRAGMA database_list");
  expect(blockedPrepare.installed(),
         "score attachment preparation trace installs");
  expect(fixture.openPersistentChartSession("attachment-snapshot"),
         "guarded score attachment chart session opens");
  if (!fixture.persistentChartSession) {
    return;
  }

  std::optional<std::string> prepareError;
  std::thread preparer([&]() {
    auto prepared = fixture.score.PrepareScoreQueryDatabase(
        *fixture.persistentChartSession);
    prepareError = prepared.error();
  });
  const bool prepareEntered = blockedPrepare.waitUntilEntered();
  expect(prepareEntered,
         "guarded score attachment reaches deterministic query gate");
  if (!prepareEntered) {
    blockedPrepare.release();
    preparer.join();
    return;
  }

  expect(ScoreRepository::HasActiveWrites() &&
             ReplayRepository::HasActiveWrites(),
         "active-path snapshot and attachment preparation publish one "
         "database operation");
  const ProfileSwitchResult duringPrepare =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(duringPrepare.error == ProfileError::SwitchBlocked,
         "profile switch cannot interleave after the active score path is "
         "selected but before attachment preparation completes");

  blockedPrepare.release();
  preparer.join();
  expect(!prepareError.has_value(),
         "guarded profile A score attachment preparation succeeds");
  expect(
      fixture.score
              .LoadBestClearRanks(*fixture.persistentChartSession, "score_db")
              .bestRankForStoredKey(kChartSha, 0) == kClearTypeEasyClearRank,
      "blocked switch leaves the retained connection attached to profile "
      "A");

  const ProfileSwitchResult afterPrepare =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(afterPrepare.ok(),
         "profile switch succeeds after guarded attachment preparation");
  expect(
      fixture.score
              .LoadBestClearRanks(*fixture.persistentChartSession, "score_db")
              .bestRankForStoredKey(kChartSha, 0) == kClearTypeHardClearRank,
      "successful switch reattaches the retained connection to profile B");
  fixture.persistentChartSession.reset();
  fixture.persistentCharts.reset();
}

void testPreparedScoreQueryGuardLivesThroughDependentQuery() {
  SwitchFixture fixture;
  if (fixture.firstId.empty()) {
    return;
  }

  ScopedBlockingStatementTrace blockedQuery(
      "FROM score_db.score_sha256_clear_rank_cache");
  expect(blockedQuery.installed(), "score-backed query trace installs");
  expect(fixture.openPersistentChartSession("dependent-score-query"),
         "score-backed query chart session opens");
  if (!fixture.persistentChartSession) {
    return;
  }

  std::optional<std::string> prepareError;
  int queryRank = kNoClearTypeRank;
  std::thread query([&]() {
    auto prepared = fixture.score.PrepareScoreQueryDatabase(
        *fixture.persistentChartSession);
    prepareError = prepared.error();
    if (!prepareError.has_value()) {
      queryRank =
          fixture.score
              .LoadBestClearRanks(*fixture.persistentChartSession, "score_db")
              .bestRankForStoredKey(kChartSha, 0);
    }
  });
  const bool queryEntered = blockedQuery.waitUntilEntered();
  expect(queryEntered,
         "dependent score-backed query reaches deterministic gate");
  if (!queryEntered) {
    blockedQuery.release();
    query.join();
    return;
  }

  expect(ScoreRepository::HasActiveWrites(),
         "prepared score query retains its write-classified operation guard");
  const ProfileSwitchResult duringQuery =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(duringQuery.error == ProfileError::SwitchBlocked,
         "profile switch cannot interleave between attachment preparation and "
         "its dependent query");

  blockedQuery.release();
  query.join();
  expect(!prepareError.has_value(),
         "profile A score-backed query preparation succeeds");
  expect(queryRank == kClearTypeEasyClearRank,
         "in-flight dependent query completes with profile A data");

  const ProfileSwitchResult afterQuery =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(afterQuery.ok(),
         "profile switch succeeds after the dependent query releases its "
         "guard");
  auto prepared =
      fixture.score.PrepareScoreQueryDatabase(*fixture.persistentChartSession);
  expect(!prepared.error().has_value(),
         "profile B score-backed query preparation succeeds");
  expect(
      fixture.score
              .LoadBestClearRanks(*fixture.persistentChartSession, "score_db")
              .bestRankForStoredKey(kChartSha, 0) == kClearTypeHardClearRank,
      "post-switch dependent query reads profile B data");
  fixture.persistentChartSession.reset();
  fixture.persistentCharts.reset();
}

void testRealChartDbScoreQueryRetainsPreparedAttachmentGuard() {
  SwitchFixture fixture;
  if (fixture.firstId.empty()) {
    return;
  }

  ScoreRepository &activeScore = fixture.score;

  ScopedBlockingStatementTrace blockedQuery(
      "FROM score_db.score_sha256_clear_rank_cache");
  expect(blockedQuery.installed(), "real ChartDB score query trace installs");
  const std::filesystem::path chartPath =
      fixture.temp.path() / "real-chart-query.db";
  ChartRepository charts(chartPath);
  expect(charts.EnsureReady(), "real ChartDB score query schema creates");
  Database chartDatabase = openDatabase(chartPath);
  expect(chartDatabase != nullptr, "real ChartDB score query connection opens");
  if (!chartDatabase) {
    return;
  }
  expect(execute(chartDatabase.get(),
                 "INSERT INTO chart_meta "
                 "(path, md5, sha256, title, ln_mode, total_long_notes, "
                 "total_backspin_notes, source_priority, source_archive_size) "
                 "VALUES ('chart.bms', '" +
                     std::string(kChartMd5) + "', '" + std::string(kChartSha) +
                     "', 'Chart', 0, 0, 0, 0, 0)"),
         "real ChartDB score query row inserts");
  chartDatabase.reset();
  auto chartSession = charts.OpenSession(&activeScore);
  expect(chartSession.has_value(), "real ChartDB score query session opens");
  if (!chartSession) {
    return;
  }

  ChartMetaQuery firstQuery;
  firstQuery.clearMarkFilter = true;
  firstQuery.clearMarkRank = kClearTypeEasyClearRank;
  int firstCount = -1;
  std::thread query(
      [&]() { firstCount = chartSession->CountChartMeta(firstQuery); });
  const bool queryEntered = blockedQuery.waitUntilEntered();
  expect(queryEntered,
         "real ChartDB score-backed count reaches deterministic gate");
  if (!queryEntered) {
    blockedQuery.release();
    query.join();
    return;
  }

  profile_database_activity::SwitchGuard duringQuery;
  expect(!duringQuery.ownsLock(),
         "real ChartDB score-backed count prevents a concurrent profile "
         "switch");
  blockedQuery.release();
  query.join();
  expect(firstCount == 1,
         "real ChartDB score-backed count completes with profile A data");

  {
    profile_database_activity::SwitchGuard switchToSecond;
    expect(switchToSecond.ownsLock(),
           "profile switch gate opens after real ChartDB count completes");
    if (switchToSecond.ownsLock()) {
      activeScore.SetDatabasePath(fixture.secondPaths.scoresDb);
    }
  }
  ChartMetaQuery secondQuery = firstQuery;
  secondQuery.clearMarkRank = kClearTypeHardClearRank;
  expect(chartSession->CountChartMeta(secondQuery) == 1,
         "next real ChartDB score-backed count reattaches and reads profile "
         "B");
}

void testLegacyForcedLongNoteScoreMigrationPreservesLamp() {
#if !TARGET_OS_WINDOWS
  TempDirectory temp;
  ScopedHomeOverride home(temp.path());
  const std::filesystem::path chartDirectory = Utils::GetDocumentsPath("db");
  std::filesystem::create_directories(chartDirectory);
  const std::filesystem::path chartPath = chartDirectory / "chart.db";
  ChartRepository charts(chartPath);
  expect(charts.EnsureReady(),
         "legacy forced-LN migration chart schema creates");
  Database chartDatabase = openDatabase(chartPath);
  expect(chartDatabase != nullptr,
         "legacy forced-LN migration chart database opens");
  if (!chartDatabase) {
    return;
  }
  expect(execute(chartDatabase.get(),
                 "INSERT INTO chart_meta "
                 "(path, md5, sha256, title, ln_mode, total_long_notes, "
                 "total_backspin_notes, source_priority, source_archive_size) "
                 "VALUES ('forced.bms', '" +
                     std::string(kChartMd5) + "', '" + std::string(kChartSha) +
                     "', 'Forced CN', 2, 1, 0, 0, 0)"),
         "legacy forced-LN migration chart metadata inserts");
  chartDatabase.reset();

  const std::filesystem::path scorePath = temp.path() / "legacy-score.db";
  Database scoreDatabase = openDatabase(scorePath);
  expect(scoreDatabase != nullptr,
         "legacy forced-LN migration score database opens");
  if (!scoreDatabase) {
    return;
  }
  expect(execute(scoreDatabase.get(),
                 "CREATE TABLE scores ("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT, chart_path TEXT, "
                 "chart_md5 TEXT, chart_sha256 TEXT NOT NULL, "
                 "ln_mode INTEGER NOT NULL DEFAULT 0, chart_title TEXT, "
                 "chart_artist TEXT, score INTEGER NOT NULL, "
                 "max_score INTEGER NOT NULL, max_combo INTEGER NOT NULL, "
                 "combo_break INTEGER NOT NULL, pgreat INTEGER NOT NULL, "
                 "great INTEGER NOT NULL, good INTEGER NOT NULL, "
                 "bad INTEGER NOT NULL, poor INTEGER NOT NULL, "
                 "kpoor INTEGER NOT NULL, fast INTEGER NOT NULL, "
                 "slow INTEGER NOT NULL, final_gauge REAL NOT NULL, "
                 "clear_type INTEGER NOT NULL, "
                 "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
                 "INSERT INTO scores (chart_path, chart_md5, chart_sha256, "
                 "ln_mode, chart_title, chart_artist, score, max_score, "
                 "max_combo, combo_break, pgreat, great, good, bad, poor, "
                 "kpoor, fast, slow, final_gauge, clear_type) VALUES "
                 "('forced.bms', '" +
                     std::string(kChartMd5) + "', '" + std::string(kChartSha) +
                     "', 0, 'Forced CN', 'Artist', 1500, 2000, 500, 2, "
                     "700, 100, 10, 2, 3, 4, 5, 6, 75.0, " +
                     std::to_string(kClearTypeHardClearRank) +
                     "); PRAGMA user_version=1;"),
         "legacy forced-LN score fixture creates");
  scoreDatabase.reset();

  ScoreRepository helper(scorePath);
  expect(helper.EnsureSchema(), "legacy forced-LN score database migrates");
  scoreDatabase = openDatabase(scorePath);
  expect(scoreDatabase != nullptr &&
             queryInt(scoreDatabase.get(), "SELECT COUNT(*) FROM scores") == 1,
         "forced-LN migration retains the historical score row");
  expect(scoreDatabase != nullptr &&
             queryInt(scoreDatabase.get(), "SELECT ln_mode FROM scores") ==
                 long_note_mode::kLnValue,
         "forced-LN migration classifies the historical score as classic LN");
  scoreDatabase.reset();

  bms_parser::ChartMeta meta;
  meta.SHA256 = std::string(kChartSha);
  meta.LnMode = long_note_mode::kCnValue;
  meta.TotalLongNotes = 1;
  expect(helper.LoadBestClearRanks().bestRankFor(
             meta, long_note_mode::kHcnValue) == kClearTypeHardClearRank,
         "forced-CN chart inherits its preserved historical classic-LN lamp");
#endif
}

void testLegacyLongNoteScoreMigrationSurvivesUnavailableChartMetadata() {
#if !TARGET_OS_WINDOWS
  struct Case {
    const char *label;
    bool rebuildRequired;
  };
  for (const Case testCase : {
           Case{"empty chart metadata", false},
           Case{"chart metadata rebuild", true},
       }) {
    TempDirectory temp;
    ScopedHomeOverride home(temp.path());
    const std::filesystem::path chartDirectory = Utils::GetDocumentsPath("db");
    std::filesystem::create_directories(chartDirectory);
    const std::filesystem::path chartPath = chartDirectory / "chart.db";
    ChartRepository charts(chartPath);
    expect(charts.EnsureReady(),
           std::string(testCase.label) + " chart schema creates");
    Database chartDatabase = openDatabase(chartPath);
    expect(chartDatabase != nullptr,
           std::string(testCase.label) + " chart database opens");
    if (!chartDatabase) {
      continue;
    }
    if (testCase.rebuildRequired) {
      expect(execute(chartDatabase.get(),
                     "INSERT INTO chart_meta "
                     "(path, md5, sha256, title, ln_mode, total_long_notes, "
                     "total_backspin_notes, source_priority, "
                     "source_archive_size) VALUES ('stale.bms', '" +
                         std::string(kChartMd5) + "', '" +
                         std::string(kChartSha) +
                         "', 'Stale', 2, 1, 0, 0, 0);"
                         "INSERT OR REPLACE INTO chart_meta_rebuild_state "
                         "(id, required) VALUES (1, 1);"),
             "rebuild fixture marks chart metadata unavailable");
    }
    chartDatabase.reset();

    const std::filesystem::path scorePath =
        temp.path() / "legacy-unclassified-score.db";
    Database scoreDatabase = openDatabase(scorePath);
    expect(scoreDatabase != nullptr,
           std::string(testCase.label) + " score database opens");
    if (!scoreDatabase) {
      continue;
    }
    expect(execute(scoreDatabase.get(),
                   "CREATE TABLE scores ("
                   "id INTEGER PRIMARY KEY AUTOINCREMENT, chart_path TEXT, "
                   "chart_md5 TEXT, chart_sha256 TEXT NOT NULL, "
                   "ln_mode INTEGER NOT NULL DEFAULT 0, chart_title TEXT, "
                   "chart_artist TEXT, score INTEGER NOT NULL, "
                   "max_score INTEGER NOT NULL, max_combo INTEGER NOT NULL, "
                   "combo_break INTEGER NOT NULL, pgreat INTEGER NOT NULL, "
                   "great INTEGER NOT NULL, good INTEGER NOT NULL, "
                   "bad INTEGER NOT NULL, poor INTEGER NOT NULL, "
                   "kpoor INTEGER NOT NULL, fast INTEGER NOT NULL, "
                   "slow INTEGER NOT NULL, final_gauge REAL NOT NULL, "
                   "clear_type INTEGER NOT NULL, "
                   "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
                   "INSERT INTO scores (chart_path, chart_md5, chart_sha256, "
                   "ln_mode, chart_title, chart_artist, score, max_score, "
                   "max_combo, combo_break, pgreat, great, good, bad, poor, "
                   "kpoor, fast, slow, final_gauge, clear_type) VALUES "
                   "('legacy.bms', '" +
                       std::string(kChartMd5) + "', '" +
                       std::string(kChartSha) +
                       "', 0, 'Legacy', 'Artist', 1500, 2000, 500, 2, "
                       "700, 100, 10, 2, 3, 4, 5, 6, 75.0, " +
                       std::to_string(kClearTypeHardClearRank) +
                       "); PRAGMA user_version=1;"),
           std::string(testCase.label) + " legacy score fixture creates");
    scoreDatabase.reset();

    ScoreRepository helper(scorePath);
    expect(helper.EnsureSchema(),
           std::string(testCase.label) +
               " does not strand the legacy score migration");
    scoreDatabase = openDatabase(scorePath);
    expect(scoreDatabase != nullptr &&
               queryInt(scoreDatabase.get(), "PRAGMA user_version") ==
                   ScoreRepository::kCurrentSchemaVersion &&
               queryInt(scoreDatabase.get(), "SELECT COUNT(*) FROM scores") ==
                   1 &&
               queryInt(scoreDatabase.get(), "SELECT ln_mode FROM scores") ==
                   long_note_mode::kUnknownValue,
           std::string(testCase.label) +
               " keeps the unclassified row and completes the schema");
    scoreDatabase.reset();

    bms_parser::ChartMeta meta;
    meta.SHA256 = std::string(kChartSha);
    meta.LnMode = long_note_mode::kCnValue;
    meta.TotalLongNotes = 1;
    expect(helper.LoadBestClearRanks().bestRankFor(
               meta, long_note_mode::kHcnValue) == kClearTypeHardClearRank,
           std::string(testCase.label) +
               " retains the historical lamp through legacy fallback");
  }
#endif
}

void testDifficultyCourseKeysTrackCanonicalDefinitions() {
  TempDirectory temporary;
  const auto chartPath = temporary.path() / "difficulty-course-keys.db";
  ChartRepository chartDb(chartPath);
  auto chartSession = chartDb.OpenSession();
  expect(chartSession.has_value(), "difficulty course key session opens");
  if (!chartSession) {
    return;
  }
  Database chartDatabase = openDatabase(chartPath);
  expect(chartDatabase != nullptr, "difficulty course key database opens");
  if (!chartDatabase) {
    return;
  }

  constexpr std::string_view md5A = "11111111111111111111111111111111";
  constexpr std::string_view md5B = "22222222222222222222222222222222";
  constexpr std::string_view shaA =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  constexpr std::string_view shaB =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  constexpr const char *sourceUrl = "https://example.test/table.json";
  const std::vector<course_identity::ChartIdentity> md5Definition = {
      {.md5 = std::string(md5A)}, {.md5 = std::string(md5B)}};
  const std::vector<course_identity::ChartIdentity> enrichedDefinition = {
      {.sha256 = std::string(shaA), .md5 = std::string(md5A)},
      {.sha256 = std::string(shaB), .md5 = std::string(md5B)}};
  const std::string enrichedUnconstrainedKey =
      course_identity::makeCourseKey(enrichedDefinition, "[]");

  const auto makeChartJson = [](std::string_view md5, std::string_view sha,
                                std::string_view level,
                                std::string_view displayToken,
                                bool includeSha) {
    std::string result = "{\"md5\":\"" + std::string(md5) + "\"";
    if (includeSha) {
      result += ",\"sha256\":\"" + std::string(sha) + "\"";
    }
    result += ",\"level\":\"" + std::string(level) + "\",\"title\":\"Chart " +
              std::string(displayToken) + "\",\"artist\":\"Artist " +
              std::string(displayToken) + "\",\"url\":\"https://example.test/" +
              std::string(displayToken) + ".zip\"}";
    return result;
  };
  const auto makeHeader =
      [&](std::string_view tableName, std::string_view courseName,
          std::string_view constraintJson, std::string_view displayToken,
          bool includeSha, bool reverseCharts = false) {
        const std::string first =
            makeChartJson(md5A, shaA, "1", displayToken, includeSha);
        const std::string second =
            makeChartJson(md5B, shaB, "2", displayToken, includeSha);
        const std::string charts =
            reverseCharts ? second + "," + first : first + "," + second;
        return "{\"name\":\"" + std::string(tableName) +
               "\",\"symbol\":\"L\",\"data_url\":\"data.json\","
               "\"course\":[{\"name\":\"" +
               std::string(courseName) +
               "\",\"constraint\":" + std::string(constraintJson) +
               ",\"charts\":[" + charts + "]}]}";
      };
  const auto makeData = [&](std::string_view displayToken, bool includeSha) {
    return "[" + makeChartJson(md5A, shaA, "1", displayToken, includeSha) +
           "," + makeChartJson(md5B, shaB, "2", displayToken, includeSha) + "]";
  };

  expect(replaceDifficultyTableJson(*chartSession,
                                    makeHeader("Original Table",
                                               "Original Group L1", "[]",
                                               "initial", false),
                                    makeData("initial", false), sourceUrl),
         "initial difficulty table import succeeds");
  const int initialCourseId = queryInt(
      chartDatabase.get(), "SELECT id FROM difficulty_courses LIMIT 1");
  expect(initialCourseId > 0, "initial difficulty course receives an id");
  const std::string initialCourseKey = queryString(
      chartDatabase.get(), "SELECT course_key FROM difficulty_courses LIMIT 1");
  expect(initialCourseKey ==
             course_identity::makeCourseKey(md5Definition, "[]"),
         "initial difficulty course receives its ordered stored content key");

  expect(chartSession->EnsureSchema(),
         "course definition local chart schema opens");
  expect(execute(chartDatabase.get(),
                 "INSERT INTO chart_meta(path,md5,sha256) VALUES "
                 "('local-a.bms','" +
                     std::string(md5A) + "','" + std::string(shaA) +
                     "'),('local-b.bms','" + std::string(md5B) + "','" +
                     std::string(shaB) + "')"),
         "local chart metadata supplies stronger course hash evidence");
  const auto initialDefinitions =
      chartSession->SelectDifficultyCourseDefinitions();
  const auto initialDefinition =
      std::find_if(initialDefinitions.begin(), initialDefinitions.end(),
                   [initialCourseId](const auto &definition) {
                     return definition.courseId == initialCourseId;
                   });
  expect(initialDefinition != initialDefinitions.end(),
         "stored course definition is publicly selectable");
  if (initialDefinition != initialDefinitions.end()) {
    expect(initialDefinition->courseKey == initialCourseKey,
           "selected course definition exposes its stored key");
    expect(initialDefinition->charts.size() == 2,
           "selected course definition preserves chart order");
    if (initialDefinition->charts.size() == 2) {
      expect(initialDefinition->charts[0].md5 == md5A &&
                 initialDefinition->charts[0].sha256 == shaA &&
                 initialDefinition->charts[1].md5 == md5B &&
                 initialDefinition->charts[1].sha256 == shaB,
             "selected definitions combine stored and resolved local hashes");
    }
  }

  for (const std::string gradeConstraint :
       {"grade", "grade_mirror", "grade_random"}) {
    const std::string displayToken = "renamed-" + gradeConstraint;
    expect(replaceDifficultyTableJson(
               *chartSession,
               makeHeader("Renamed " + gradeConstraint,
                          "Renamed " + gradeConstraint + " L9",
                          "[\"" + gradeConstraint + "\"]", displayToken, true),
               makeData(displayToken, true), sourceUrl),
           gradeConstraint + " difficulty course refresh succeeds");
    expect(queryInt(chartDatabase.get(),
                    "SELECT id FROM difficulty_courses LIMIT 1") ==
               initialCourseId,
           gradeConstraint +
               " and display metadata preserve the numeric course id");
    expect(queryString(chartDatabase.get(),
                       "SELECT course_key FROM difficulty_courses LIMIT 1") ==
               initialCourseKey,
           gradeConstraint +
               " and display metadata preserve the stored course key");
  }

  const int tableId = queryInt(
      chartDatabase.get(), "SELECT table_id FROM difficulty_courses LIMIT 1");
  const auto selectedCourses =
      chartSession->SelectDifficultyCourses(tableId, "Renamed grade_random");
  expect(selectedCourses.size() == 1 &&
             selectedCourses.front().courseKey == initialCourseKey,
         "difficulty course metadata exposes the canonical key");
  const auto selectedGroups =
      chartSession->SelectDifficultyCourseGroups(tableId);
  const auto singletonGroup =
      std::find_if(selectedGroups.begin(), selectedGroups.end(),
                   [](const auto &group) { return group.courseCount == 1; });
  expect(singletonGroup != selectedGroups.end() &&
             singletonGroup->singletonCourseKey == initialCourseKey,
         "singleton course group metadata exposes the canonical key");

  CoursePlaySession selectedSession;
  selectedSession.courseId = initialCourseId + 1000;
  selectedSession.courseKey = initialCourseKey;
  expect(selectedSession.courseId != initialCourseId &&
             selectedSession.courseKey == initialCourseKey,
         "course play session carries a selected key independently of id");

  expect(replaceDifficultyTableJson(
             *chartSession,
             makeHeader("Order Change", "Order Change L1", "[\"grade_random\"]",
                        "order", true, true),
             makeData("order", true), sourceUrl),
         "reordered difficulty course import succeeds");
  std::vector<course_identity::ChartIdentity> reversedDefinition =
      enrichedDefinition;
  std::ranges::reverse(reversedDefinition);
  const std::string reorderedCourseKey = queryString(
      chartDatabase.get(), "SELECT course_key FROM difficulty_courses LIMIT 1");
  expect(reorderedCourseKey == course_identity::makeCourseKey(
                                   reversedDefinition, "[\"grade_random\"]") &&
             reorderedCourseKey != enrichedUnconstrainedKey,
         "changed chart order receives a new course key");

  const std::vector<std::pair<std::string, std::string>> identityConstraints = {
      {"gauge", "[\"gauge_7k\"]"},
      {"no-speed", "[\"no_speed\"]"},
      {"judgement", "[\"no_good\"]"},
      {"forced-LN", "[\"ln\"]"},
  };
  for (const auto &[label, constraintJson] : identityConstraints) {
    expect(replaceDifficultyTableJson(*chartSession,
                                      makeHeader("Constraint " + label,
                                                 "Constraint " + label + " L1",
                                                 constraintJson, label, true),
                                      makeData(label, true), sourceUrl),
           label + " constrained difficulty course import succeeds");
    const std::string constrainedCourseKey =
        queryString(chartDatabase.get(),
                    "SELECT course_key FROM difficulty_courses LIMIT 1");
    expect(constrainedCourseKey == course_identity::makeCourseKey(
                                       enrichedDefinition, constraintJson) &&
               constrainedCourseKey != enrichedUnconstrainedKey,
           label + " constraint receives a new course key");
  }
}

void testDifficultyCourseImportRejectsAmbiguousCounterpartHashes() {
  TempDirectory temporary;
  const auto chartPath = temporary.path() / "ambiguous-difficulty-import.db";
  ChartRepository chartDb(chartPath);
  auto chartSession = chartDb.OpenSession();
  expect(chartSession.has_value(),
         "ambiguous difficulty table evidence session opens");
  if (!chartSession) {
    return;
  }
  Database chartDatabase = openDatabase(chartPath);
  expect(chartDatabase != nullptr,
         "ambiguous difficulty table evidence database opens");
  if (!chartDatabase) {
    return;
  }

  constexpr std::string_view sharedMd5 = "33333333333333333333333333333333";
  constexpr std::string_view sharedSha =
      "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
  constexpr std::string_view shaC =
      "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
  constexpr std::string_view shaD =
      "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
  constexpr std::string_view md5D = "44444444444444444444444444444444";
  constexpr std::string_view md5E = "55555555555555555555555555555555";
  const std::string header =
      "{\"name\":\"Ambiguous Import\",\"symbol\":\"A\","
      "\"data_url\":\"data.json\",\"course\":["
      "{\"name\":\"MD5 A1\",\"constraint\":[],\"charts\":[{\"md5\":\"" +
      std::string(sharedMd5) +
      "\"}]},{\"name\":\"SHA A2\",\"constraint\":[],\"charts\":[{"
      "\"sha256\":\"" +
      std::string(sharedSha) + "\"}]}]}";
  const std::string data =
      "[{\"md5\":\"" + std::string(sharedMd5) + "\",\"sha256\":\"" +
      std::string(shaC) + "\"},{\"md5\":\"" + std::string(sharedMd5) +
      "\",\"sha256\":\"" + std::string(shaD) + "\"},{\"md5\":\"" +
      std::string(md5D) + "\",\"sha256\":\"" + std::string(sharedSha) +
      "\"},{\"md5\":\"" + std::string(md5E) + "\",\"sha256\":\"" +
      std::string(sharedSha) + "\"}]";

  expect(
      replaceDifficultyTableJson(*chartSession, header, data,
                                 "https://example.test/ambiguous-import.json"),
      "ambiguous counterpart table imports without inventing identity");

  expect(queryString(chartDatabase.get(),
                     "SELECT dce.md5 FROM difficulty_course_entries dce JOIN "
                     "difficulty_courses dc ON dc.id=dce.course_id WHERE "
                     "dc.name='MD5 A1'") == sharedMd5 &&
             queryString(
                 chartDatabase.get(),
                 "SELECT dce.sha256 FROM difficulty_course_entries dce JOIN "
                 "difficulty_courses dc ON dc.id=dce.course_id WHERE "
                 "dc.name='MD5 A1'")
                 .empty(),
         "conflicting SHA candidates preserve only the directly stored MD5");
  expect(
      queryString(chartDatabase.get(),
                  "SELECT dce.sha256 FROM difficulty_course_entries dce "
                  "JOIN difficulty_courses dc ON dc.id=dce.course_id WHERE "
                  "dc.name='SHA A2'") == sharedSha &&
          queryString(chartDatabase.get(),
                      "SELECT dce.md5 FROM difficulty_course_entries dce JOIN "
                      "difficulty_courses dc ON dc.id=dce.course_id WHERE "
                      "dc.name='SHA A2'")
              .empty(),
      "conflicting MD5 candidates preserve only the directly stored SHA");

  expect(queryString(chartDatabase.get(),
                     "SELECT course_key FROM difficulty_courses WHERE "
                     "name='MD5 A1'") ==
                 course_identity::makeCourseKey(
                     std::vector<course_identity::ChartIdentity>{
                         {.md5 = std::string(sharedMd5)}},
                     "[]") &&
             queryString(chartDatabase.get(),
                         "SELECT course_key FROM difficulty_courses WHERE "
                         "name='SHA A2'") ==
                 course_identity::makeCourseKey(
                     std::vector<course_identity::ChartIdentity>{
                         {.sha256 = std::string(sharedSha)}},
                     "[]"),
         "ambiguous table evidence keeps canonical directly stored keys");
}

void testDifficultyCourseDefinitionsRejectAmbiguousLocalHashEvidence() {
  TempDirectory temporary;
  const auto chartPath =
      temporary.path() / "ambiguous-local-course-definitions.db";
  SqlStatementTrace trace;
  ScopedSqlStatementTrace observation(trace);
  expect(observation.installed(), "course definition query trace installs");
  ChartRepository chartDb(chartPath);
  auto chartSession = chartDb.OpenSession();
  expect(chartSession.has_value(),
         "ambiguous local course definition session opens");
  if (!chartSession) {
    return;
  }
  Database chartDatabase = openDatabase(chartPath);
  expect(chartDatabase != nullptr,
         "ambiguous local course definition database opens");
  if (!chartDatabase) {
    return;
  }

  constexpr std::string_view sharedMd5 = "66666666666666666666666666666666";
  constexpr std::string_view sharedSha =
      "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  constexpr std::string_view shaA =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  constexpr std::string_view shaB =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  constexpr std::string_view md5A = "77777777777777777777777777777777";
  constexpr std::string_view md5B = "88888888888888888888888888888888";
  const std::string header =
      "{\"name\":\"Ambiguous Local\",\"symbol\":\"A\","
      "\"data_url\":\"data.json\",\"course\":["
      "{\"name\":\"Local MD5 A1\",\"constraint\":[],\"charts\":[{"
      "\"md5\":\"" +
      std::string(sharedMd5) +
      "\"}]},{\"name\":\"Local SHA A2\",\"constraint\":[],\"charts\":[{"
      "\"sha256\":\"" +
      std::string(sharedSha) +
      "\"}]},{\"name\":\"Local MD5 Duplicate A3\",\"constraint\":[],"
      "\"charts\":[{\"md5\":\"" +
      std::string(sharedMd5) + "\"}]}]}";
  const std::string data = "[{\"md5\":\"" + std::string(sharedMd5) +
                           "\"},{\"sha256\":\"" + std::string(sharedSha) +
                           "\"}]";

  expect(
      replaceDifficultyTableJson(*chartSession, header, data,
                                 "https://example.test/ambiguous-local.json"),
      "direct-only local evidence fixture imports");
  expect(chartSession->EnsureSchema(),
         "ambiguous local chart metadata schema opens");
  expect(execute(chartDatabase.get(),
                 "INSERT INTO chart_meta(path,md5,sha256) VALUES "
                 "('ambiguous-md5-a.bms','" +
                     std::string(sharedMd5) + "','" + std::string(shaA) +
                     "'),('ambiguous-md5-b.bms','" + std::string(sharedMd5) +
                     "','" + std::string(shaB) + "'),('ambiguous-sha-a.bms','" +
                     std::string(md5A) + "','" + std::string(sharedSha) +
                     "'),('ambiguous-sha-b.bms','" + std::string(md5B) + "','" +
                     std::string(sharedSha) + "')"),
         "conflicting local hash counterparts are installed");

  trace.statements.clear();
  const auto definitions = chartSession->SelectDifficultyCourseDefinitions();
  const auto statementCount = [&](std::string_view fragment) {
    return std::ranges::count_if(trace.statements, [&](const std::string &sql) {
      return std::string_view(sql).find(fragment) != std::string_view::npos;
    });
  };
  expect(
      statementCount(
          "SELECT table_id, sha256, md5 FROM difficulty_table_entries ") == 0 &&
          statementCount("SELECT sha256, md5 FROM chart_meta ORDER BY") == 0 &&
          statementCount("FROM difficulty_table_entries WHERE table_id = "
                         "@table_id AND md5 = @hash") == 1 &&
          statementCount("FROM difficulty_table_entries WHERE table_id = "
                         "@table_id AND sha256 = @hash") == 1 &&
          statementCount("FROM chart_meta WHERE md5 = @hash") == 1 &&
          statementCount("FROM chart_meta WHERE sha256 = @hash") == 1,
      "course definition evidence uses deduplicated indexed hash lookups");
  const auto md5Definition = std::find_if(
      definitions.begin(), definitions.end(),
      [](const auto &item) { return item.name == "Local MD5 A1"; });
  const auto shaDefinition = std::find_if(
      definitions.begin(), definitions.end(),
      [](const auto &item) { return item.name == "Local SHA A2"; });
  expect(md5Definition != definitions.end() &&
             md5Definition->charts.size() == 1 &&
             md5Definition->charts.front().md5 == sharedMd5 &&
             md5Definition->charts.front().sha256.empty(),
         "conflicting local SHA evidence does not enrich a stored MD5");
  expect(shaDefinition != definitions.end() &&
             shaDefinition->charts.size() == 1 &&
             shaDefinition->charts.front().sha256 == sharedSha &&
             shaDefinition->charts.front().md5.empty(),
         "conflicting local MD5 evidence does not enrich a stored SHA");
}

void testDifficultyCourseKeySchemaBackfillsWithoutDeletingRows() {
  TempDirectory temporary;
  const auto chartPath = temporary.path() / "legacy-difficulty-course.db";
  Database chartDatabase = openDatabase(chartPath);
  expect(chartDatabase != nullptr, "legacy difficulty course database opens");
  if (!chartDatabase) {
    return;
  }

  expect(
      execute(chartDatabase.get(),
              "CREATE TABLE difficulty_courses("
              "id INTEGER PRIMARY KEY AUTOINCREMENT,table_id INTEGER NOT "
              "NULL,name TEXT NOT NULL,group_name TEXT NOT NULL DEFAULT '',"
              "level TEXT NOT NULL DEFAULT '',constraint_json TEXT NOT NULL "
              "DEFAULT '[]',sort_order INTEGER NOT NULL DEFAULT 0);"
              "CREATE TABLE difficulty_course_entries("
              "id INTEGER PRIMARY KEY AUTOINCREMENT,course_id INTEGER NOT "
              "NULL,level TEXT NOT NULL DEFAULT '',md5 TEXT NOT NULL DEFAULT "
              "'',sha256 TEXT NOT NULL DEFAULT '',sort_order INTEGER NOT "
              "NULL DEFAULT 0);"
              "INSERT INTO difficulty_courses(id,table_id,name,"
              "constraint_json,sort_order) VALUES(42,7,'Legacy','[]',0);"
              "INSERT INTO difficulty_course_entries(course_id,md5,"
              "sort_order) VALUES(42,'11111111111111111111111111111111',0),"
              "(42,'22222222222222222222222222222222',1);"),
      "legacy difficulty course schema is prepared");
  chartDatabase.reset();

  ChartRepository chartDb(chartPath);
  auto chartSession = chartDb.OpenSession();
  expect(chartSession.has_value(), "legacy difficulty course schema migrates");
  if (!chartSession) {
    return;
  }
  chartDatabase = openDatabase(chartPath);
  expect(chartDatabase != nullptr,
         "migrated difficulty course database reopens");
  if (!chartDatabase) {
    return;
  }
  expect(queryInt(chartDatabase.get(),
                  "SELECT COUNT(*) FROM difficulty_courses") == 1 &&
             queryInt(chartDatabase.get(),
                      "SELECT id FROM difficulty_courses LIMIT 1") == 42,
         "course key migration does not delete legacy course rows");
  const std::vector<course_identity::ChartIdentity> legacyDefinition = {
      {.md5 = "11111111111111111111111111111111"},
      {.md5 = "22222222222222222222222222222222"}};
  expect(queryString(chartDatabase.get(),
                     "SELECT course_key FROM difficulty_courses WHERE id=42") ==
                 course_identity::makeCourseKey(legacyDefinition, "[]") &&
             queryInt(chartDatabase.get(),
                      "SELECT COUNT(*) FROM difficulty_course_entries WHERE "
                      "course_id=42") == 2,
         "course key migration preserves both entries and backfills their "
         "exact ordered key");
  expect(queryInt(chartDatabase.get(),
                  "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
                  "name='idx_difficulty_courses_key'") == 1,
         "course key migration creates the lookup index");

  expect(execute(chartDatabase.get(),
                 "UPDATE difficulty_courses SET course_key='retained-key' "
                 "WHERE id=42"),
         "legacy nonempty key is installed");
  expect(chartSession->EnsureSchema(),
         "difficulty course schema migration is safely repeatable");
  expect(queryString(chartDatabase.get(),
                     "SELECT course_key FROM difficulty_courses WHERE id=42") ==
             "retained-key",
         "blank-key backfill preserves an existing nonempty key");
}
} // namespace

int main() {
  testSuccessfulSwitchIsIsolatedAndPersistsOldState();
  testTargetRecoveryRunsAfterBothDatabaseBindsBeforeCacheRefresh();
  testServicePauseFailureAndActivationRollback();
  testRecoveryWarningDoesNotRollbackSuccessfulSwitch();
  testRecoveryExceptionBecomesSanitizedWarning();
  testRollbackRestoresOldBindingsAfterLaterFailure();
  testRecoveryRunsUnderExistingSwitchGuardWithoutDeadlock();
  testRecoverySkipsSameProfileAndFailedDatabaseBinds();
  testSupportedOlderTargetMigratesAtSchemaOwnerBoundary();
  testEveryDeclaredBlockerRejectsWithoutMutation();
  testBackgroundLibraryBlockerSurvivesSceneReplacement();
  testEveryTransactionalFailureRollsBackAllVisibleState();
  testRollbackBindFailureClosesTargetAndFailsClosed();
  testInvalidTargetFailsBeforeSavingOrBinding();
  testInputReplacementNotificationPrecedesApplyAndRollback();
  testPersistentScoreAttachmentFailureRollsBackToSource();
  testDatabasePathReadsAreIndependentSnapshots();
  testRetainedMainMenuSelectionsReloadWithoutProfileLeakage();
  testRealScoreReadBlocksSwitchUntilQueryCompletes();
  testScoreAttachmentPreparationOwnsActivePathSnapshot();
  testPreparedScoreQueryGuardLivesThroughDependentQuery();
  testRealChartDbScoreQueryRetainsPreparedAttachmentGuard();
  testLegacyForcedLongNoteScoreMigrationPreservesLamp();
  testLegacyLongNoteScoreMigrationSurvivesUnavailableChartMetadata();
  testDifficultyCourseKeysTrackCanonicalDefinitions();
  testDifficultyCourseImportRejectsAmbiguousCounterpartHashes();
  testDifficultyCourseDefinitionsRejectAmbiguousLocalHashEvidence();
  testDifficultyCourseKeySchemaBackfillsWithoutDeletingRows();

  if (failures != 0) {
    std::cerr << failures << " profile switch test(s) failed.\n";
    return 1;
  }
  std::cout << "Profile switch tests passed.\n";
  return 0;
}
