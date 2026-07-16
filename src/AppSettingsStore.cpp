#include "AppSettingsStore.h"

#include "VersionedJson.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <system_error>
#include <type_traits>
#include <utility>

namespace {
using nlohmann::json;

void invalidValue(std::string_view key, std::string_view reason,
                  std::vector<std::string> &diagnostics) {
  diagnostics.push_back("Invalid setting '" + std::string(key) +
                        "': " + std::string(reason));
}

template <typename Value>
bool readValue(const json &document, std::string_view key, Value &destination,
               std::vector<std::string> &diagnostics) {
  const auto found = document.find(std::string(key));
  if (found == document.end()) {
    return false;
  }
  try {
    if constexpr (std::is_same_v<Value, bool>) {
      if (!found->is_boolean()) {
        invalidValue(key, "expected boolean", diagnostics);
        return false;
      }
      destination = found->get<bool>();
    } else if constexpr (std::is_integral_v<Value>) {
      if (found->is_number_unsigned()) {
        const std::uint64_t encoded = found->get<std::uint64_t>();
        if (encoded >
            static_cast<std::uint64_t>(std::numeric_limits<Value>::max())) {
          invalidValue(key, "integer is out of range", diagnostics);
          return false;
        }
        destination = static_cast<Value>(encoded);
      } else if (found->is_number_integer()) {
        const std::int64_t encoded = found->get<std::int64_t>();
        if constexpr (std::is_unsigned_v<Value>) {
          if (encoded < 0 || static_cast<std::uint64_t>(encoded) >
                                 static_cast<std::uint64_t>(
                                     std::numeric_limits<Value>::max())) {
            invalidValue(key, "integer is out of range", diagnostics);
            return false;
          }
        } else if (encoded < static_cast<std::int64_t>(
                                 std::numeric_limits<Value>::lowest()) ||
                   encoded > static_cast<std::int64_t>(
                                 std::numeric_limits<Value>::max())) {
          invalidValue(key, "integer is out of range", diagnostics);
          return false;
        }
        destination = static_cast<Value>(encoded);
      } else {
        invalidValue(key, "expected integer", diagnostics);
        return false;
      }
    } else if constexpr (std::is_floating_point_v<Value>) {
      if (!found->is_number()) {
        invalidValue(key, "expected number", diagnostics);
        return false;
      }
      const long double encoded =
          found->is_number_unsigned()
              ? static_cast<long double>(found->get<std::uint64_t>())
          : found->is_number_integer()
              ? static_cast<long double>(found->get<std::int64_t>())
              : static_cast<long double>(found->get<double>());
      if (!std::isfinite(encoded) ||
          encoded <
              static_cast<long double>(std::numeric_limits<Value>::lowest()) ||
          encoded >
              static_cast<long double>(std::numeric_limits<Value>::max())) {
        invalidValue(key, "number is out of range", diagnostics);
        return false;
      }
      destination = static_cast<Value>(encoded);
    } else if constexpr (std::is_same_v<Value, std::string>) {
      if (!found->is_string()) {
        invalidValue(key, "expected string", diagnostics);
        return false;
      }
      destination = found->get<std::string>();
    } else {
      destination = found->get<Value>();
    }
  } catch (const json::exception &error) {
    invalidValue(key, error.what(), diagnostics);
    return false;
  }
  return true;
}

template <typename Enum>
void readEnum(const json &document, std::string_view key, Enum &destination,
              std::vector<std::string> &diagnostics) {
  static_assert(std::is_enum_v<Enum>);
  int encoded = static_cast<int>(destination);
  if (readValue(document, key, encoded, diagnostics)) {
    destination = static_cast<Enum>(encoded);
  }
}

json settingsToJson(const AppSettings &settings) {
  return {
      {"schemaVersion", AppSettingsStore::kCurrentSchemaVersion},
      {"audioOffsetMs", settings.audioOffsetMs},
      {"visualOffsetMs", settings.visualOffsetMs},
      {"visibleTimeGreenNumber", settings.visibleTimeGreenNumber},
      {"visibleTimeUseMilliseconds", settings.visibleTimeUseMilliseconds},
      {"visibleTimeBpmStrategy",
       static_cast<int>(settings.visibleTimeBpmStrategy)},
      {"inputKeysoundEnabled", settings.inputKeysoundEnabled},
      {"prepMetronomeEnabled", settings.prepMetronomeEnabled},
      {"startLaneIndicatorsEnabled", settings.startLaneIndicatorsEnabled},
      {"showInvisibleNotes", settings.showInvisibleNotes},
      {"touchVisualizationEnabled", settings.touchVisualizationEnabled},
      {"archiveChartPreviewEnabled", settings.archiveChartPreviewEnabled},
      {"bgaEnabled", settings.bgaEnabled},
      {"bgaBrightnessPercent", settings.bgaBrightnessPercent},
      {"bgaBlurStrength", settings.bgaBlurStrength},
      {"bgaDisplayMode", static_cast<int>(settings.bgaDisplayMode)},
      {"laneAngleDegrees", settings.laneAngleDegrees},
      {"laneLength", settings.laneLength},
      {"laneBeamLengthPercent", settings.laneBeamLengthPercent},
      {"noteStartPositionPercent", settings.noteStartPositionPercent},
      {"floatingLaneCoverEnabled", settings.floatingLaneCoverEnabled},
      {"playAreaWidth4K", settings.playAreaWidth4K},
      {"playAreaWidth5K", settings.playAreaWidth5K},
      {"playAreaWidth6K", settings.playAreaWidth6K},
      {"playAreaWidth7K", settings.playAreaWidth7K},
      {"playAreaWidth8K", settings.playAreaWidth8K},
      {"playAreaWidth10K", settings.playAreaWidth10K},
      {"playAreaWidth14K", settings.playAreaWidth14K},
      {"notePriorityMode", static_cast<int>(settings.notePriorityMode)},
      {"judgementIndicatorEnabled", settings.judgementIndicatorEnabled},
      {"judgementIndicatorY", settings.judgementIndicatorY},
      {"judgementIndicatorWidthScale", settings.judgementIndicatorWidthScale},
      {"judgementTextY", settings.judgementTextY},
      {"judgementIndicatorRenderMode",
       static_cast<int>(settings.judgementIndicatorRenderMode)},
      {"judgementCounterEnabled", settings.judgementCounterEnabled},
      {"judgementCounterPosition",
       static_cast<int>(settings.judgementCounterPosition)},
      {"judgementTimingFastSlowCriteria",
       static_cast<int>(settings.judgementTimingFastSlowCriteria)},
      {"judgementTimingMillisecondsCriteria",
       static_cast<int>(settings.judgementTimingMillisecondsCriteria)},
      {"gaugeBarPosition", static_cast<int>(settings.gaugeBarPosition)},
      {"uiThemeMode", static_cast<int>(settings.uiThemeMode)},
      {"systemPlaybackShowJacket", settings.systemPlaybackShowJacket},
      {"systemPlaybackShowTitle", settings.systemPlaybackShowTitle},
      {"systemPlaybackShowArtist", settings.systemPlaybackShowArtist},
      {"musicPlayerPlaybackRatePercent",
       settings.musicPlayerPlaybackRatePercent},
      {"musicPlayerPlaybackMode",
       static_cast<int>(settings.musicPlayerPlaybackMode)},
      {"gameplayClubModeEnabled", settings.gameplayClubModeEnabled},
      {"musicPlayerClubModeEnabled", settings.musicPlayerClubModeEnabled},
      {"selectedGaugeType", settings.selectedGaugeType},
      {"selectedGaugeAutoShiftMode", settings.selectedGaugeAutoShiftMode},
      {"selectedGaugeAutoShiftLowerBound",
       settings.selectedGaugeAutoShiftLowerBound},
      {"selectedPlayOption", settings.selectedPlayOption},
      {"selectedLnMode", settings.selectedLnMode},
      {"selectedAssistOption", settings.selectedAssistOption},
      {"selectedPacemakerTarget", settings.selectedPacemakerTarget},
      {"selectedPlaybackRatePercent", settings.selectedPlaybackRatePercent},
      {"selectedPlaybackMode", static_cast<int>(settings.selectedPlaybackMode)},
      {"defaultDifficultyTablesSeeded", settings.defaultDifficultyTablesSeeded},
      {"audio",
       {{"outputDeviceId", settings.audioVideo.audio.outputDeviceId},
        {"requestedSampleRate", settings.audioVideo.audio.requestedSampleRate},
        {"requestedBufferFrames",
         settings.audioVideo.audio.requestedBufferFrames},
        {"masterVolume", settings.audioVideo.audio.masterVolume},
        {"bgmVolume", settings.audioVideo.audio.bgmVolume},
        {"keysoundVolume", settings.audioVideo.audio.keysoundVolume}}},
      {"video",
       {{"mode", static_cast<int>(settings.audioVideo.video.mode)},
        {"displayIndex", settings.audioVideo.video.displayIndex},
        {"width", settings.audioVideo.video.width},
        {"height", settings.audioVideo.video.height},
        {"vsync", settings.audioVideo.video.vsync},
        {"frameCap", settings.audioVideo.video.frameCap}}},
  };
}

AppSettings settingsFromJson(const json &document,
                             std::vector<std::string> &diagnostics) {
  AppSettings settings;
  if (!document.contains("selectedGaugeAutoShiftMode") &&
      document.contains("selectedGaugeType")) {
    settings.selectedGaugeAutoShiftMode = "none";
  }
  readValue(document, "audioOffsetMs", settings.audioOffsetMs, diagnostics);
  readValue(document, "visualOffsetMs", settings.visualOffsetMs, diagnostics);
  readValue(document, "visibleTimeGreenNumber", settings.visibleTimeGreenNumber,
            diagnostics);
  readValue(document, "visibleTimeUseMilliseconds",
            settings.visibleTimeUseMilliseconds, diagnostics);
  readEnum(document, "visibleTimeBpmStrategy", settings.visibleTimeBpmStrategy,
           diagnostics);
  readValue(document, "inputKeysoundEnabled", settings.inputKeysoundEnabled,
            diagnostics);
  readValue(document, "prepMetronomeEnabled", settings.prepMetronomeEnabled,
            diagnostics);
  readValue(document, "startLaneIndicatorsEnabled",
            settings.startLaneIndicatorsEnabled, diagnostics);
  readValue(document, "showInvisibleNotes", settings.showInvisibleNotes,
            diagnostics);
  readValue(document, "touchVisualizationEnabled",
            settings.touchVisualizationEnabled, diagnostics);
  readValue(document, "archiveChartPreviewEnabled",
            settings.archiveChartPreviewEnabled, diagnostics);
  readValue(document, "bgaEnabled", settings.bgaEnabled, diagnostics);
  readValue(document, "bgaBrightnessPercent", settings.bgaBrightnessPercent,
            diagnostics);
  readValue(document, "bgaBlurStrength", settings.bgaBlurStrength, diagnostics);
  readEnum(document, "bgaDisplayMode", settings.bgaDisplayMode, diagnostics);
  readValue(document, "laneAngleDegrees", settings.laneAngleDegrees,
            diagnostics);
  readValue(document, "laneLength", settings.laneLength, diagnostics);
  readValue(document, "laneBeamLengthPercent", settings.laneBeamLengthPercent,
            diagnostics);
  readValue(document, "noteStartPositionPercent",
            settings.noteStartPositionPercent, diagnostics);
  readValue(document, "floatingLaneCoverEnabled",
            settings.floatingLaneCoverEnabled, diagnostics);
  readValue(document, "playAreaWidth4K", settings.playAreaWidth4K, diagnostics);
  readValue(document, "playAreaWidth5K", settings.playAreaWidth5K, diagnostics);
  readValue(document, "playAreaWidth6K", settings.playAreaWidth6K, diagnostics);
  readValue(document, "playAreaWidth7K", settings.playAreaWidth7K, diagnostics);
  readValue(document, "playAreaWidth8K", settings.playAreaWidth8K, diagnostics);
  readValue(document, "playAreaWidth10K", settings.playAreaWidth10K,
            diagnostics);
  readValue(document, "playAreaWidth14K", settings.playAreaWidth14K,
            diagnostics);
  readEnum(document, "notePriorityMode", settings.notePriorityMode,
           diagnostics);
  readValue(document, "judgementIndicatorEnabled",
            settings.judgementIndicatorEnabled, diagnostics);
  readValue(document, "judgementIndicatorY", settings.judgementIndicatorY,
            diagnostics);
  readValue(document, "judgementIndicatorWidthScale",
            settings.judgementIndicatorWidthScale, diagnostics);
  readValue(document, "judgementTextY", settings.judgementTextY, diagnostics);
  readEnum(document, "judgementIndicatorRenderMode",
           settings.judgementIndicatorRenderMode, diagnostics);
  readValue(document, "judgementCounterEnabled",
            settings.judgementCounterEnabled, diagnostics);
  readEnum(document, "judgementCounterPosition",
           settings.judgementCounterPosition, diagnostics);
  readEnum(document, "judgementTimingFastSlowCriteria",
           settings.judgementTimingFastSlowCriteria, diagnostics);
  readEnum(document, "judgementTimingMillisecondsCriteria",
           settings.judgementTimingMillisecondsCriteria, diagnostics);
  readEnum(document, "gaugeBarPosition", settings.gaugeBarPosition,
           diagnostics);
  readEnum(document, "uiThemeMode", settings.uiThemeMode, diagnostics);
  readValue(document, "systemPlaybackShowJacket",
            settings.systemPlaybackShowJacket, diagnostics);
  readValue(document, "systemPlaybackShowTitle",
            settings.systemPlaybackShowTitle, diagnostics);
  readValue(document, "systemPlaybackShowArtist",
            settings.systemPlaybackShowArtist, diagnostics);
  readValue(document, "musicPlayerPlaybackRatePercent",
            settings.musicPlayerPlaybackRatePercent, diagnostics);
  readEnum(document, "musicPlayerPlaybackMode",
           settings.musicPlayerPlaybackMode, diagnostics);
  readValue(document, "gameplayClubModeEnabled",
            settings.gameplayClubModeEnabled, diagnostics);
  readValue(document, "musicPlayerClubModeEnabled",
            settings.musicPlayerClubModeEnabled, diagnostics);
  readValue(document, "selectedGaugeType", settings.selectedGaugeType,
            diagnostics);
  readValue(document, "selectedGaugeAutoShiftMode",
            settings.selectedGaugeAutoShiftMode, diagnostics);
  readValue(document, "selectedGaugeAutoShiftLowerBound",
            settings.selectedGaugeAutoShiftLowerBound, diagnostics);
  readValue(document, "selectedPlayOption", settings.selectedPlayOption,
            diagnostics);
  readValue(document, "selectedLnMode", settings.selectedLnMode, diagnostics);
  readValue(document, "selectedAssistOption", settings.selectedAssistOption,
            diagnostics);
  readValue(document, "selectedPacemakerTarget",
            settings.selectedPacemakerTarget, diagnostics);
  readValue(document, "selectedPlaybackRatePercent",
            settings.selectedPlaybackRatePercent, diagnostics);
  readEnum(document, "selectedPlaybackMode", settings.selectedPlaybackMode,
           diagnostics);
  readValue(document, "defaultDifficultyTablesSeeded",
            settings.defaultDifficultyTablesSeeded, diagnostics);

  const auto audio = document.find("audio");
  if (audio != document.end()) {
    if (!audio->is_object()) {
      diagnostics.push_back("Invalid setting 'audio': expected object");
    } else {
      readValue(*audio, "outputDeviceId",
                settings.audioVideo.audio.outputDeviceId, diagnostics);
      readValue(*audio, "requestedSampleRate",
                settings.audioVideo.audio.requestedSampleRate, diagnostics);
      readValue(*audio, "requestedBufferFrames",
                settings.audioVideo.audio.requestedBufferFrames, diagnostics);
      readValue(*audio, "masterVolume", settings.audioVideo.audio.masterVolume,
                diagnostics);
      readValue(*audio, "bgmVolume", settings.audioVideo.audio.bgmVolume,
                diagnostics);
      readValue(*audio, "keysoundVolume",
                settings.audioVideo.audio.keysoundVolume, diagnostics);
    }
  }

  const auto video = document.find("video");
  if (video != document.end()) {
    if (!video->is_object()) {
      diagnostics.push_back("Invalid setting 'video': expected object");
    } else {
      readEnum(*video, "mode", settings.audioVideo.video.mode, diagnostics);
      readValue(*video, "displayIndex", settings.audioVideo.video.displayIndex,
                diagnostics);
      readValue(*video, "width", settings.audioVideo.video.width, diagnostics);
      readValue(*video, "height", settings.audioVideo.video.height,
                diagnostics);
      readValue(*video, "vsync", settings.audioVideo.video.vsync, diagnostics);
      readValue(*video, "frameCap", settings.audioVideo.video.frameCap,
                diagnostics);
    }
  }

  const json beforeSanitize = settingsToJson(settings);
  settings.sanitize();
  if (settingsToJson(settings) != beforeSanitize) {
    diagnostics.push_back("One or more settings were outside valid ranges and "
                          "were sanitized");
  }
  return settings;
}

AppSettingsLoadStatus mapFailure(versioned_json::LoadStatus status) {
  if (status == versioned_json::LoadStatus::FutureVersion) {
    return AppSettingsLoadStatus::FutureVersion;
  }
  if (status == versioned_json::LoadStatus::Missing) {
    return AppSettingsLoadStatus::Missing;
  }
  return AppSettingsLoadStatus::Invalid;
}
} // namespace

AppSettingsLoadResult
AppSettingsStore::Load(const std::filesystem::path &settingsJson) {
  const std::array<versioned_json::Migration, 1> migrations = {
      [](json &document, std::string &) {
        document["schemaVersion"] = 1;
        return true;
      }};
  auto loaded = versioned_json::loadAndMigrate(
      settingsJson, kCurrentSchemaVersion, migrations);

  AppSettingsLoadResult result;
  result.diagnostics = std::move(loaded.diagnostics);
  if (loaded.status != versioned_json::LoadStatus::Loaded) {
    result.status = mapFailure(loaded.status);
    result.settings.sanitize();
    return result;
  }
  result.status = AppSettingsLoadStatus::Loaded;
  result.settings = settingsFromJson(loaded.document, result.diagnostics);
  return result;
}

AppSettingsLoadResult
AppSettingsStore::LoadLegacyCfg(const std::filesystem::path &settingsCfg) {
  AppSettingsLoadResult result;
  std::error_code ec;
  const bool exists = std::filesystem::exists(settingsCfg, ec);
  if (ec) {
    result.status = AppSettingsLoadStatus::Invalid;
    result.diagnostics.push_back("Legacy settings existence check failed: " +
                                 ec.message());
    result.settings.sanitize();
    return result;
  }
  if (!exists) {
    result.status = AppSettingsLoadStatus::Missing;
    result.settings.sanitize();
    return result;
  }
  ec.clear();
  if (!std::filesystem::is_regular_file(settingsCfg, ec) || ec) {
    result.status = AppSettingsLoadStatus::Invalid;
    result.diagnostics.push_back(
        ec ? "Legacy settings type check failed: " + ec.message()
           : "Legacy settings path is not a regular file");
    result.settings.sanitize();
    return result;
  }
  AppSettings parsed;
  parsed.selectedGaugeAutoShiftMode = "none";
  if (!AppSettings::loadLegacyCfg(settingsCfg, parsed, &result.diagnostics)) {
    result.status = AppSettingsLoadStatus::Invalid;
    result.settings.sanitize();
    return result;
  }
  result.settings = std::move(parsed);
  result.status = AppSettingsLoadStatus::Loaded;
  return result;
}

bool AppSettingsStore::Save(const std::filesystem::path &settingsJson,
                            const AppSettings &settings,
                            std::string &errorMessage) {
  AppSettings sanitized = settings;
  sanitized.sanitize();
  return versioned_json::saveAtomic(settingsJson, settingsToJson(sanitized),
                                    errorMessage);
}

#ifdef APP_SETTINGS_STORE_TESTING
AppSettingsLoadResult
AppSettingsStore::LoadLegacyCfgStreamForTesting(std::istream &input) {
  AppSettingsLoadResult result;
  AppSettings parsed;
  parsed.selectedGaugeAutoShiftMode = "none";
  if (!AppSettings::parseLegacyCfg(input, parsed, &result.diagnostics)) {
    result.status = AppSettingsLoadStatus::Invalid;
    result.settings.sanitize();
    return result;
  }
  result.settings = std::move(parsed);
  result.status = AppSettingsLoadStatus::Loaded;
  return result;
}
#endif
