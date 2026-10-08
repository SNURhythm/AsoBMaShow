#include "ApplicationUiStateStore.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>

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
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-application-ui-state-" + std::to_string(nonce));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void testMissingUsesDeclaredDefault() {
  TempDirectory temp;
  const auto result =
      ApplicationUiStateStore::Load(applicationUiStatePath(temp.path()));
  expect(result.status == ApplicationUiStateLoadStatus::Missing,
         "missing state is reported as missing");
  expect(result.state.musicSelectToolbar.mode ==
             MusicSelectToolbarMode::Expanded,
         "missing state starts with an expanded toolbar");
  expect(!result.state.musicSelectToolbar.hasPosition,
         "missing state has no authored position");
}

void testEveryModeAndAuthoredPositionRoundTrips() {
  TempDirectory temp;
  const auto path = applicationUiStatePath(temp.path());
  for (const auto mode : {MusicSelectToolbarMode::Expanded,
                          MusicSelectToolbarMode::Collapsed,
                          MusicSelectToolbarMode::Hidden}) {
    ApplicationUiState expected;
    expected.musicSelectToolbar = {
        .mode = mode, .x = -37.25F, .y = 812.5F, .hasPosition = true};
    std::string diagnostic;
    expect(ApplicationUiStateStore::SaveAtomic(path, expected, diagnostic),
           "application UI state saves atomically: " + diagnostic);
    const auto loaded = ApplicationUiStateStore::Load(path);
    expect(loaded.status == ApplicationUiStateLoadStatus::Loaded,
           "saved application UI state loads");
    expect(loaded.state == expected,
           "toolbar mode and authored floats round-trip exactly");
  }
}

void testLanguagePreferenceRoundTripsAndMigrates() {
  TempDirectory temp;
  const auto path = applicationUiStatePath(temp.path());
  for (const std::string preference : {"system", "en", "ko", "ja", "zh-Hans", "zh-Hant"}) {
    ApplicationUiState state;
    state.language = preference;
    state.musicSelectToolbar.x = 42.0F;
    std::string diagnostic;
    expect(ApplicationUiStateStore::SaveAtomic(path, state, diagnostic),
           "language preference saves");
    expect(ApplicationUiStateStore::Load(path).state == state,
           "language and toolbar survive an application restart");
  }
  nlohmann::json document;
  { std::ifstream input(path); input >> document; }
  document.erase("language");
  { std::ofstream output(path); output << document; }
  expect(ApplicationUiStateStore::Load(path).state.language == "system",
         "older installations follow device language");
  for (const auto invalid : {nlohmann::json("invalid"), nlohmann::json(3)}) {
    document["language"] = invalid;
    { std::ofstream output(path); output << document; }
    const auto loaded = ApplicationUiStateStore::Load(path);
    expect(loaded.state.language == "system" &&
               loaded.state.musicSelectToolbar.x == 42.0F,
           "invalid language falls back without losing toolbar state");
  }
}

void testPathIsDeviceScoped() {
  TempDirectory temp;
  const auto expected = temp.path() / "application-ui-state.json";
  expect(applicationUiStatePath(temp.path()) == expected,
         "application UI state is directly below the application root");
  expect(applicationUiStatePath(temp.path()) !=
             temp.path() / "profiles" / "profile-a" /
                 "application-ui-state.json",
         "application UI state is not profile-scoped");
}

void testDifficultyTableSeedStateSurvivesRestart() {
  TempDirectory temp;
  const auto path = applicationUiStatePath(temp.path());
  ApplicationUiState state;
  state.defaultDifficultyTablesSeeded = true;
  state.bundledDifficultyTablesRevision = 2;
  std::string diagnostic;
  expect(ApplicationUiStateStore::SaveAtomic(path, state, diagnostic),
         "shared table seed state saves");
  expect(ApplicationUiStateStore::Load(path).state == state,
         "both table seed markers survive restart independently of profiles");
  nlohmann::json document;
  { std::ifstream input(path); input >> document; }
  document.erase("defaultDifficultyTablesSeeded");
  document.erase("bundledDifficultyTablesRevision");
  { std::ofstream output(path); output << document; }
  const auto loaded = ApplicationUiStateStore::Load(path);
  expect(!loaded.state.defaultDifficultyTablesSeeded &&
             loaded.state.bundledDifficultyTablesRevision == 0,
         "older application state leaves table seed migration pending");
}

void testLegacyTableSeedFlagsMigrateToRevision() {
  TempDirectory temp;
  const auto path = applicationUiStatePath(temp.path());
  std::string diagnostic;
  expect(ApplicationUiStateStore::SaveAtomic(path, {}, diagnostic),
         "initial state saves");
  nlohmann::json document;
  { std::ifstream input(path); input >> document; }
  document.erase("bundledDifficultyTablesRevision");
  document["defaultDifficultyTablesSeeded"] = true;
  document["aeryDifficultyTablesSeeded"] = true;
  { std::ofstream output(path); output << document; }
  const auto loaded = ApplicationUiStateStore::Load(path);
  expect(ApplicationUiStateStore::SaveAtomic(path, loaded.state, diagnostic),
         "migrated state saves");
  { std::ifstream input(path); input >> document; }
  expect(document.value("bundledDifficultyTablesRevision", 0) == 2,
         "legacy Aery completion migrates to bundled seed revision 2");
  expect(!document.contains("aeryDifficultyTablesSeeded"),
         "new state saves a revision instead of the Aery-specific marker");
  document.erase("bundledDifficultyTablesRevision");
  { std::ofstream output(path); output << document; }
  expect(ApplicationUiStateStore::Load(path).state.bundledDifficultyTablesRevision == 1,
         "legacy original defaults migrate to revision 1 without skipping additions");
  for (const auto invalid : {nlohmann::json(-1), nlohmann::json("2"),
                             nlohmann::json(2.5), nlohmann::json(999999999999ULL)}) {
    document["bundledDifficultyTablesRevision"] = invalid;
    { std::ofstream output(path); output << document; }
    expect(ApplicationUiStateStore::Load(path).state.bundledDifficultyTablesRevision == 1,
           "invalid revisions retain the legacy baseline");
  }
  document["bundledDifficultyTablesRevision"] = 7;
  document["aeryDifficultyTablesSeeded"] = true;
  { std::ofstream output(path); output << document; }
  const auto future = ApplicationUiStateStore::Load(path);
  expect(future.state.bundledDifficultyTablesRevision == 7,
         "older builds preserve a newer seed revision despite stale legacy flags");
  expect(ApplicationUiStateStore::SaveAtomic(path, future.state, diagnostic),
         "newer seed revision saves");
  expect(ApplicationUiStateStore::Load(path).state.bundledDifficultyTablesRevision == 7,
         "newer seed revision survives restart");

}

void testTutorialCompletionSurvivesRestart() {
  TempDirectory temp;
  const auto path = applicationUiStatePath(temp.path());
  expect(!ApplicationUiStateStore::Load(path).state.newcomerTutorialCompleted,
         "a new installation offers the tutorial");
  ApplicationUiState state;
  state.newcomerTutorialCompleted = true;
  state.language = "ko";
  std::string diagnostic;
  expect(ApplicationUiStateStore::SaveAtomic(path, state, diagnostic),
         "tutorial completion saves");
  expect(ApplicationUiStateStore::Load(path).state == state,
         "finishing or skipping the tutorial persists with the chosen language");
  nlohmann::json document;
  { std::ifstream input(path); input >> document; }
  document.erase("newcomerTutorialCompleted");
  { std::ofstream output(path); output << document; }
  expect(!ApplicationUiStateStore::Load(path).state.newcomerTutorialCompleted,
         "older state without tutorial completion offers the tour once");
  document["newcomerTutorialCompleted"] = "true";
  { std::ofstream output(path); output << document; }
  const auto loaded = ApplicationUiStateStore::Load(path);
  expect(!loaded.state.newcomerTutorialCompleted && loaded.state.language == "ko",
         "invalid completion does not hide the tutorial or reset language");
}
} // namespace

int main() {
  testMissingUsesDeclaredDefault();
  testEveryModeAndAuthoredPositionRoundTrips();
  testPathIsDeviceScoped();
  testLanguagePreferenceRoundTripsAndMigrates();
  testTutorialCompletionSurvivesRestart();
  testDifficultyTableSeedStateSurvivesRestart();
  testLegacyTableSeedFlagsMigrateToRevision();
  if (failures != 0) {
    std::cerr << failures << " application UI state test(s) failed\n";
    return 1;
  }
  std::cout << "application UI state store tests passed\n";
  return 0;
}
