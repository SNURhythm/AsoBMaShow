#include "../src/AppSettingsStore.h"
#include "../src/AtomicFile.h"
#include "../src/VersionedJson.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
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

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path(__FILE__).parent_path() / "fixtures" /
         "settings" / name;
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path &path, std::string_view value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

class TempDirectory {
public:
  TempDirectory() {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-settings-" + std::to_string(nonce));
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

AppSettings makeDistinctSettings() {
  AppSettings value;
  value.audioVideo.audio.outputDeviceId = "device:studio";
  value.audioVideo.audio.requestedSampleRate = 96000;
  value.audioVideo.audio.requestedBufferFrames = 256;
  value.audioVideo.audio.masterVolume = 0.81f;
  value.audioVideo.audio.bgmVolume = 0.62f;
  value.audioVideo.audio.keysoundVolume = 0.43f;
  value.audioVideo.video.mode =
      player_settings::DisplayMode::ExclusiveFullscreen;
  value.audioVideo.video.displayIndex = 2;
  value.audioVideo.video.width = 2560;
  value.audioVideo.video.height = 1440;
  value.audioVideo.video.vsync = true;
  value.audioVideo.video.frameCap = 240;
  value.audioOffsetMs = -23;
  value.visualOffsetMs = 41;
  value.visibleTimeGreenNumber = 777;
  value.visibleTimeUseMilliseconds = true;
  value.visibleTimeBpmStrategy =
      AppSettings::VisibleTimeBpmStrategy::MostPrevalent;
  value.inputKeysoundEnabled = false;
  value.prepMetronomeEnabled = true;
  value.showInvisibleNotes = true;
  value.touchVisualizationEnabled = false;
  value.archiveChartPreviewEnabled = false;
  value.bgaEnabled = false;
  value.bgaBrightnessPercent = 37;
  value.bgaBlurStrength = 4.5f;
  value.bgaDisplayMode = AppSettings::BgaDisplayMode::Fill;
  value.laneAngleDegrees = 19.5f;
  value.laneLength = 10.25f;
  value.laneBeamLengthPercent = 61;
  value.noteStartPositionPercent = 33;
  value.floatingLaneCoverEnabled = false;
  value.playAreaWidth4K = 5.1f;
  value.playAreaWidth5K = 5.2f;
  value.playAreaWidth6K = 6.3f;
  value.playAreaWidth7K = 7.4f;
  value.playAreaWidth8K = 8.5f;
  value.playAreaWidth10K = 9.6f;
  value.playAreaWidth14K = 10.7f;
  value.notePriorityMode = AppSettings::NotePriorityMode::Score;
  value.judgementIndicatorEnabled = false;
  value.judgementIndicatorY = 0.22f;
  value.judgementIndicatorWidthScale = 1.45f;
  value.judgementTextY = 0.73f;
  value.judgementIndicatorRenderMode =
      AppSettings::JudgementIndicatorRenderMode::Hud2D;
  value.judgementCounterEnabled = false;
  value.judgementCounterPosition =
      AppSettings::JudgementCounterPosition::Left;
  value.judgementTimingFastSlowCriteria =
      AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow;
  value.judgementTimingMillisecondsCriteria =
      AppSettings::JudgementTimingDisplayCriteria::BadOrBelow;
  value.gaugeBarPosition = AppSettings::GaugeBarPosition::Right;
  value.uiThemeMode = AppSettings::UiThemeMode::Light;
  value.systemPlaybackShowJacket = false;
  value.systemPlaybackShowTitle = false;
  value.systemPlaybackShowArtist = false;
  value.selectedGaugeType = "hard";
  value.selectedPlayOption = "R-RANDOM";
  value.selectedLnMode = "CN";
  value.selectedAssistOption = "DRAG";
  value.selectedPacemakerTarget = "RATE-AAA";
  value.defaultDifficultyTablesSeeded = true;
  value.sanitize();
  return value;
}

void testLegacyFixtureLoadsEverySetting() {
  const auto result =
      AppSettingsStore::LoadLegacyCfg(fixture("legacy-full.cfg"));
  AppSettings expected = makeDistinctSettings();
  expected.audioVideo = player_settings::defaultAudioVideoSettingsForPlatform();
  expect(result.status == AppSettingsLoadStatus::Loaded,
         "complete legacy fixture loads");
  expect(result.settings == expected,
         "complete legacy fixture preserves every legacy field");
}

void testJsonRoundTripIncludesAudioAndVideo() {
  TempDirectory temp;
  const auto path = temp.path() / "settings.json";
  const AppSettings expected = makeDistinctSettings();
  std::string error;
  expect(AppSettingsStore::Save(path, expected, error),
         "versioned settings save succeeds: " + error);
  const auto loaded = AppSettingsStore::Load(path);
  expect(loaded.status == AppSettingsLoadStatus::Loaded,
         "saved settings load");
  expect(loaded.settings == expected,
         "JSON round trip preserves every setting including audio/video");
  expect(readFile(path).find("\"schemaVersion\": 1") != std::string::npos,
         "saved JSON declares schema version 1");
}

void testVersionFixturesAndNoRewrite() {
  TempDirectory missingTemp;
  const auto missing =
      AppSettingsStore::Load(missingTemp.path() / "missing.json");
  AppSettings defaults;
  defaults.sanitize();
  expect(missing.status == AppSettingsLoadStatus::Missing,
         "missing settings report Missing");
  expect(missing.settings == defaults, "missing settings return defaults");

  const auto v0 = AppSettingsStore::Load(fixture("settings-v0.json"));
  expect(v0.status == AppSettingsLoadStatus::Loaded,
         "missing schema version migrates from v0");
  expect(v0.settings == makeDistinctSettings(), "v0 migration is lossless");

  const auto v1 = AppSettingsStore::Load(fixture("settings-v1.json"));
  expect(v1.status == AppSettingsLoadStatus::Loaded,
         "v1 fixture loads without migration");
  expect(v1.settings == v0.settings, "v0 migration is idempotent with v1");

  TempDirectory temp;
  for (const auto &[name, expectedStatus] :
       {std::pair{"malformed.json", AppSettingsLoadStatus::Invalid},
        std::pair{"array.json", AppSettingsLoadStatus::Invalid},
        std::pair{"future.json", AppSettingsLoadStatus::FutureVersion}}) {
    const auto target = temp.path() / name;
    if (name == std::string_view("malformed.json")) {
      writeFile(target, "{not-json");
    } else if (name == std::string_view("array.json")) {
      writeFile(target, "[1,2,3]");
    } else {
      writeFile(target, readFile(fixture("settings-future.json")));
    }
    const std::string before = readFile(target);
    const auto result = AppSettingsStore::Load(target);
    expect(result.status == expectedStatus,
           std::string(name) + " fails closed");
    expect(readFile(target) == before,
           std::string(name) + " is never rewritten on load");
  }
}

void testInvalidValuesAreSanitizedWithDiagnostics() {
  TempDirectory temp;
  const auto path = temp.path() / "settings.json";
  writeFile(path,
            R"({"schemaVersion":1,"audioOffsetMs":9999,"laneLength":"bad","audio":{"masterVolume":-2.0},"video":{"frameCap":14}})");
  const auto result = AppSettingsStore::Load(path);
  expect(result.status == AppSettingsLoadStatus::Loaded,
         "document with invalid individual values still loads");
  expect(result.settings.audioOffsetMs == AppSettings::kMaxAudioOffsetMs,
         "numeric value is sanitized");
  expect(result.settings.laneLength == AppSettings::kDefaultLaneLength,
         "wrong-typed value falls back");
  expect(result.settings.audioVideo.audio.masterVolume == 0.0f,
         "nested audio value is sanitized");
  expect(result.settings.audioVideo.video.frameCap == 0,
         "nested video value is sanitized");
  expect(!result.diagnostics.empty(), "invalid values produce diagnostics");
}

void testAtomicFailureRestoresDestinationAndExistingBackup() {
  TempDirectory temp;
  const auto target = temp.path() / "settings.json";
  const auto backup = temp.path() / "settings.json.bak";
  writeFile(target, "current");
  writeFile(backup, "older-backup");

  atomic_file::Operations operations = atomic_file::defaultOperations();
  const auto realReplace = operations.replace;
  bool failedFinalInstall = false;
  operations.replace = [&](const auto &from, const auto &to,
                           std::string &error) {
    if (!failedFinalInstall && from == target.string() + ".tmp" &&
        to == target) {
      failedFinalInstall = true;
      error = "injected final replacement failure";
      return false;
    }
    return realReplace(from, to, error);
  };

  const std::string replacement = "replacement";
  const auto bytes = std::as_bytes(std::span(replacement));
  std::string error;
  expect(!atomic_file::writeWithBackup(target, bytes, error, &operations),
         "injected atomic replacement fails");
  expect(failedFinalInstall, "failure reached final install phase");
  expect(readFile(target) == "current", "prior destination is restored");
  expect(readFile(backup) == "older-backup",
         "pre-existing backup is restored");
  expect(!std::filesystem::exists(target.string() + ".tmp"),
         "failed temporary file is cleaned");
}

void testAtomicSuccessRotatesOneBackupGeneration() {
  TempDirectory temp;
  const auto target = temp.path() / "settings.json";
  const auto backup = temp.path() / "settings.json.bak";
  writeFile(target, "current");
  writeFile(backup, "older-backup");
  const std::string replacement = "replacement";
  std::string error;
  expect(atomic_file::writeWithBackup(
             target, std::as_bytes(std::span(replacement)), error),
         "atomic replacement succeeds: " + error);
  expect(readFile(target) == replacement,
         "successful atomic write installs replacement");
  expect(readFile(backup) == "current",
         "successful atomic write retains one prior generation");
  expect(!std::filesystem::exists(target.string() + ".bak.previous"),
         "successful atomic write removes backup staging file");
}
} // namespace

int main() {
  testLegacyFixtureLoadsEverySetting();
  testJsonRoundTripIncludesAudioAndVideo();
  testVersionFixturesAndNoRewrite();
  testInvalidValuesAreSanitizedWithDiagnostics();
  testAtomicFailureRestoresDestinationAndExistingBackup();
  testAtomicSuccessRotatesOneBackupGeneration();
  if (failures != 0) {
    std::cerr << failures << " app settings store assertion(s) failed\n";
    return 1;
  }
  std::cout << "app settings store tests passed\n";
  return 0;
}
