#include "ApplicationUiStateStore.h"

#include <chrono>
#include <filesystem>
#include <iostream>
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
} // namespace

int main() {
  testMissingUsesDeclaredDefault();
  testEveryModeAndAuthoredPositionRoundTrips();
  testPathIsDeviceScoped();
  if (failures != 0) {
    std::cerr << failures << " application UI state test(s) failed\n";
    return 1;
  }
  std::cout << "application UI state store tests passed\n";
  return 0;
}
