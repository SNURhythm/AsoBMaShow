#include "../src/AppSettingsStore.h"
#include "../src/ChartDBHelper.h"
#include "../src/ProfileDatabaseActivity.h"
#include "../src/ProfileDatabaseTools.h"
#include "../src/ProfileSessionCoordinator.h"
#include "../src/ReplayDBHelper.h"
#include "../src/ScoreDBHelper.h"
#include "../src/input/InputProfileStore.h"
#include "../src/sqlite3.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
  int refreshCount = 0;
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
  ProfileSessionCoordinator coordinator;

  SwitchFixture()
      : manager(temp.path(), makeManagerDependencies()), score(), replay(),
        coordinator(
            manager, score, replay, [this]() { return blocker; },
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
    firstSettings.selectedPlayOption = "MIRROR";
    firstSettings.sanitize();
    AppSettings secondSettings;
    secondSettings.audioOffsetMs = 42;
    secondSettings.selectedPlayOption = "R-RANDOM";
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
    return dependencies;
  }

  bool applyInput(const std::filesystem::path &path, std::string &error) {
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
    const ProfileSwitchResult result =
        fixture.coordinator.switchTo(fixture.secondId, fixture.currentSettings);
    expect(result.error == ProfileError::SwitchBlocked,
           "active score/replay write gate rejects profile switch");
  }
  expect(fixture.refreshCount == 0,
         "active database write blocker performs no cache mutation");
  expectFirstProfileState(fixture, bootstrapBefore, "database write blocker");
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
} // namespace

int main() {
  testSuccessfulSwitchIsIsolatedAndPersistsOldState();
  testEveryDeclaredBlockerRejectsWithoutMutation();
  testEveryTransactionalFailureRollsBackAllVisibleState();
  testInvalidTargetFailsBeforeSavingOrBinding();
  testDatabasePathReadsAreIndependentSnapshots();

  if (failures != 0) {
    std::cerr << failures << " profile switch test(s) failed.\n";
    return 1;
  }
  std::cout << "Profile switch tests passed.\n";
  return 0;
}
