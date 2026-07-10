#include "AppSettingsStore.h"

#include "VersionedJson.h"

#include <array>
#include <system_error>
#include <type_traits>

namespace {
using nlohmann::json;

template <typename Value>
void readValue(const json &document, std::string_view key, Value &destination,
               std::vector<std::string> &diagnostics) {
  const auto found = document.find(std::string(key));
  if (found == document.end()) {
    return;
  }
  try {
    destination = found->get<Value>();
  } catch (const json::exception &error) {
    diagnostics.push_back("Invalid setting '" + std::string(key) + "': " +
                          error.what());
  }
}

template <typename Enum>
void readEnum(const json &document, std::string_view key, Enum &destination,
              std::vector<std::string> &diagnostics) {
  static_assert(std::is_enum_v<Enum>);
  int encoded = static_cast<int>(destination);
  readValue(document, key, encoded, diagnostics);
  destination = static_cast<Enum>(encoded);
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
      {"judgementIndicatorWidthScale",
       settings.judgementIndicatorWidthScale},
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
      {"selectedGaugeType", settings.selectedGaugeType},
      {"selectedPlayOption", settings.selectedPlayOption},
      {"selectedLnMode", settings.selectedLnMode},
      {"selectedAssistOption", settings.selectedAssistOption},
      {"selectedPacemakerTarget", settings.selectedPacemakerTarget},
      {"defaultDifficultyTablesSeeded",
       settings.defaultDifficultyTablesSeeded},
      {"audio",
       {{"outputDeviceId", settings.audioVideo.audio.outputDeviceId},
        {"requestedSampleRate",
         settings.audioVideo.audio.requestedSampleRate},
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
  readValue(document, "audioOffsetMs", settings.audioOffsetMs, diagnostics);
  readValue(document, "visualOffsetMs", settings.visualOffsetMs, diagnostics);
  readValue(document, "visibleTimeGreenNumber",
            settings.visibleTimeGreenNumber, diagnostics);
  readValue(document, "visibleTimeUseMilliseconds",
            settings.visibleTimeUseMilliseconds, diagnostics);
  readEnum(document, "visibleTimeBpmStrategy",
           settings.visibleTimeBpmStrategy, diagnostics);
  readValue(document, "inputKeysoundEnabled", settings.inputKeysoundEnabled,
            diagnostics);
  readValue(document, "prepMetronomeEnabled", settings.prepMetronomeEnabled,
            diagnostics);
  readValue(document, "showInvisibleNotes", settings.showInvisibleNotes,
            diagnostics);
  readValue(document, "touchVisualizationEnabled",
            settings.touchVisualizationEnabled, diagnostics);
  readValue(document, "archiveChartPreviewEnabled",
            settings.archiveChartPreviewEnabled, diagnostics);
  readValue(document, "bgaEnabled", settings.bgaEnabled, diagnostics);
  readValue(document, "bgaBrightnessPercent", settings.bgaBrightnessPercent,
            diagnostics);
  readValue(document, "bgaBlurStrength", settings.bgaBlurStrength,
            diagnostics);
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
  readValue(document, "playAreaWidth4K", settings.playAreaWidth4K,
            diagnostics);
  readValue(document, "playAreaWidth5K", settings.playAreaWidth5K,
            diagnostics);
  readValue(document, "playAreaWidth6K", settings.playAreaWidth6K,
            diagnostics);
  readValue(document, "playAreaWidth7K", settings.playAreaWidth7K,
            diagnostics);
  readValue(document, "playAreaWidth8K", settings.playAreaWidth8K,
            diagnostics);
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
  readValue(document, "judgementTextY", settings.judgementTextY,
            diagnostics);
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
  readValue(document, "selectedGaugeType", settings.selectedGaugeType,
            diagnostics);
  readValue(document, "selectedPlayOption", settings.selectedPlayOption,
            diagnostics);
  readValue(document, "selectedLnMode", settings.selectedLnMode, diagnostics);
  readValue(document, "selectedAssistOption", settings.selectedAssistOption,
            diagnostics);
  readValue(document, "selectedPacemakerTarget",
            settings.selectedPacemakerTarget, diagnostics);
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
      readValue(*audio, "masterVolume",
                settings.audioVideo.audio.masterVolume, diagnostics);
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
  result.settings =
      AppSettings::loadLegacyCfg(settingsCfg, &result.diagnostics);
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
