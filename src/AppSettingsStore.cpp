#include "AppSettingsStore.h"

#include "VersionedJson.h"
#include "scene/play/GameplayRuleset.h"
#include "skin/GameplaySkinTraits.h"
#include "skin/package/SkinPathPolicy.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
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

const char *viewportModeToString(skin::ViewportMode mode) {
  switch (mode) {
  case skin::ViewportMode::Fit:
    return "fit";
  case skin::ViewportMode::Stretch:
    return "stretch";
  case skin::ViewportMode::Custom:
    return "custom";
  }
  return "fit";
}

const char *customViewportBaseToString(skin::CustomViewportBase base) {
  switch (base) {
  case skin::CustomViewportBase::Fit:
    return "fit";
  case skin::CustomViewportBase::Stretch:
    return "stretch";
  }
  return "fit";
}

json skinEntryIdToJson(const skin::SkinEntryId &entry) {
  return {{"package", entry.package.directoryName},
          {"path", entry.packageRelativePath}};
}

json skinProfileSettingsToJson(const skin::SkinProfileSettings &skinSettings) {
  json entries = json::array();
  for (const auto &[entry, settings] : skinSettings.entries) {
    json offsets = json::object();
    for (const auto &[name, offset] : settings.offsets) {
      offsets[name] = {{"x", offset.x}, {"y", offset.y}, {"w", offset.w},
                       {"h", offset.h}, {"r", offset.r}, {"a", offset.a}};
    }
    entries.push_back(
        {{"entry", skinEntryIdToJson(entry)},
         {"settings",
          {{"options", settings.options},
           {"filePaths", settings.filePaths},
           {"offsets", std::move(offsets)},
           {"viewport",
            {{"mode", viewportModeToString(settings.viewport.mode)},
             {"customBase",
              customViewportBaseToString(settings.viewport.customBase)},
             {"scaleX", settings.viewport.scaleX},
             {"scaleY", settings.viewport.scaleY},
             {"translateX", settings.viewport.translateX},
             {"translateY", settings.viewport.translateY}}}}}});
  }
  json selectedGameplayEntries = json::object();
  for (const auto &[skinType, entry] : skinSettings.selectedGameplayEntries) {
    selectedGameplayEntries[std::to_string(skinType)] = skinEntryIdToJson(entry);
  }
  return {{"safetyLevel", static_cast<int>(skinSettings.safetyLevel)},
          {"selectedGameplayEntries", std::move(selectedGameplayEntries)},
          {"entries", std::move(entries)}};
}

std::optional<skin::SkinEntryId>
readSkinEntryId(const json &encoded, std::string_view key,
                std::vector<std::string> &diagnostics) {
  if (!encoded.is_object()) {
    invalidValue(key, "expected package/path object", diagnostics);
    return std::nullopt;
  }
  const auto packageValue = encoded.find("package");
  const auto pathValue = encoded.find("path");
  if (packageValue == encoded.end() || !packageValue->is_string() ||
      pathValue == encoded.end() || !pathValue->is_string()) {
    invalidValue(key, "expected package and path strings", diagnostics);
    return std::nullopt;
  }
  const auto &packageName = packageValue->get_ref<const std::string &>();
  const auto &relativePath = pathValue->get_ref<const std::string &>();
  if (packageName.size() > skin::SkinPackagePolicy::maxPackageNameBytes ||
      relativePath.size() > skin::SkinPackagePolicy::maxPathBytes) {
    invalidValue(key, "package or path exceeds its byte limit", diagnostics);
    return std::nullopt;
  }
  const auto package = skin::normalizePackageId(packageName);
  if (!package.package) {
    invalidValue(key, "invalid package identity", diagnostics);
    return std::nullopt;
  }
  const auto entry = skin::normalizeEntryPath(*package.package, relativePath);
  if (!entry.entry) {
    invalidValue(key, "invalid entry identity", diagnostics);
    return std::nullopt;
  }
  return *entry.entry;
}

void readViewport(const json &encoded, skin::ViewportSettings &viewport,
                  std::vector<std::string> &diagnostics) {
  if (!encoded.is_object()) {
    invalidValue("skin.entries.settings.viewport", "expected object",
                 diagnostics);
    viewport.mode = static_cast<skin::ViewportMode>(255);
    return;
  }
  std::string mode = "fit";
  if (readValue(encoded, "mode", mode, diagnostics)) {
    if (mode == "fit") {
      viewport.mode = skin::ViewportMode::Fit;
    } else if (mode == "stretch") {
      viewport.mode = skin::ViewportMode::Stretch;
    } else if (mode == "custom") {
      viewport.mode = skin::ViewportMode::Custom;
    } else {
      viewport.mode = static_cast<skin::ViewportMode>(255);
      invalidValue("skin.entries.settings.viewport.mode",
                   "expected fit, stretch, or custom", diagnostics);
    }
  }
  std::string base = "fit";
  if (readValue(encoded, "customBase", base, diagnostics)) {
    if (base == "fit") {
      viewport.customBase = skin::CustomViewportBase::Fit;
    } else if (base == "stretch") {
      viewport.customBase = skin::CustomViewportBase::Stretch;
    } else {
      viewport.customBase = static_cast<skin::CustomViewportBase>(255);
      invalidValue("skin.entries.settings.viewport.customBase",
                   "expected fit or stretch", diagnostics);
    }
  }
  readValue(encoded, "scaleX", viewport.scaleX, diagnostics);
  readValue(encoded, "scaleY", viewport.scaleY, diagnostics);
  readValue(encoded, "translateX", viewport.translateX, diagnostics);
  readValue(encoded, "translateY", viewport.translateY, diagnostics);
}

void readSkinProfileSettings(const json &document,
                             skin::SkinProfileSettings &destination,
                             std::vector<std::string> &diagnostics) {
  const auto found = document.find("skin");
  if (found == document.end()) {
    return;
  }
  if (!found->is_object()) {
    invalidValue("skin", "expected object", diagnostics);
    return;
  }
  readValue(*found, "gameplayCompatibilityEnabled",
            destination.gameplayCompatibilityEnabled, diagnostics);
  readEnum(*found, "safetyLevel", destination.safetyLevel, diagnostics);
  if (const auto selected = found->find("selected7KeyEntry");
      selected != found->end() && !selected->is_null()) {
    destination.selected7KeyEntry =
        readSkinEntryId(*selected, "skin.selected7KeyEntry", diagnostics);
  }
  if (const auto selectedGameplayEntries =
          found->find("selectedGameplayEntries");
      selectedGameplayEntries != found->end()) {
    if (!selectedGameplayEntries->is_object()) {
      invalidValue("skin.selectedGameplayEntries", "expected object",
                   diagnostics);
    } else {
      for (const auto &[rawSkinType, encodedEntry] :
           selectedGameplayEntries->items()) {
        int skinType = -1;
        const auto parsed = std::from_chars(
            rawSkinType.data(), rawSkinType.data() + rawSkinType.size(),
            skinType);
        if (parsed.ec != std::errc{} || parsed.ptr != rawSkinType.data() + rawSkinType.size() ||
            !skin::gameplaySkinTraitForSkinType(skinType)) {
          invalidValue("skin.selectedGameplayEntries",
                       "key is not a supported gameplay skin type",
                       diagnostics);
          continue;
        }
        const auto entry = readSkinEntryId(
            encodedEntry, "skin.selectedGameplayEntries." + rawSkinType,
            diagnostics);
        if (entry) {
          destination.selectedGameplayEntries.insert_or_assign(skinType,
                                                                *entry);
        }
      }
    }
  }
  const auto entries = found->find("entries");
  if (entries == found->end()) {
    return;
  }
  if (!entries->is_array()) {
    invalidValue("skin.entries", "expected array", diagnostics);
    return;
  }
  struct RetainedEntry {
    const json *settings = nullptr;
  };
  std::map<skin::SkinEntryId, RetainedEntry> retainedEntries;
  for (const auto &record : *entries) {
    if (!record.is_object() || !record.contains("entry") ||
        !record.contains("settings") || !record["settings"].is_object()) {
      invalidValue("skin.entries", "expected entry/settings record",
                   diagnostics);
      continue;
    }
    auto entry =
        readSkinEntryId(record["entry"], "skin.entries.entry", diagnostics);
    if (!entry) {
      continue;
    }
    const auto collision =
        std::ranges::find_if(retainedEntries, [&](const auto &candidate) {
          return candidate.first.collisionKey == entry->collisionKey;
        });
    if (collision != retainedEntries.end()) {
      if (!(*entry < collision->first)) {
        continue;
      }
      retainedEntries.erase(collision);
    }
    retainedEntries.emplace(*entry,
                            RetainedEntry{.settings = &record["settings"]});
  }

  struct RetainedMapValue {
    std::string rawKey;
    const json *value = nullptr;
  };
  const auto retainUniqueMapValues = [&](const json &map,
                                         std::string_view name) {
    std::map<std::string, RetainedMapValue, std::less<>> retained;
    for (const auto &[rawKey, value] : map.items()) {
      auto key = skin::normalizeSkinConfigurationKey(rawKey);
      if (!key) {
        invalidValue("skin.entries.settings." + std::string(name),
                     "key is invalid", diagnostics);
        continue;
      }
      const auto existing = retained.find(*key);
      if (existing != retained.end()) {
        if (rawKey < existing->second.rawKey) {
          existing->second = {.rawKey = rawKey, .value = &value};
        }
        continue;
      }
      retained.emplace(std::move(*key),
                       RetainedMapValue{.rawKey = rawKey, .value = &value});
    }
    return retained;
  };

  for (const auto &[entry, retainedEntry] : retainedEntries) {
    skin::EntryProfileSettings settings;
    const auto &encoded = *retainedEntry.settings;
    const auto readMap = [&](std::string_view name, auto &target) {
      const auto map = encoded.find(std::string(name));
      if (map == encoded.end()) {
        return;
      }
      if (!map->is_object()) {
        invalidValue("skin.entries.settings." + std::string(name),
                     "expected object", diagnostics);
        return;
      }
      auto retained = retainUniqueMapValues(*map, name);
      for (const auto &[key, candidate] : retained) {
        try {
          using Mapped = typename std::decay_t<decltype(target)>::mapped_type;
          const auto &value = *candidate.value;
          if constexpr (std::is_same_v<Mapped, std::string>) {
            if (!value.is_string()) {
              invalidValue("skin.entries.settings." + std::string(name),
                           "value is not a string", diagnostics);
              continue;
            }
          }
          target.emplace(key, value.template get<Mapped>());
        } catch (const std::exception &) {
          invalidValue("skin.entries.settings." + std::string(name),
                       "invalid mapped value", diagnostics);
        }
      }
    };
    readMap("options", settings.options);
    readMap("filePaths", settings.filePaths);
    if (const auto offsets = encoded.find("offsets");
        offsets != encoded.end()) {
      if (!offsets->is_object()) {
        invalidValue("skin.entries.settings.offsets", "expected object",
                     diagnostics);
      } else {
        auto retained = retainUniqueMapValues(*offsets, "offsets");
        for (const auto &[name, candidate] : retained) {
          const auto &value = *candidate.value;
          if (!value.is_object()) {
            invalidValue("skin.entries.settings.offsets." + name,
                         "expected object", diagnostics);
            continue;
          }
          skin::ConfigOffset offset;
          readValue(value, "x", offset.x, diagnostics);
          readValue(value, "y", offset.y, diagnostics);
          readValue(value, "w", offset.w, diagnostics);
          readValue(value, "h", offset.h, diagnostics);
          readValue(value, "r", offset.r, diagnostics);
          readValue(value, "a", offset.a, diagnostics);
          settings.offsets[name] = offset;
        }
      }
    }
    if (const auto viewport = encoded.find("viewport");
        viewport != encoded.end()) {
      readViewport(*viewport, settings.viewport, diagnostics);
    }
    destination.entries.try_emplace(entry, std::move(settings));
  }
}

json settingsToJson(const AppSettings &settings) {
  json document = {
      {"schemaVersion", AppSettingsStore::kCurrentSchemaVersion},
      {"audioOffsetMs", settings.audioOffsetMs},
      {"visualOffsetMs", settings.visualOffsetMs},
      {"visibleTimeDurationMilliseconds",
       settings.visibleTimeDurationMilliseconds},
      {"gameplayHispeed", settings.gameplayHispeed},
      {"hispeedMargin", settings.hispeedMargin},
      {"visibleTimeUseMilliseconds", settings.visibleTimeUseMilliseconds},
      {"hispeedFixMode", static_cast<int>(settings.hispeedFixMode)},
      {"inputKeysoundEnabled", settings.inputKeysoundEnabled},
      {"prepMetronomeEnabled", settings.prepMetronomeEnabled},
      {"startLaneIndicatorsEnabled", settings.startLaneIndicatorsEnabled},
      {"showInvisibleNotes", settings.showInvisibleNotes},
      {"showPastNotes", settings.showPastNotes},
      {"markProcessedNotes", settings.markProcessedNotes},
      {"customJudge", settings.customJudge},
      {"showJudgeArea", settings.showJudgeArea},
      {"notesDisplayTimingAutoAdjust", settings.notesDisplayTimingAutoAdjust},
      {"notesDisplayTimingMilliseconds",
       settings.notesDisplayTimingMilliseconds},
      {"autoSaveReplay", settings.autoSaveReplay},
      {"guideSoundEffects", settings.guideSoundEffects},
      {"extraNoteDepth", settings.extraNoteDepth},
      {"mineMode", settings.mineMode},
      {"scrollMode", settings.scrollMode},
      {"longNoteModifierMode", settings.longNoteModifierMode},
      {"sevenToNinePattern", settings.sevenToNinePattern},
      {"sevenToNineType", settings.sevenToNineType},
      {"constantScroll", settings.constantScroll},
      {"constantFadeInMilliseconds", settings.constantFadeInMilliseconds},
      {"touchVisualizationEnabled", settings.touchVisualizationEnabled},
      {"archiveChartPreviewEnabled", settings.archiveChartPreviewEnabled},
      {"findBmsSkipUnarchivingForNonSolidArchives",
       settings.findBmsSkipUnarchivingForNonSolidArchives},
      {"bgaEnabled", settings.bgaEnabled},
      {"bgaBrightnessPercent", settings.bgaBrightnessPercent},
      {"bgaBlurStrength", settings.bgaBlurStrength},
      {"bgaDisplayMode", static_cast<int>(settings.bgaDisplayMode)},
      {"laneAngleDegrees", settings.laneAngleDegrees},
      {"laneLength", settings.laneLength},
      {"laneBeamLengthPercent", settings.laneBeamLengthPercent},
      {"noteStartPositionPercent", settings.noteStartPositionPercent},
      {"laneCoverEnabled", settings.laneCoverEnabled},
      {"hispeedAutoAdjust", settings.hispeedAutoAdjust},
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
      {"judgementIndicatorRangeMilliseconds",
       settings.judgementIndicatorRangeMilliseconds},
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
      {"selectedGameplayRuleset", settings.selectedGameplayRuleset},
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
      {"skin", skinProfileSettingsToJson(settings.skin)},
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
  json providers = json::object();
  for (const auto &[providerId, provider] : settings.irProviders) {
    providers[providerId] = {
        {"enabled", provider.enabled},
        {"autoSubmit", provider.autoSubmit},
        {"serverOrigin", provider.serverOrigin},
    };
  }
  document["ir"] = {{"providers", std::move(providers)}};
  return document;
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
  if (document.contains("visibleTimeDurationMilliseconds")) {
    readValue(document, "visibleTimeDurationMilliseconds",
              settings.visibleTimeDurationMilliseconds, diagnostics);
  } else {
    int legacyGreenNumber = settings.visibleTimeGreenNumber();
    if (readValue(document, "visibleTimeGreenNumber", legacyGreenNumber,
                  diagnostics)) {
      settings.setVisibleTimeGreenNumber(legacyGreenNumber);
    }
  }
  readValue(document, "gameplayHispeed", settings.gameplayHispeed,
            diagnostics);
  readValue(document, "hispeedMargin", settings.hispeedMargin, diagnostics);
  readValue(document, "visibleTimeUseMilliseconds",
            settings.visibleTimeUseMilliseconds, diagnostics);
  readEnum(document, "hispeedFixMode", settings.hispeedFixMode, diagnostics);
  readValue(document, "inputKeysoundEnabled", settings.inputKeysoundEnabled,
            diagnostics);
  readValue(document, "prepMetronomeEnabled", settings.prepMetronomeEnabled,
            diagnostics);
  readValue(document, "startLaneIndicatorsEnabled",
            settings.startLaneIndicatorsEnabled, diagnostics);
  readValue(document, "showInvisibleNotes", settings.showInvisibleNotes,
            diagnostics);
  readValue(document, "showPastNotes", settings.showPastNotes, diagnostics);
  readValue(document, "markProcessedNotes", settings.markProcessedNotes,
            diagnostics);
  readValue(document, "customJudge", settings.customJudge, diagnostics);
  readValue(document, "showJudgeArea", settings.showJudgeArea, diagnostics);
  readValue(document, "notesDisplayTimingAutoAdjust",
            settings.notesDisplayTimingAutoAdjust, diagnostics);
  readValue(document, "notesDisplayTimingMilliseconds",
            settings.notesDisplayTimingMilliseconds, diagnostics);
  readValue(document, "autoSaveReplay", settings.autoSaveReplay,
            diagnostics);
  readValue(document, "guideSoundEffects", settings.guideSoundEffects,
            diagnostics);
  readValue(document, "extraNoteDepth", settings.extraNoteDepth,
            diagnostics);
  readValue(document, "mineMode", settings.mineMode, diagnostics);
  readValue(document, "scrollMode", settings.scrollMode, diagnostics);
  readValue(document, "longNoteModifierMode", settings.longNoteModifierMode,
            diagnostics);
  readValue(document, "sevenToNinePattern", settings.sevenToNinePattern,
            diagnostics);
  readValue(document, "sevenToNineType", settings.sevenToNineType,
            diagnostics);
  readValue(document, "constantScroll", settings.constantScroll,
            diagnostics);
  readValue(document, "constantFadeInMilliseconds",
            settings.constantFadeInMilliseconds, diagnostics);
  readValue(document, "touchVisualizationEnabled",
            settings.touchVisualizationEnabled, diagnostics);
  readValue(document, "archiveChartPreviewEnabled",
            settings.archiveChartPreviewEnabled, diagnostics);
  readValue(document, "findBmsSkipUnarchivingForNonSolidArchives",
            settings.findBmsSkipUnarchivingForNonSolidArchives, diagnostics);
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
  readValue(document, "laneCoverEnabled", settings.laneCoverEnabled,
            diagnostics);
  readValue(document, "hispeedAutoAdjust", settings.hispeedAutoAdjust,
            diagnostics);
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
  readValue(document, "judgementIndicatorRangeMilliseconds",
            settings.judgementIndicatorRangeMilliseconds, diagnostics);
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
  std::string selectedGameplayRuleset = settings.selectedGameplayRuleset;
  if (readValue(document, "selectedGameplayRuleset", selectedGameplayRuleset,
                diagnostics)) {
    if (gameplayRulesetFromId(selectedGameplayRuleset).has_value()) {
      settings.selectedGameplayRuleset = std::move(selectedGameplayRuleset);
    } else {
      invalidValue("selectedGameplayRuleset", "expected lr2 or beatoraja",
                   diagnostics);
    }
  }
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
  readSkinProfileSettings(document, settings.skin, diagnostics);

  const auto irObject = document.find("ir");
  if (irObject != document.end()) {
    if (!irObject->is_object()) {
      invalidValue("ir", "expected object", diagnostics);
    } else {
      const auto providers = irObject->find("providers");
      if (providers == irObject->end() || !providers->is_object()) {
        invalidValue("ir.providers", "expected object", diagnostics);
      } else {
        for (const auto &[providerId, encoded] : providers->items()) {
          if (!encoded.is_object()) {
            invalidValue("ir.providers." + providerId, "expected object",
                         diagnostics);
            continue;
          }
          ir::IrProviderSettings provider;
          readValue(encoded, "enabled", provider.enabled, diagnostics);
          readValue(encoded, "autoSubmit", provider.autoSubmit, diagnostics);
          readValue(encoded, "serverOrigin", provider.serverOrigin,
                    diagnostics);
          const auto normalized =
              ir::normalizeServerOrigin(provider.serverOrigin);
          if (!normalized) {
            invalidValue("ir.providers." + providerId + ".serverOrigin",
                         "expected absolute HTTP or HTTPS origin", diagnostics);
            provider.serverOrigin = std::string(ir::kDefaultTachiServerOrigin);
          } else {
            provider.serverOrigin = *normalized;
          }
          settings.irProviders[providerId] = std::move(provider);
        }
      }
    }
  }

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
  const std::array<versioned_json::Migration, 7> migrations = {
      [](json &document, std::string &) {
        document["schemaVersion"] = 1;
        return true;
      },
      [](json &document, std::string &) {
        if (!document.contains("ir")) {
          document["ir"] = json::object();
        }
        if (document["ir"].is_object() &&
            !document["ir"].contains("providers")) {
          document["ir"]["providers"] = json::object();
        }
        if (document["ir"].is_object() &&
            document["ir"].value("providers", json{}).is_object() &&
            !document["ir"]["providers"].contains("tachi")) {
          document["ir"]["providers"]["tachi"] = {
              {"enabled", false},
              {"autoSubmit", false},
              {"serverOrigin", ir::kDefaultTachiServerOrigin},
          };
        }
        return true;
      },
      [](json &document, std::string &) {
        if (!document.contains("selectedGameplayRuleset")) {
          document["selectedGameplayRuleset"] = "lr2";
        }
        return true;
      },
      [](json &document, std::string &) {
        if (!document.contains("skin")) {
          document["skin"] = {
              {"gameplayCompatibilityEnabled", false},
              {"selected7KeyEntry", nullptr},
              {"entries", json::array()},
          };
        }
        return true;
      },
      [](json &document, std::string &) {
        if (!document.contains("visibleTimeDurationMilliseconds") &&
            document.contains("visibleTimeGreenNumber")) {
          const auto &legacy = document["visibleTimeGreenNumber"];
          if (legacy.is_number_integer()) {
            if (legacy.is_number_unsigned()) {
              const auto value = legacy.get<std::uint64_t>();
              if (value <= static_cast<std::uint64_t>(
                               std::numeric_limits<int>::max())) {
                document["visibleTimeDurationMilliseconds"] =
                    AppSettings::greenNumberToDurationMilliseconds(
                        static_cast<int>(value));
              }
            } else {
              const auto value = legacy.get<std::int64_t>();
              if (value >= static_cast<std::int64_t>(
                               std::numeric_limits<int>::lowest()) &&
                  value <= static_cast<std::int64_t>(
                               std::numeric_limits<int>::max())) {
                document["visibleTimeDurationMilliseconds"] =
                    AppSettings::greenNumberToDurationMilliseconds(
                        static_cast<int>(value));
              }
            }
          }
        }
        return true;
      },
      [](json &document, std::string &) {
        // Versions through schema 5 represented fixed Hi-Speed as a second
        // multiplier layered over duration and either chart/time-weighted BPM.
        // That state has no Beatoraja-equivalent persisted value. Migrate to
        // the upstream default fixed MAIN mode and discard the transient
        // multiplier rather than silently retaining a different note speed.
        if (!document.contains("hispeedFixMode")) {
          document["hispeedFixMode"] =
              static_cast<int>(AppSettings::HiSpeedFixMode::Main);
        }
        if (!document.contains("gameplayHispeed")) {
          document["gameplayHispeed"] = 1.0F;
        }
        if (!document.contains("hispeedMargin")) {
          document["hispeedMargin"] = AppSettings::kDefaultHispeedMargin;
        }
        return true;
      },
      [](json &document, std::string &) {
        if (!document.contains("skin")) {
          document["skin"] = json::object();
        }
        if (document["skin"].is_object() &&
            !document["skin"].contains("safetyLevel")) {
          document["skin"]["safetyLevel"] =
              static_cast<int>(skin::SkinSafetyLevel::Standard);
        }
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
