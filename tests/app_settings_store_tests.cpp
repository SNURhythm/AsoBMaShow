#include "../src/AppSettingsStore.h"
#include "../src/AtomicFile.h"
#include "../src/VersionedJson.h"
#include "../src/skin/SkinProfileSettings.h"
#include "../src/skin/package/SkinPathPolicy.h"
#include "../yoga/lib/nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <streambuf>
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

bool hasDiagnostic(const std::vector<std::string> &diagnostics,
                   std::string_view key, std::string_view detail) {
  return std::ranges::any_of(diagnostics, [&](const std::string &diagnostic) {
    return diagnostic.find(key) != std::string::npos &&
           diagnostic.find(detail) != std::string::npos;
  });
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
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
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
  value.startLaneIndicatorsEnabled = false;
  value.showInvisibleNotes = true;
  value.touchVisualizationEnabled = false;
  value.archiveChartPreviewEnabled = false;
  value.findBmsSkipUnarchivingForNonSolidArchives = true;
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
  value.judgementCounterPosition = AppSettings::JudgementCounterPosition::Left;
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
  value.selectedGaugeAutoShiftMode = "none";
  value.selectedPlayOption = "R-RANDOM";
  value.selectedLnMode = "CN";
  value.selectedAssistOption = "DRAG";
  value.selectedPacemakerTarget = "AAA";
  value.defaultDifficultyTablesSeeded = true;
  value.sanitize();
  return value;
}

void testLegacyFixtureLoadsEverySetting() {
  const auto result =
      AppSettingsStore::LoadLegacyCfg(fixture("legacy-full.cfg"));
  AppSettings expected = makeDistinctSettings();
  expected.audioVideo = player_settings::defaultAudioVideoSettingsForPlatform();
  expected.findBmsSkipUnarchivingForNonSolidArchives = false;
  expect(result.status == AppSettingsLoadStatus::Loaded,
         "complete legacy fixture loads");
  expect(result.settings == expected,
         "complete legacy fixture preserves every legacy field");
  expect(result.settings.selectedPacemakerTarget == "AAA",
         "legacy fixture discriminates pacemaker persistence");
}

void testJsonRoundTripIncludesAudioAndVideo() {
  TempDirectory temp;
  const auto path = temp.path() / "settings.json";
  AppSettings expected = makeDistinctSettings();
  expected.selectedPlaybackRatePercent = 75;
  expected.selectedPlaybackMode = audio::PlaybackMode::PitchShift;
  expected.musicPlayerPlaybackRatePercent = 135;
  expected.musicPlayerPlaybackMode = audio::PlaybackMode::TimeStretch;
  expected.gameplayClubModeEnabled = true;
  expected.musicPlayerClubModeEnabled = true;
  expected.judgementIndicatorRangeMilliseconds = 333;
  expected.selectedGameplayRuleset = "beatoraja";
  expected.irProviders["tachi"] = {
      .enabled = true,
      .autoSubmit = true,
      .serverOrigin = "https://scores.example.test:8443",
  };
  const auto package = skin::normalizePackageId("ModernChic");
  const auto entry =
      skin::normalizeEntryPath(*package.package, "play/7key.luaskin");
  expected.skin.gameplayCompatibilityEnabled = true;
  expected.skin.selected7KeyEntry = *entry.entry;
  expected.skin.entries[*entry.entry] = {
      .options = {{"Lane", 101}},
      .filePaths = {{"Judge", "parts/judge.png"}},
      .offsets = {{"Judge offset",
                   {.x = 1, .y = -2, .w = 3, .h = 4, .r = 5, .a = -6}}},
      .viewport = {.mode = skin::ViewportMode::Custom,
                   .customBase = skin::CustomViewportBase::Stretch,
                   .scaleX = 1.25F,
                   .scaleY = 0.75F,
                   .translateX = 123.0F,
                   .translateY = -456.0F},
  };
  const std::string expectedConfigurationDigest =
      skin::skinConfigurationDigest(expected.skin.entries.at(*entry.entry));
  std::string error;
  expect(AppSettingsStore::Save(path, expected, error),
         "versioned settings save succeeds: " + error);
  const auto loaded = AppSettingsStore::Load(path);
  expect(loaded.status == AppSettingsLoadStatus::Loaded, "saved settings load");
  expect(loaded.settings == expected,
         "JSON round trip preserves every setting including audio/video");
  expect(skin::skinConfigurationDigest(loaded.settings.skin.entries.at(
             *entry.entry)) == expectedConfigurationDigest,
         "restart reconstructs the exact configuration digest from persisted "
         "entry maps");
  expect(readFile(path).find("\"schemaVersion\": 4") != std::string::npos,
         "saved JSON declares schema version 4");
  expect(readFile(path).find("configurationDigest") == std::string::npos,
         "schema 4 does not persist a competing configuration digest map");
  expect(readFile(path).find("\"package\": \"ModernChic\"") !=
                 std::string::npos &&
             readFile(path).find("\"path\": \"play/7key.luaskin\"") !=
                 std::string::npos &&
             readFile(path).find("collisionKey") == std::string::npos,
         "skin IDs serialize as package/path objects without collision keys");
  expect(readFile(path).find("\"selectedGameplayRuleset\": \"beatoraja\"") !=
             std::string::npos,
         "saved JSON includes the per-profile gameplay ruleset");
  expect(readFile(path).find("\"startLaneIndicatorsEnabled\": false") !=
             std::string::npos,
         "saved JSON includes the start lane indicator setting");
  expect(readFile(path).find("\"judgementIndicatorRangeMilliseconds\": 333") !=
             std::string::npos,
         "saved JSON includes the judgement indicator range");
  expect(readFile(path).find("\"selectedPlaybackRatePercent\": 75") !=
             std::string::npos,
         "saved JSON includes the selected normal-play rate");
  expect(readFile(path).find("\"selectedPlaybackMode\": 0") !=
             std::string::npos,
         "saved JSON includes the selected pitch-shift mode");
  expect(readFile(path).find("\"musicPlayerPlaybackRatePercent\": 135") !=
             std::string::npos,
         "saved JSON includes the music player rate");
  expect(readFile(path).find("\"musicPlayerPlaybackMode\": 1") !=
             std::string::npos,
         "saved JSON includes the music player mode");
  expect(readFile(path).find("\"gameplayClubModeEnabled\": true") !=
             std::string::npos,
         "saved JSON includes gameplay Club mode");
  expect(readFile(path).find("\"musicPlayerClubModeEnabled\": true") !=
             std::string::npos,
         "saved JSON includes music-player Club mode");
  expect(readFile(path).find(
             "\"serverOrigin\": \"https://scores.example.test:8443\"") !=
             std::string::npos,
         "saved JSON includes non-secret IR provider settings");
  expect(readFile(path).find("sentinel-api-key") == std::string::npos,
         "serialized settings contain no API key material");
}

void testSchemaThreeMigrationDisablesCompatibility() {
  TempDirectory temp;
  const auto path = temp.path() / "schema3.json";
  writeFile(path, R"({"schemaVersion":3,"audioOffsetMs":11})");
  const auto loaded = AppSettingsStore::Load(path);
  expect(loaded.status == AppSettingsLoadStatus::Loaded,
         "schema 3 settings migrate to schema 4");
  expect(!loaded.settings.skin.gameplayCompatibilityEnabled,
         "schema 3 migration disables compatibility");
  expect(!loaded.settings.skin.selected7KeyEntry.has_value(),
         "schema 3 migration has no selected gameplay skin");
  expect(loaded.settings.skin.entries.empty(),
         "schema 3 migration starts with no remembered skin entries");
}

void testSkinSettingsRejectUntrustedIdentityAndSanitizeBounds() {
  TempDirectory temp;
  const auto path = temp.path() / "skin.json";
  writeFile(path, R"JSON({
    "schemaVersion": 4,
    "skin": {
      "gameplayCompatibilityEnabled": true,
      "selected7KeyEntry": {"package":"Pack","path":"play/main.luaskin","collisionKey":"forged"},
      "entries": [{
        "entry":{"package":"Pack","path":"play/main.luaskin","collisionKey":"forged"},
        "settings":{
          "options":{"ok":7},
          "filePaths":{"file":"parts/a.png"},
          "offsets":{"offset":{"x":-99999,"y":99999,"w":1,"h":2,"r":3,"a":4}},
          "viewport":{"mode":"bogus","customBase":"bogus","scaleX":1e100,"scaleY":-2,"translateX":1e100,"translateY":-1e100}
        }
      }]
    }
  })JSON");
  const auto loaded = AppSettingsStore::Load(path);
  expect(loaded.status == AppSettingsLoadStatus::Loaded,
         "schema 4 skin settings load");
  expect(loaded.settings.skin.selected7KeyEntry.has_value() &&
             loaded.settings.skin.entries.size() == 1,
         "valid typed selection and matching entry survive");
  if (loaded.settings.skin.selected7KeyEntry) {
    const auto &id = *loaded.settings.skin.selected7KeyEntry;
    expect(id.package.collisionKey == "pack" &&
               id.collisionKey == "pack/play/main.luaskin",
           "collision keys are rederived rather than trusted from JSON");
    const auto &entry = loaded.settings.skin.entries.at(id);
    expect(entry.offsets.at("offset").x == -99999 &&
               entry.offsets.at("offset").y == 99999,
           "offset components preserve their authored integer values");
    expect(entry.viewport.mode == skin::ViewportMode::Fit &&
               entry.viewport.customBase == skin::CustomViewportBase::Fit &&
               entry.viewport.scaleX == 1.0F && entry.viewport.scaleY == 1.0F &&
               entry.viewport.translateX == 0.0F &&
               entry.viewport.translateY == 0.0F,
           "invalid viewport enums and transforms reset deterministically");
  }
}

void testSkinSettingsDeterministicallyEnforceFixedLimits() {
  AppSettings settings;
  settings.skin.gameplayCompatibilityEnabled = true;
  for (int index = 99; index >= 0; --index) {
    const auto package =
        skin::normalizePackageId("package-" + std::to_string(index));
    const auto entry =
        skin::normalizeEntryPath(*package.package, "main.luaskin");
    skin::EntryProfileSettings remembered;
    for (int key = 299; key >= 0; --key) {
      const std::string name = "key-" + std::to_string(key + 1000);
      remembered.options[name] = key;
      remembered.filePaths[name] = "parts/" + std::to_string(key) + ".png";
      remembered.offsets[name] = {
          .x = key, .y = key, .w = key, .h = key, .r = key, .a = key};
    }
    remembered.options[std::string(129, 'x')] = 1;
    remembered.filePaths["absolute"] = "/Users/example/secret.png";
    settings.skin.entries[*entry.entry] = std::move(remembered);
  }
  settings.skin.sanitize();
  expect(settings.skin.entries.size() == 100,
         "skin profile retains every valid entry without an app-defined limit");
  expect(settings.skin.entries.begin()->first.package.directoryName ==
             "package-0",
         "entry truncation is deterministic map order");
  for (const auto &[entry, remembered] : settings.skin.entries) {
    (void)entry;
    expect(remembered.options.size() == 301 &&
               remembered.filePaths.size() == 300 &&
               remembered.offsets.size() == 300,
           "each configuration map retains every valid authored declaration");
    expect(!remembered.filePaths.contains("absolute"),
           "host filesystem paths are never retained in profile settings");
  }
}

void testHostileSkinJsonIsBoundedDuringDecode() {
  TempDirectory temp;
  const auto path = temp.path() / "hostile-skin.json";
  nlohmann::json entries = nlohmann::json::array();
  for (int entryIndex = 0; entryIndex < 70; ++entryIndex) {
    nlohmann::json options = nlohmann::json::object();
    nlohmann::json files = nlohmann::json::object();
    nlohmann::json offsets = nlohmann::json::object();
    for (int keyIndex = 0; keyIndex < 270; ++keyIndex) {
      const std::string key = "key-" + std::to_string(keyIndex + 1000);
      options[key] = keyIndex;
      files[key] = "parts/" + std::to_string(keyIndex) + ".png";
      offsets[key] = {{"x", keyIndex}};
    }
    options[std::string(129, 'k')] = 1;
    files["000-oversized-value"] = std::string(1025, 'v');
    entries.push_back({{"entry",
                        {{"package", "Package-" + std::to_string(entryIndex)},
                         {"path", "play/main.luaskin"}}},
                       {"settings",
                        {{"options", std::move(options)},
                         {"filePaths", std::move(files)},
                         {"offsets", std::move(offsets)}}}});
  }
  entries.push_back(
      {{"entry",
        {{"package", std::string(129, 'p')}, {"path", "main.luaskin"}}},
       {"settings", nlohmann::json::object()}});
  nlohmann::json document = {
      {"schemaVersion", 4},
      {"skin",
       {{"gameplayCompatibilityEnabled", false},
        {"selected7KeyEntry", nullptr},
        {"entries", std::move(entries)}}},
  };
  writeFile(path, document.dump());

  const auto loaded = AppSettingsStore::Load(path);
  expect(loaded.status == AppSettingsLoadStatus::Loaded &&
             loaded.settings.skin.entries.size() == 70,
         "skin JSON retains every valid persisted entry without an app-defined "
         "count limit");
  if (!loaded.settings.skin.entries.empty()) {
    const auto &settings = loaded.settings.skin.entries.begin()->second;
    expect(settings.options.size() == 271 && settings.filePaths.size() == 271 &&
               settings.offsets.size() == 270,
           "skin JSON retains every valid persisted configuration declaration");
    expect(settings.filePaths.contains("000-oversized-value") &&
               settings.filePaths.at("000-oversized-value").size() == 1025,
           "valid Beatoraja file selections are not rejected by an app-defined "
           "text limit");
  }
  expect(!hasDiagnostic(loaded.diagnostics, "options", "limit") &&
             !hasDiagnostic(loaded.diagnostics, "filePaths", "limit") &&
             !hasDiagnostic(loaded.diagnostics, "offsets", "limit"),
         "configuration persistence does not impose count limits absent from "
         "Beatoraja");
}

void testSkinEntryCollisionKeysDeduplicateDeterministically() {
  const auto upperPackage = skin::normalizePackageId("Pack");
  const auto lowerPackage = skin::normalizePackageId("pack");
  const auto upperEntry =
      skin::normalizeEntryPath(*upperPackage.package, "play/main.luaskin");
  const auto lowerEntry =
      skin::normalizeEntryPath(*lowerPackage.package, "play/main.luaskin");
  expect(upperEntry.entry->collisionKey == lowerEntry.entry->collisionKey,
         "collision fixture has one derived case-fold identity");

  skin::SkinProfileSettings direct;
  direct.entries[*lowerEntry.entry].options["variant"] = 2;
  direct.entries[*upperEntry.entry].options["variant"] = 1;
  direct.selected7KeyEntry = *lowerEntry.entry;
  direct.gameplayCompatibilityEnabled = true;
  direct.sanitize();
  expect(direct.entries.size() == 1 &&
             direct.entries.begin()->first.package.directoryName == "Pack" &&
             direct.entries.begin()->second.options.at("variant") == 1 &&
             direct.selected7KeyEntry == direct.entries.begin()->first,
         "sanitize keeps the lexically first derived collision and remaps "
         "selection");

  TempDirectory temp;
  const auto path = temp.path() / "collision-skin.json";
  writeFile(path, R"JSON({
    "schemaVersion":4,
    "skin":{
      "gameplayCompatibilityEnabled":true,
      "selected7KeyEntry":{"package":"pack","path":"play/main.luaskin"},
      "entries":[
        {"entry":{"package":"pack","path":"play/main.luaskin"},"settings":{"options":{"variant":2}}},
        {"entry":{"package":"Pack","path":"play/main.luaskin"},"settings":{"options":{"variant":1}}}
      ]
    }
  })JSON");
  const auto loaded = AppSettingsStore::Load(path);
  expect(
      loaded.status == AppSettingsLoadStatus::Loaded &&
          loaded.settings.skin.entries.size() == 1 &&
          loaded.settings.skin.entries.begin()->first.package.directoryName ==
              "Pack" &&
          loaded.settings.skin.entries.begin()->second.options.at("variant") ==
              1 &&
          loaded.settings.skin.selected7KeyEntry ==
              loaded.settings.skin.entries.begin()->first,
      "JSON load deduplicates derived collisions independent of array "
      "order");
}

std::string nfcAliasKey(int variant) {
  std::string key = "alias-";
  for (int bit = 0; bit < 9; ++bit) {
    key += (variant & (1 << bit)) != 0 ? "\xC3\xA9" : "e\xCC\x81";
  }
  return key;
}

std::string caseAliasPackage(int variant) {
  std::string package = "ALIASPKG";
  for (int bit = 0; bit < 8; ++bit) {
    if ((variant & (1 << bit)) != 0) {
      package[static_cast<std::size_t>(bit)] =
          static_cast<char>(package[static_cast<std::size_t>(bit)] - 'A' + 'a');
    }
  }
  return package;
}

void testDecodeBoundsDerivedUniqueIdentitiesBeforeAllocatingValues() {
  TempDirectory temp;
  const auto path = temp.path() / "unique-bounds-skin.json";
  nlohmann::json entries = nlohmann::json::array();
  nlohmann::json options = nlohmann::json::object();
  nlohmann::json files = nlohmann::json::object();
  nlohmann::json offsets = nlohmann::json::object();
  for (int alias = 0; alias < 257; ++alias) {
    const auto key = nfcAliasKey(alias);
    options[key] = alias;
    files[key] = alias == 0 ? "winner.png" : "other.png";
    offsets[key] = {{"x", alias == 0 ? 123 : alias}};
  }
  options["zzzz-later-unique"] = 9001;
  files["zzzz-later-unique"] = "later.png";
  offsets["zzzz-later-unique"] = {{"x", 321}};
  entries.push_back(
      {{"entry", {{"package", "AAA-NESTED"}, {"path", "play/main.luaskin"}}},
       {"settings",
        {{"options", std::move(options)},
         {"filePaths", std::move(files)},
         {"offsets", std::move(offsets)}}}});
  for (int alias = 0; alias < 65; ++alias) {
    entries.push_back({{"entry",
                        {{"package", caseAliasPackage(alias)},
                         {"path", "play/main.luaskin"}}},
                       {"settings", {{"options", {{"entryWinner", alias}}}}}});
  }
  entries.push_back(
      {{"entry", {{"package", "zzzz-unique"}, {"path", "play/main.luaskin"}}},
       {"settings", {{"options", {{"selected", 1}}}}}});
  nlohmann::json document = {
      {"schemaVersion", 4},
      {"skin",
       {{"gameplayCompatibilityEnabled", true},
        {"selected7KeyEntry",
         {{"package", "zzzz-unique"}, {"path", "play/main.luaskin"}}},
        {"entries", std::move(entries)}}},
  };
  writeFile(path, document.dump());

  const auto loaded = AppSettingsStore::Load(path);
  const auto selectedPackage = skin::normalizePackageId("zzzz-unique");
  const auto selectedEntry =
      skin::normalizeEntryPath(*selectedPackage.package, "play/main.luaskin");
  const auto aliasPackage = skin::normalizePackageId("ALIASPKG");
  const auto aliasEntry =
      skin::normalizeEntryPath(*aliasPackage.package, "play/main.luaskin");
  const auto nestedPackage = skin::normalizePackageId("AAA-NESTED");
  const auto nestedEntry =
      skin::normalizeEntryPath(*nestedPackage.package, "play/main.luaskin");
  const auto selected = loaded.settings.skin.entries.find(*selectedEntry.entry);
  const auto winner = loaded.settings.skin.entries.find(*aliasEntry.entry);
  const auto nested = loaded.settings.skin.entries.find(*nestedEntry.entry);
  const std::string normalizedAlias = "alias-"
                                      "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                                      "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
  expect(loaded.status == AppSettingsLoadStatus::Loaded &&
             loaded.settings.skin.entries.size() == 3 &&
             loaded.settings.skin.selected7KeyEntry == selectedEntry.entry &&
             selected != loaded.settings.skin.entries.end() &&
             winner != loaded.settings.skin.entries.end() &&
             nested != loaded.settings.skin.entries.end(),
         "entry bounds count derived collision identities so a later unique "
         "selected entry survives duplicate aliases");
  if (nested != loaded.settings.skin.entries.end()) {
    expect(nested->second.options.size() == 2 &&
               nested->second.options.at(normalizedAlias) == 0 &&
               nested->second.options.at("zzzz-later-unique") == 9001 &&
               nested->second.filePaths.size() == 2 &&
               nested->second.filePaths.at(normalizedAlias) == "winner.png" &&
               nested->second.filePaths.at("zzzz-later-unique") ==
                   "later.png" &&
               nested->second.offsets.size() == 2 &&
               nested->second.offsets.at(normalizedAlias).x == 123 &&
               nested->second.offsets.at("zzzz-later-unique").x == 321,
           "nested bounds count NFC identities, keep the lexical alias "
           "winner, and retain later unique values");
  }
  if (winner != loaded.settings.skin.entries.end()) {
    expect(winner->second.options.at("entryWinner") == 0,
           "entry collision aliases deterministically keep the lexical "
           "winner's settings");
  }
}

void testFindBmsArchivePreferenceDefaultsAndRoundTrips() {
  AppSettings defaults;
  expect(!defaults.findBmsSkipUnarchivingForNonSolidArchives,
         "Find BMS archive preservation defaults off");

  TempDirectory temp;
  const auto missingPath = temp.path() / "missing-find-bms-option.json";
  writeFile(missingPath, R"({"schemaVersion":3})");
  const auto missing = AppSettingsStore::Load(missingPath);
  expect(missing.status == AppSettingsLoadStatus::Loaded &&
             !missing.settings.findBmsSkipUnarchivingForNonSolidArchives,
         "old settings without the field retain the false default");

  const auto enabledPath = temp.path() / "enabled-find-bms-option.json";
  AppSettings enabled;
  enabled.findBmsSkipUnarchivingForNonSolidArchives = true;
  std::string error;
  expect(AppSettingsStore::Save(enabledPath, enabled, error),
         "Find BMS preference saves: " + error);
  const auto loaded = AppSettingsStore::Load(enabledPath);
  expect(loaded.status == AppSettingsLoadStatus::Loaded &&
             loaded.settings.findBmsSkipUnarchivingForNonSolidArchives,
         "Find BMS preference survives a JSON round trip");
  expect(readFile(enabledPath)
                 .find("\"findBmsSkipUnarchivingForNonSolidArchives\": true") !=
             std::string::npos,
         "saved JSON contains the Find BMS preference");
}

void testJudgementIndicatorRangeDefaultsAndSanitization() {
  AppSettings defaults;
  defaults.sanitize();
  expect(defaults.judgementIndicatorRangeMilliseconds == 180,
         "judgement indicator range defaults to 180 ms");

  AppSettings invalid;
  invalid.judgementIndicatorRangeMilliseconds = 0;
  invalid.sanitize();
  expect(invalid.judgementIndicatorRangeMilliseconds == 180,
         "non-positive stored range uses the default");

  AppSettings excessive;
  excessive.judgementIndicatorRangeMilliseconds = 1001;
  excessive.sanitize();
  expect(excessive.judgementIndicatorRangeMilliseconds == 1000,
         "stored range clamps to the 1000 ms hard cap");

  TempDirectory temp;
  const auto path = temp.path() / "legacy-range-settings.json";
  writeFile(path, R"({"schemaVersion":3,"audioOffsetMs":12})");
  const auto legacy = AppSettingsStore::Load(path);
  expect(legacy.status == AppSettingsLoadStatus::Loaded,
         "settings written before range configuration still load");
  expect(legacy.settings.judgementIndicatorRangeMilliseconds == 180,
         "settings without the range field use 180 ms");

  const auto malformedPath = temp.path() / "malformed-range-settings.json";
  writeFile(
      malformedPath,
      R"({"schemaVersion":3,"judgementIndicatorRangeMilliseconds":"wide"})");
  const auto malformed = AppSettingsStore::Load(malformedPath);
  expect(malformed.status == AppSettingsLoadStatus::Loaded,
         "malformed range does not invalidate the settings document");
  expect(malformed.settings.judgementIndicatorRangeMilliseconds == 180,
         "malformed range falls back to 180 ms");
  expect(hasDiagnostic(malformed.diagnostics,
                       "judgementIndicatorRangeMilliseconds",
                       "expected integer"),
         "malformed range emits a setting diagnostic");
}

void testGameplayRulesetDefaultsMigrationAndValidation() {
  expect(AppSettings{}.selectedGameplayRuleset == "lr2",
         "new settings default gameplay to LR2");

  TempDirectory temp;
  const auto schema2Path = temp.path() / "schema-2.json";
  writeFile(schema2Path,
            R"({"schemaVersion":2,"selectedPlayOption":"MIRROR"})");
  const auto migrated = AppSettingsStore::Load(schema2Path);
  expect(migrated.status == AppSettingsLoadStatus::Loaded,
         "schema-2 settings migrate to schema 3");
  expect(migrated.settings.selectedGameplayRuleset == "lr2",
         "schema-2 migration inserts the LR2 default");
  expect(migrated.settings.selectedPlayOption == "MIRROR",
         "ruleset migration preserves unrelated settings");

  const auto missingPath = temp.path() / "missing-ruleset.json";
  writeFile(missingPath, R"({"schemaVersion":3})");
  const auto missing = AppSettingsStore::Load(missingPath);
  expect(missing.status == AppSettingsLoadStatus::Loaded &&
             missing.settings.selectedGameplayRuleset == "lr2",
         "a current document missing the field still defaults to LR2");

  const auto validPath = temp.path() / "beatoraja.json";
  writeFile(validPath,
            R"({"schemaVersion":3,"selectedGameplayRuleset":"beatoraja"})");
  const auto valid = AppSettingsStore::Load(validPath);
  expect(valid.status == AppSettingsLoadStatus::Loaded &&
             valid.settings.selectedGameplayRuleset == "beatoraja",
         "a valid Beatoraja selection survives loading");

  const auto invalidPath = temp.path() / "invalid-ruleset.json";
  writeFile(
      invalidPath,
      R"({"schemaVersion":3,"selectedGameplayRuleset":"future-ruleset"})");
  const auto invalid = AppSettingsStore::Load(invalidPath);
  expect(invalid.status == AppSettingsLoadStatus::Loaded &&
             invalid.settings.selectedGameplayRuleset == "lr2",
         "an invalid ruleset selection falls back to LR2");
  expect(hasDiagnostic(invalid.diagnostics, "selectedGameplayRuleset",
                       "lr2 or beatoraja"),
         "invalid ruleset emits a field-specific diagnostic");
  expect(std::ranges::none_of(
             invalid.diagnostics,
             [](const std::string &diagnostic) {
               return diagnostic.find("api") != std::string::npos ||
                      diagnostic.find("secret") != std::string::npos ||
                      diagnostic.find("selectedGaugeType") != std::string::npos;
             }),
         "ruleset diagnostics mention neither secrets nor unrelated fields");

  std::istringstream legacyValid("selected_gameplay_ruleset=beatoraja\n");
  const auto parsedValid =
      AppSettingsStore::LoadLegacyCfgStreamForTesting(legacyValid);
  expect(parsedValid.status == AppSettingsLoadStatus::Loaded &&
             parsedValid.settings.selectedGameplayRuleset == "beatoraja",
         "manual legacy CFG can select Beatoraja");
  std::istringstream legacyMissing("selected_play_option=RANDOM\n");
  const auto parsedMissing =
      AppSettingsStore::LoadLegacyCfgStreamForTesting(legacyMissing);
  expect(parsedMissing.settings.selectedGameplayRuleset == "lr2",
         "legacy CFG without a ruleset remains LR2");
}

void testIrDefaultsMigrationAndOriginSanitization() {
  AppSettings defaults;
  const auto defaultProvider = defaults.irProviders.find("tachi");
  expect(defaultProvider != defaults.irProviders.end(),
         "settings include the stable Tachi provider by default");
  if (defaultProvider != defaults.irProviders.end()) {
    expect(!defaultProvider->second.enabled &&
               !defaultProvider->second.autoSubmit &&
               defaultProvider->second.serverOrigin == "https://boku.tachi.ac",
           "Tachi defaults are disabled with the production origin");
  }

  TempDirectory temp;
  const auto migratedPath = temp.path() / "schema-1.json";
  writeFile(migratedPath, R"({"schemaVersion":1,"audioOffsetMs":17})");
  const auto migrated = AppSettingsStore::Load(migratedPath);
  expect(migrated.status == AppSettingsLoadStatus::Loaded,
         "schema-1 settings migrate to schema 2");
  expect(migrated.settings.audioOffsetMs == 17,
         "schema-1 migration preserves existing settings");
  expect(migrated.settings.irProviders.at("tachi") == ir::IrProviderSettings{},
         "schema-1 migration inserts default Tachi settings");

  const auto normalizedPath = temp.path() / "normalized.json";
  writeFile(
      normalizedPath,
      R"({"schemaVersion":2,"ir":{"providers":{"tachi":{"enabled":true,"autoSubmit":true,"serverOrigin":"HTTPS://BOKU.TACHI.AC:443/"}}}})");
  const auto normalized = AppSettingsStore::Load(normalizedPath);
  expect(normalized.status == AppSettingsLoadStatus::Loaded,
         "valid provider settings load");
  expect(normalized.settings.irProviders.at("tachi").serverOrigin ==
             "https://boku.tachi.ac",
         "origin normalization lowercases and removes default syntax");

  const auto insecurePath = temp.path() / "insecure-origin.json";
  writeFile(
      insecurePath,
      R"({"schemaVersion":2,"ir":{"providers":{"tachi":{"enabled":true,"autoSubmit":true,"serverOrigin":"http://LOCALHOST:80/"}}}})");
  const auto insecure = AppSettingsStore::Load(insecurePath);
  expect(insecure.status == AppSettingsLoadStatus::Loaded &&
             insecure.settings.irProviders.at("tachi").serverOrigin ==
                 "http://localhost" &&
             !insecure.settings.irProviders.at("tachi").autoSubmit,
         "HTTP origins remain stored but automatic authenticated work is "
         "disabled");

  const auto invalidPath = temp.path() / "invalid-origin.json";
  writeFile(
      invalidPath,
      R"({"schemaVersion":2,"ir":{"providers":{"tachi":{"enabled":true,"autoSubmit":true,"serverOrigin":"https://secret@example.test/path?key=value#fragment"}}}})");
  const auto invalid = AppSettingsStore::Load(invalidPath);
  expect(invalid.status == AppSettingsLoadStatus::Loaded,
         "invalid origin is an individual setting error");
  expect(invalid.settings.irProviders.at("tachi").serverOrigin ==
             "https://boku.tachi.ac",
         "invalid stored origin falls back to production");
  expect(hasDiagnostic(invalid.diagnostics, "serverOrigin", "origin"),
         "invalid stored origin emits a non-secret diagnostic");
  expect(std::ranges::none_of(invalid.diagnostics,
                              [](const std::string &diagnostic) {
                                return diagnostic.find("secret") !=
                                           std::string::npos ||
                                       diagnostic.find("key=value") !=
                                           std::string::npos;
                              }),
         "origin diagnostics do not echo rejected URL contents");

  for (const auto &[input, expected] :
       {std::pair<std::string_view, std::string_view>{"http://LOCALHOST:80/",
                                                      "http://localhost"},
        {"https://Example.Test:444", "https://example.test:444"},
        {"https://[::1]:443/", "https://[::1]"}}) {
    const auto origin = ir::normalizeServerOrigin(input);
    expect(origin && *origin == expected,
           std::string("normalizes origin: ") + std::string(input));
  }
  const std::vector<std::string> rejectedOrigins = {
      "ftp://example.test",
      "https://",
      "https://user@example.test",
      "https://example.test/path",
      "https://example.test?query",
      "https://example.test#fragment",
      "https://example.test\\path",
      std::string("https://example.test/") + std::string(2048, 'x'),
  };
  for (const std::string &input : rejectedOrigins) {
    expect(!ir::normalizeServerOrigin(input),
           "rejects a non-origin server URL");
  }
}

void testPlaybackSelectionSanitizationAndLegacyDefaults() {
  AppSettings rounded;
  rounded.selectedPlaybackRatePercent = 73;
  rounded.sanitize();
  expect(rounded.selectedPlaybackRatePercent == 75,
         "normal-play rate rounds to the nearest supported step");

  AppSettings clamped;
  clamped.selectedPlaybackRatePercent = 250;
  clamped.sanitize();
  expect(clamped.selectedPlaybackRatePercent == 200,
         "normal-play rate clamps to the supported maximum");

  AppSettings musicPlayerRounded;
  musicPlayerRounded.musicPlayerPlaybackRatePercent = 73;
  musicPlayerRounded.sanitize();
  expect(musicPlayerRounded.musicPlayerPlaybackRatePercent == 75,
         "music-player rate rounds to the nearest supported step");

  AppSettings invalidMusicPlayerMode;
  invalidMusicPlayerMode.musicPlayerPlaybackMode =
      static_cast<audio::PlaybackMode>(99);
  invalidMusicPlayerMode.sanitize();
  expect(invalidMusicPlayerMode.musicPlayerPlaybackMode ==
             audio::PlaybackMode::PitchShift,
         "invalid music-player mode falls back to pitch shift");

  TempDirectory temp;
  const auto path = temp.path() / "legacy-neutral-settings.json";
  writeFile(path, R"({"schemaVersion":1,"audioOffsetMs":12})");
  const auto legacy = AppSettingsStore::Load(path);
  expect(legacy.status == AppSettingsLoadStatus::Loaded,
         "settings written before normal-play controls still load");
  expect(legacy.settings.selectedPlaybackRatePercent == 100,
         "legacy settings default to neutral playback rate");
  expect(legacy.settings.selectedPlaybackMode ==
             audio::PlaybackMode::PitchShift,
         "legacy settings default to pitch-shift playback mode");
  expect(legacy.settings.musicPlayerPlaybackRatePercent == 100,
         "legacy settings default to neutral music-player rate");
  expect(legacy.settings.musicPlayerPlaybackMode ==
             audio::PlaybackMode::PitchShift,
         "legacy settings default to pitch-shift music-player mode");
  expect(!legacy.settings.gameplayClubModeEnabled &&
             !legacy.settings.musicPlayerClubModeEnabled,
         "legacy settings default both Club modes off");
  expect(legacy.settings.startLaneIndicatorsEnabled,
         "settings without the field default start lane indicators on");
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
  AppSettings expectedV0 = makeDistinctSettings();
  expectedV0.findBmsSkipUnarchivingForNonSolidArchives = false;
  expect(v0.settings == expectedV0, "v0 migration is lossless");

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

void testMigrationRunsExactlyOnce() {
  TempDirectory temp;
  const auto v0Path = temp.path() / "observable-v0.json";
  writeFile(v0Path, R"({"value":"before"})");
  int migrationCalls = 0;
  const std::array<versioned_json::Migration, 1> migrations = {
      [&](nlohmann::json &document, std::string &) {
        ++migrationCalls;
        document["value"] = "after";
        return true;
      }};
  const auto migrated = versioned_json::loadAndMigrate(v0Path, 1, migrations);
  expect(migrated.status == versioned_json::LoadStatus::Loaded,
         "observable v0 document migrates");
  expect(migrationCalls == 1, "v0 migration runs exactly once");
  expect(migrated.document.at("schemaVersion") == 1,
         "migration stamps the resulting schema version");
  expect(migrated.document.at("value") == "after",
         "migration result is observable");

  const auto v1Path = temp.path() / "observable-v1.json";
  writeFile(v1Path, R"({"schemaVersion":1,"value":"current"})");
  migrationCalls = 0;
  const auto current = versioned_json::loadAndMigrate(v1Path, 1, migrations);
  expect(current.status == versioned_json::LoadStatus::Loaded,
         "current document loads");
  expect(migrationCalls == 0, "current document does not rerun migration");
  expect(current.document.at("value") == "current",
         "current document remains unchanged");
}

void testOversizedVersionsAndSettingsFailClosed() {
  TempDirectory temp;
  for (const auto &[name, encodedVersion] :
       {std::pair{"future-int.json", "2147483648"},
        std::pair{"future-uint.json", "4294967296"},
        std::pair{"future-max.json", "18446744073709551615"}}) {
    const auto path = temp.path() / name;
    writeFile(path, std::string("{\"schemaVersion\":") + encodedVersion +
                        ",\"audioOffsetMs\":12}");
    const std::string before = readFile(path);
    const auto loaded = AppSettingsStore::Load(path);
    expect(loaded.status == AppSettingsLoadStatus::FutureVersion,
           std::string(name) + " is rejected as a future version");
    expect(readFile(path) == before,
           std::string(name) + " remains byte-for-byte unchanged");
  }

  const auto negativeVersionPath = temp.path() / "invalid-negative.json";
  writeFile(negativeVersionPath,
            R"({"schemaVersion":-2147483649,"audioOffsetMs":12})");
  const std::string negativeBefore = readFile(negativeVersionPath);
  const auto negative = AppSettingsStore::Load(negativeVersionPath);
  expect(negative.status == AppSettingsLoadStatus::Invalid,
         "schema version below INT_MIN is invalid without narrowing");
  expect(readFile(negativeVersionPath) == negativeBefore,
         "invalid negative schema remains byte-for-byte unchanged");

  const auto valuesPath = temp.path() / "oversized-values.json";
  writeFile(
      valuesPath,
      R"({"schemaVersion":1,"audioOffsetMs":4294967296,"visualOffsetMs":-2147483649,"audio":{"requestedSampleRate":4294967296,"requestedBufferFrames":-1}})");
  const auto values = AppSettingsStore::Load(valuesPath);
  expect(values.status == AppSettingsLoadStatus::Loaded,
         "representability failures remain individual-value errors");
  expect(values.settings.audioOffsetMs == 0,
         "oversized signed setting falls back without wrapping");
  expect(values.settings.visualOffsetMs == 0,
         "undersized signed setting falls back without wrapping");
  expect(values.settings.audioVideo.audio.requestedSampleRate == 0,
         "oversized unsigned setting falls back without wrapping");
  expect(values.settings.audioVideo.audio.requestedBufferFrames == 0,
         "negative unsigned setting falls back without wrapping");
  expect(hasDiagnostic(values.diagnostics, "audioOffsetMs", "out of range"),
         "oversized signed setting emits a key-specific range diagnostic");
  expect(hasDiagnostic(values.diagnostics, "visualOffsetMs", "out of range"),
         "undersized signed setting emits a key-specific range diagnostic");
  expect(
      hasDiagnostic(values.diagnostics, "requestedSampleRate", "out of range"),
      "oversized unsigned setting emits a key-specific range diagnostic");
  expect(hasDiagnostic(values.diagnostics, "requestedBufferFrames",
                       "out of range"),
         "negative unsigned setting emits a key-specific range diagnostic");
}

class FailingReadBuffer final : public std::streambuf {
public:
  FailingReadBuffer(std::string contents, std::size_t failAfter)
      : contents_(std::move(contents)), failAfter_(failAfter) {}

protected:
  int_type underflow() override {
    if (position_ >= failAfter_) {
      throw std::ios_base::failure("injected legacy read failure");
    }
    if (position_ >= contents_.size()) {
      return traits_type::eof();
    }
    current_ = contents_[position_++];
    setg(&current_, &current_, &current_ + 1);
    return traits_type::to_int_type(current_);
  }

private:
  std::string contents_;
  std::size_t failAfter_ = 0;
  std::size_t position_ = 0;
  char current_ = 0;
};

void testLegacyIoFailuresAreInvalid() {
  TempDirectory temp;
  const auto directoryAtFilePath = temp.path() / "settings.cfg";
  std::filesystem::create_directory(directoryAtFilePath);
  const auto openFailure = AppSettingsStore::LoadLegacyCfg(directoryAtFilePath);
  expect(openFailure.status == AppSettingsLoadStatus::Invalid,
         "legacy open failure is not reported as loaded defaults");

#ifndef _WIN32
  const auto unreadablePath = temp.path() / "unreadable.cfg";
  writeFile(unreadablePath, "audio_offset_ms=17\n");
  std::filesystem::permissions(unreadablePath, std::filesystem::perms::none);
  const auto actualOpenFailure =
      AppSettingsStore::LoadLegacyCfg(unreadablePath);
  std::filesystem::permissions(unreadablePath,
                               std::filesystem::perms::owner_all);
  expect(actualOpenFailure.status == AppSettingsLoadStatus::Invalid,
         "regular legacy file open failure is reported as invalid");
  expect(hasDiagnostic(actualOpenFailure.diagnostics, "Unable to open", ""),
         "regular legacy file open failure emits an opener diagnostic");
#endif

  FailingReadBuffer buffer("audio_offset_ms=17\nvisual_offset_ms=29\n", 25);
  std::istream failingStream(&buffer);
  const auto readFailure =
      AppSettingsStore::LoadLegacyCfgStreamForTesting(failingStream);
  expect(readFailure.status == AppSettingsLoadStatus::Invalid,
         "legacy mid-read failure is not reported as loaded partial data");
  AppSettings defaults;
  defaults.sanitize();
  expect(readFailure.settings == defaults,
         "legacy read failure discards partially parsed settings");
}

void testInvalidValuesAreSanitizedWithDiagnostics() {
  TempDirectory temp;
  const auto path = temp.path() / "settings.json";
  writeFile(
      path,
      R"({"schemaVersion":1,"audioOffsetMs":9999,"laneLength":"bad","audio":{"masterVolume":-2.0},"video":{"frameCap":1001}})");
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
  bool observedMissingCanonicalPath = false;
  operations.replace = [&](const auto &from, const auto &to,
                           std::string &error) {
    if (!failedFinalInstall && from == target.string() + ".tmp" &&
        to == target) {
      failedFinalInstall = true;
      error = "injected final replacement failure";
      return false;
    }
    const bool replaced = realReplace(from, to, error);
    if (replaced && !std::filesystem::exists(target)) {
      observedMissingCanonicalPath = true;
    }
    return replaced;
  };

  const std::string replacement = "replacement";
  const auto bytes = std::as_bytes(std::span(replacement));
  std::string error;
  expect(!atomic_file::writeWithBackup(target, bytes, error, &operations),
         "injected atomic replacement fails");
  expect(failedFinalInstall, "failure reached final install phase");
  expect(!observedMissingCanonicalPath,
         "backup preparation never vacates the canonical path");
  expect(readFile(target) == "current", "prior destination is restored");
  expect(readFile(backup) == "older-backup", "pre-existing backup is restored");
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

void testAtomicFirstSaveCreatesNestedParents() {
  TempDirectory temp;
  const auto target = temp.path() / "new" / "profile" / "settings.json";
  const std::string contents = "first-save";
  std::string error;
  expect(atomic_file::writeWithBackup(
             target, std::as_bytes(std::span(contents)), error),
         "atomic first save creates and syncs nested parents: " + error);
  expect(readFile(target) == contents,
         "atomic first save installs content under nested parents");
}

void testAtomicFirstSaveCreatesRelativeNestedParents() {
  TempDirectory temp;
  const auto previousDirectory = std::filesystem::current_path();
  std::filesystem::current_path(temp.path());
  const std::filesystem::path target =
      std::filesystem::path("relative") / "profile" / "settings.json";
  const std::string contents = "relative-first-save";
  std::string error;
  const bool saved = atomic_file::writeWithBackup(
      target, std::as_bytes(std::span(contents)), error);
  std::filesystem::current_path(previousDirectory);
  expect(saved, "atomic relative first save syncs nested parents: " + error);
  expect(readFile(temp.path() / target) == contents,
         "atomic relative first save installs nested content");
}
} // namespace

int main() {
  testLegacyFixtureLoadsEverySetting();
  testJsonRoundTripIncludesAudioAndVideo();
  testSchemaThreeMigrationDisablesCompatibility();
  testSkinSettingsRejectUntrustedIdentityAndSanitizeBounds();
  testSkinSettingsDeterministicallyEnforceFixedLimits();
  testHostileSkinJsonIsBoundedDuringDecode();
  testSkinEntryCollisionKeysDeduplicateDeterministically();
  testDecodeBoundsDerivedUniqueIdentitiesBeforeAllocatingValues();
  testFindBmsArchivePreferenceDefaultsAndRoundTrips();
  testJudgementIndicatorRangeDefaultsAndSanitization();
  testGameplayRulesetDefaultsMigrationAndValidation();
  testIrDefaultsMigrationAndOriginSanitization();
  testPlaybackSelectionSanitizationAndLegacyDefaults();
  testVersionFixturesAndNoRewrite();
  testMigrationRunsExactlyOnce();
  testOversizedVersionsAndSettingsFailClosed();
  testLegacyIoFailuresAreInvalid();
  testInvalidValuesAreSanitizedWithDiagnostics();
  testAtomicFailureRestoresDestinationAndExistingBackup();
  testAtomicSuccessRotatesOneBackupGeneration();
  testAtomicFirstSaveCreatesNestedParents();
  testAtomicFirstSaveCreatesRelativeNestedParents();
  if (failures != 0) {
    std::cerr << failures << " app settings store assertion(s) failed\n";
    return 1;
  }
  std::cout << "app settings store tests passed\n";
  return 0;
}
