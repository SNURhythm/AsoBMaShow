#include "../src/AppSettingsStore.h"
#include "../src/ChartDBHelper.h"
#include "../src/ProfileDatabaseActivity.h"
#include "../src/ProfileDatabaseTools.h"
#include "../src/ProfileSessionCoordinator.h"
#include "../src/ReplayDBHelper.h"
#include "../src/ScoreCacheQueries.h"
#include "../src/ScoreDBHelper.h"
#include "../src/input/InputProfileStore.h"
#include "../src/scene/MainMenuProfileSelections.h"
#include "../src/sqlite3.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
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
    !std::is_move_constructible_v<ScoreDBHelper::PreparedScoreQueryDatabase>);
static_assert(
    !std::is_move_assignable_v<ScoreDBHelper::PreparedScoreQueryDatabase>);

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
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

std::filesystem::path attachedDatabasePath(sqlite3 *database,
                                           std::string_view schema) {
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, "PRAGMA database_list", -1, &statement,
                         nullptr) != SQLITE_OK) {
    return {};
  }
  std::filesystem::path path;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    if (sqliteColumnString(statement, 1) == schema) {
      path = sqliteColumnString(statement, 2);
      break;
    }
  }
  sqlite3_finalize(statement);
  return path;
}

struct DenyNextAttach {
  bool pending = true;
};

int denyNextAttach(void *rawState, int action, const char *, const char *,
                   const char *, const char *) {
  auto &state = *static_cast<DenyNextAttach *>(rawState);
  if (action == SQLITE_ATTACH && state.pending) {
    state.pending = false;
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}

struct BlockingStatementTrace {
  std::mutex mutex;
  std::condition_variable condition;
  std::string fragment;
  bool entered = false;
  bool released = false;
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

ReplayData sampleReplay(const std::filesystem::path &root, int score) {
  ReplayData replay;
  replay.chartMeta.BmsPath = root / "BMS" / "chart.bms";
  replay.chartMeta.MD5 = kChartMd5;
  replay.chartMeta.SHA256 = kChartSha;
  replay.chartMeta.Title = "Chart";
  replay.chartMeta.Artist = "Artist";
  replay.chartMeta.TotalNotes = 1;
  replay.finalScore = score;
  replay.maxCombo = 1;
  replay.finalGauge = 75.0f;
  replay.clearType = kClearTypeNormalClearRank;
  replay.events.push_back({.action = ReplayEventAction::Press,
                           .lane = 1,
                           .noteTimeMicros = 1000,
                           .songTimeMicros = 1000,
                           .judgeTimeMicros = 1000,
                           .judgement = PGreat,
                           .gauge = 75.0f,
                           .gaugeType = GaugeType::Normal,
                           .combo = 1,
                           .score = score});
  replay.provenance = ScoreProvenance::Legacy();
  return replay;
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
  std::optional<std::string> blocker;
  ProfileSwitchBlockers operationBlockers;
  int refreshCount = 0;
  std::vector<std::string> inputReplacementEvents;
  std::function<void()> refreshAction;
  std::filesystem::path appliedInputPath;
  InputProfile currentInput = makeDefaultInputProfile();
  PlayerProfileManager manager;
  std::string firstId;
  std::string secondId;
  PlayerProfilePaths firstPaths;
  PlayerProfilePaths secondPaths;
  AppSettings currentSettings;
  ScoreDBHelper score;
  ReplayDBHelper replay;
  Database persistentChartDatabase;
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
    firstSettings.selectedGaugeType = "gas";
    firstSettings.selectedPlayOption = "MIRROR";
    firstSettings.selectedLnMode = "LN";
    firstSettings.selectedAssistOption = "DRAG";
    firstSettings.selectedPacemakerTarget = "A";
    firstSettings.sanitize();
    AppSettings secondSettings;
    secondSettings.audioOffsetMs = 42;
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
    ReplayDBHelper firstReplay(firstPaths.replaysDb);
    ReplayDBHelper secondReplay(secondPaths.replaysDb);
    expect(firstReplay.SaveReplay(sampleReplay(temp.path(), 500)).has_value(),
           "first replay seed saves");
    expect(secondReplay.SaveReplay(sampleReplay(temp.path(), 1500)).has_value(),
           "second replay seed saves");
    expect(secondReplay.SaveReplay(sampleReplay(temp.path(), 1600)).has_value(),
           "second profile's additional replay seed saves");

    currentSettings = firstSettings;
    currentInput = firstInput;
    appliedInputPath = firstPaths.inputJson;
    score.SetDatabasePath(firstPaths.scoresDb);
    replay.SetDatabasePath(firstPaths.replaysDb);
  }

  PlayerProfileManagerDependencies makeManagerDependencies() {
    PlayerProfileManagerDependencies dependencies;
    dependencies.generateUuid = [this] { return ids.at(nextId++); };
    dependencies.utcNow = [] { return "2026-07-11T12:34:56Z"; };
    dependencies.beforeMigrationPhase = [this](ProfileMigrationPhase phase,
                                               std::string &error) {
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
    dependencies.bindScore = [this](ScoreDBHelper &helper,
                                    const std::filesystem::path &path,
                                    std::string &error) {
      if (failScoreBind) {
        error = "injected score bind failure";
        return false;
      }
      return helper.BindDatabasePath(path, error);
    };
    dependencies.bindReplay = [this](ReplayDBHelper &helper,
                                     const std::filesystem::path &path,
                                     std::string &error) {
      if (failReplayBind) {
        error = "injected replay bind failure";
        return false;
      }
      return helper.BindDatabasePath(path, error);
    };
    dependencies.beforeInputReplacement = [this]() {
      inputReplacementEvents.emplace_back("before");
    };
    return dependencies;
  }

  bool applyInput(const std::filesystem::path &path, std::string &error) {
    inputReplacementEvents.emplace_back("apply");
    const auto loaded = InputProfileStore::load(path);
    if (loaded.status != InputProfileLoadStatus::Loaded) {
      error = "unable to apply input profile";
      return false;
    }
    currentInput = loaded.profile;
    appliedInputPath = path;
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
    }
  }

  void refreshCaches() {
    ++refreshCount;
    if (failRefreshOnce) {
      failRefreshOnce = false;
      throw std::runtime_error("injected cache refresh failure");
    }
    if (refreshAction) {
      refreshAction();
    }
    if (persistentChartDatabase) {
      auto prepared =
          score.PrepareScoreQueryDatabase(persistentChartDatabase.get());
      if (const auto &error = prepared.error()) {
        throw std::runtime_error(*error);
      }
    }
  }

  [[nodiscard]] int currentClearRank() {
    return score.LoadBestClearRanks().bestRankForStoredKey(kChartSha, 0);
  }

  [[nodiscard]] std::size_t currentReplayCount() {
    return replay.ListReplays(sampleReplay(temp.path(), 0).chartMeta, 0).size();
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
             fixture.currentSettings.selectedPlayOption == "MIRROR",
         std::string(label) + " restores current settings");
  expect(fixture.appliedInputPath == fixture.firstPaths.inputJson &&
             firstBindingId(fixture.currentInput) == "first-profile-binding",
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
             fixture.currentSettings.selectedPlayOption == "R-RANDOM",
         "successful switch installs target settings");
  expect(firstBindingId(fixture.currentInput) == "second-profile-binding" &&
             fixture.appliedInputPath == fixture.secondPaths.inputJson,
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
    expect(ScoreDBHelper::HasActiveWrites() &&
               ReplayDBHelper::HasActiveWrites(),
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
             std::vector<std::string>{"before", "apply", "before",
                                      "restore"},
         "rollback cancels pending input work before restoring A");
}

void testPersistentScoreAttachmentFailureRollsBackToSource() {
  SwitchFixture fixture;
  if (fixture.firstId.empty()) {
    return;
  }
  fixture.persistentChartDatabase = openDatabase(":memory:");
  expect(fixture.persistentChartDatabase != nullptr,
         "persistent chart connection opens");
  if (!fixture.persistentChartDatabase) {
    return;
  }
  const auto initialError = score_cache_queries::prepareScoreQueryDatabase(
      fixture.persistentChartDatabase.get(), fixture.firstPaths.scoresDb);
  expect(!initialError.has_value(),
         "persistent chart connection initially attaches profile A");
  const std::string bootstrapBefore =
      readFile(fixture.temp.path() / "active-profile.json");

  DenyNextAttach denyState;
  expect(sqlite3_set_authorizer(fixture.persistentChartDatabase.get(),
                                denyNextAttach, &denyState) == SQLITE_OK,
         "one-shot profile B attach failure installs");
  const ProfileSwitchResult result =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(sqlite3_set_authorizer(fixture.persistentChartDatabase.get(), nullptr,
                                nullptr) == SQLITE_OK,
         "one-shot profile B attach failure clears");

  expect(!result.ok(), "score cache reattach failure aborts profile switch");
  expect(!denyState.pending, "target score attach was attempted");
  expect(std::filesystem::canonical(attachedDatabasePath(
             fixture.persistentChartDatabase.get(), "score_db")) ==
             std::filesystem::canonical(fixture.firstPaths.scoresDb),
         "rollback refresh reattaches profile A score database");
  expect(queryInt(fixture.persistentChartDatabase.get(),
                  "SELECT " + score_cache_queries::scoreBestLookupExpr(
                                  "'" + std::string(kChartSha) + "'", "0",
                                  "score")) == 500,
         "rollback persistent connection queries profile A score data");
  fixture.persistentChartDatabase.reset();
  expectFirstProfileState(fixture, bootstrapBefore,
                          "score attachment refresh failure");
}

void testDatabasePathReadsAreIndependentSnapshots() {
  const std::filesystem::path scoreFirst = "profile-one/scores.db";
  const std::filesystem::path scoreSecond = "profile-two/scores.db";
  ScoreDBHelper score;
  score.SetDatabasePath(scoreFirst);
  const auto &scoreSnapshot = score.GetDatabasePath();
  score.SetDatabasePath(scoreSecond);
  expect(scoreSnapshot == scoreFirst && score.GetDatabasePath() == scoreSecond,
         "score helper path reads are value snapshots, not mutable aliases");

  const std::filesystem::path replayFirst = "profile-one/replays.db";
  const std::filesystem::path replaySecond = "profile-two/replays.db";
  ReplayDBHelper replay;
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
  expect(retained.gaugeType == GaugeType::Hard && !retained.gaugeAutoShift &&
             retained.playOption == "R-RANDOM" &&
             retained.longNoteMode == "HCN" && retained.assistOption == "OFF" &&
             retained.pacemakerTarget == "MAX-",
         "retained MainMenu gameplay selections reload every profile B value");

  AppSettings laterSave;
  retained.applyTo(laterSave);
  expect(laterSave.selectedGaugeType == "hard" &&
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
  expect(rollbackSelections.gaugeType == GaugeType::ExHard &&
             rollbackSelections.gaugeAutoShift &&
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
  expect(ScoreDBHelper::HasActiveReads() && ReplayDBHelper::HasActiveReads(),
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
  expect(!ScoreDBHelper::HasActiveReads() && !ReplayDBHelper::HasActiveReads(),
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
  fixture.persistentChartDatabase = openDatabase(":memory:");
  expect(fixture.persistentChartDatabase != nullptr,
         "guarded score attachment chart connection opens");
  if (!fixture.persistentChartDatabase) {
    return;
  }

  std::optional<std::string> prepareError;
  std::thread preparer([&]() {
    auto prepared = fixture.score.PrepareScoreQueryDatabase(
        fixture.persistentChartDatabase.get());
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

  expect(ScoreDBHelper::HasActiveWrites() && ReplayDBHelper::HasActiveWrites(),
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
  expect(std::filesystem::canonical(attachedDatabasePath(
             fixture.persistentChartDatabase.get(), "score_db")) ==
             std::filesystem::canonical(fixture.firstPaths.scoresDb),
         "blocked switch leaves the retained connection attached to profile "
         "A");

  const ProfileSwitchResult afterPrepare =
      fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
  expect(afterPrepare.ok(),
         "profile switch succeeds after guarded attachment preparation");
  expect(std::filesystem::canonical(attachedDatabasePath(
             fixture.persistentChartDatabase.get(), "score_db")) ==
             std::filesystem::canonical(fixture.secondPaths.scoresDb),
         "successful switch reattaches the retained connection to profile B");
  fixture.persistentChartDatabase.reset();
}

void testPreparedScoreQueryGuardLivesThroughDependentQuery() {
  SwitchFixture fixture;
  if (fixture.firstId.empty()) {
    return;
  }

  ScopedBlockingStatementTrace blockedQuery(
      "FROM score_db.score_sha256_clear_rank_cache");
  expect(blockedQuery.installed(), "score-backed query trace installs");
  fixture.persistentChartDatabase = openDatabase(":memory:");
  expect(fixture.persistentChartDatabase != nullptr,
         "score-backed query chart connection opens");
  if (!fixture.persistentChartDatabase) {
    return;
  }

  std::optional<std::string> prepareError;
  int queryRank = kNoClearTypeRank;
  std::thread query([&]() {
    auto prepared = fixture.score.PrepareScoreQueryDatabase(
        fixture.persistentChartDatabase.get());
    prepareError = prepared.error();
    if (!prepareError.has_value()) {
      queryRank =
          queryInt(fixture.persistentChartDatabase.get(),
                   "SELECT " + score_cache_queries::scoreRankLookupExpr(
                                   "'" + std::string(kChartSha) + "'", "0"));
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

  expect(ScoreDBHelper::HasActiveWrites(),
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
  auto prepared = fixture.score.PrepareScoreQueryDatabase(
      fixture.persistentChartDatabase.get());
  expect(!prepared.error().has_value(),
         "profile B score-backed query preparation succeeds");
  expect(queryInt(fixture.persistentChartDatabase.get(),
                  "SELECT " + score_cache_queries::scoreRankLookupExpr(
                                  "'" + std::string(kChartSha) + "'", "0")) ==
             kClearTypeHardClearRank,
         "post-switch dependent query reads profile B data");
  fixture.persistentChartDatabase.reset();
}

void testRealChartDbScoreQueryRetainsPreparedAttachmentGuard() {
  SwitchFixture fixture;
  if (fixture.firstId.empty()) {
    return;
  }

  ScoreDBHelper &activeScore = ScoreDBHelper::GetInstance();
  const std::filesystem::path previousScorePath = activeScore.GetDatabasePath();
  activeScore.SetDatabasePath(fixture.firstPaths.scoresDb);

  ScopedBlockingStatementTrace blockedQuery(
      "FROM score_db.score_sha256_clear_rank_cache");
  expect(blockedQuery.installed(), "real ChartDB score query trace installs");
  Database chartDatabase = openDatabase(":memory:");
  expect(chartDatabase != nullptr, "real ChartDB score query connection opens");
  if (!chartDatabase) {
    activeScore.SetDatabasePath(previousScorePath);
    return;
  }
  expect(ChartDBHelper::GetInstance().CreateChartMetaTable(chartDatabase.get()),
         "real ChartDB score query schema creates");
  expect(execute(chartDatabase.get(),
                 "INSERT INTO chart_meta "
                 "(path, md5, sha256, title, ln_mode, total_long_notes, "
                 "total_backspin_notes, source_priority, source_archive_size) "
                 "VALUES ('chart.bms', '" +
                     std::string(kChartMd5) + "', '" + std::string(kChartSha) +
                     "', 'Chart', 0, 0, 0, 0, 0)"),
         "real ChartDB score query row inserts");

  ChartMetaQuery firstQuery;
  firstQuery.clearMarkFilter = true;
  firstQuery.clearMarkRank = kClearTypeEasyClearRank;
  int firstCount = -1;
  std::thread query([&]() {
    firstCount = ChartDBHelper::GetInstance().CountChartMeta(
        chartDatabase.get(), firstQuery);
  });
  const bool queryEntered = blockedQuery.waitUntilEntered();
  expect(queryEntered,
         "real ChartDB score-backed count reaches deterministic gate");
  if (!queryEntered) {
    blockedQuery.release();
    query.join();
    activeScore.SetDatabasePath(previousScorePath);
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
  expect(ChartDBHelper::GetInstance().CountChartMeta(chartDatabase.get(),
                                                     secondQuery) == 1,
         "next real ChartDB score-backed count reattaches and reads profile "
         "B");
  activeScore.SetDatabasePath(previousScorePath);
}
} // namespace

int main() {
  testSuccessfulSwitchIsIsolatedAndPersistsOldState();
  testEveryDeclaredBlockerRejectsWithoutMutation();
  testBackgroundLibraryBlockerSurvivesSceneReplacement();
  testEveryTransactionalFailureRollsBackAllVisibleState();
  testInvalidTargetFailsBeforeSavingOrBinding();
  testInputReplacementNotificationPrecedesApplyAndRollback();
  testPersistentScoreAttachmentFailureRollsBackToSource();
  testDatabasePathReadsAreIndependentSnapshots();
  testRetainedMainMenuSelectionsReloadWithoutProfileLeakage();
  testRealScoreReadBlocksSwitchUntilQueryCompletes();
  testScoreAttachmentPreparationOwnsActivePathSnapshot();
  testPreparedScoreQueryGuardLivesThroughDependentQuery();
  testRealChartDbScoreQueryRetainsPreparedAttachmentGuard();

  if (failures != 0) {
    std::cerr << failures << " profile switch test(s) failed.\n";
    return 1;
  }
  std::cout << "Profile switch tests passed.\n";
  return 0;
}
