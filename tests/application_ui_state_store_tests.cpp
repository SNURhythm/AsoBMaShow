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
  if (failures != 0) {
    std::cerr << failures << " application UI state test(s) failed\n";
    return 1;
  }
  std::cout << "application UI state store tests passed\n";
  return 0;
}
