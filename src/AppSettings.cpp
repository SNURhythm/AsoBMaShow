#include "AppSettings.h"
#include "LongNoteModeUtils.h"
#include "replay/ReplayOption.h"
#include "scene/play/GameplayRuleset.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace {
std::string trim(std::string value) {
  auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](unsigned char ch) { return !isSpace(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](unsigned char ch) { return !isSpace(ch); })
                  .base(),
              value.end());
  return value;
}

bool parseBool(const std::string &value, bool &out) {
  if (value == "1" || value == "true" || value == "on" || value == "yes") {
    out = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "off" || value == "no") {
    out = false;
    return true;
  }
  return false;
}

float sanitizeFloat(float value, float fallback, float minValue,
                    float maxValue) {
  if (!std::isfinite(value)) {
    return fallback;
  }
  return std::clamp(value, minValue, maxValue);
}

AppSettings::BgaDisplayMode
parseBgaDisplayMode(const std::string &value,
                    AppSettings::BgaDisplayMode fallback) {
  if (value == "fit" || value == "0") {
    return AppSettings::BgaDisplayMode::Fit;
  }
  if (value == "fill" || value == "1") {
    return AppSettings::BgaDisplayMode::Fill;
  }
  if (value == "stretch" || value == "2") {
    return AppSettings::BgaDisplayMode::Stretch;
  }
  return fallback;
}

std::string normalizeSettingToken(std::string value) {
  value = trim(value);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   if (ch == '_' || ch == ' ') {
                     return '-';
                   }
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string normalizeUpperOptionToken(std::string value) {
  value = trim(value);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   if (ch == '_' || ch == ' ') {
                     return '-';
                   }
                   return static_cast<char>(std::toupper(ch));
                 });
  return value;
}

const char *bgaDisplayModeToString(AppSettings::BgaDisplayMode mode) {
  switch (mode) {
  case AppSettings::BgaDisplayMode::Fit:
    return "fit";
  case AppSettings::BgaDisplayMode::Fill:
    return "fill";
  case AppSettings::BgaDisplayMode::Stretch:
    return "stretch";
  }
  return "fit";
}

AppSettings::NotePriorityMode
parseNotePriorityMode(const std::string &value,
                      AppSettings::NotePriorityMode fallback) {
  const std::string normalized = normalizeSettingToken(value);
  if (normalized == "lowest") {
    return AppSettings::NotePriorityMode::Lowest;
  }
  if (normalized == "combo") {
    return AppSettings::NotePriorityMode::Combo;
  }
  if (normalized == "duration") {
    return AppSettings::NotePriorityMode::Duration;
  }
  if (normalized == "score") {
    return AppSettings::NotePriorityMode::Score;
  }
  return fallback;
}

const char *
notePriorityModeToString(AppSettings::NotePriorityMode notePriorityMode) {
  switch (notePriorityMode) {
  case AppSettings::NotePriorityMode::Lowest:
    return "lowest";
  case AppSettings::NotePriorityMode::Combo:
    return "combo";
  case AppSettings::NotePriorityMode::Duration:
    return "duration";
  case AppSettings::NotePriorityMode::Score:
    return "score";
  }
  return "lowest";
}

AppSettings::JudgementIndicatorRenderMode parseJudgementIndicatorRenderMode(
    const std::string &value,
    AppSettings::JudgementIndicatorRenderMode fallback) {
  const std::string normalized = normalizeSettingToken(value);
  if (normalized == "3d" || normalized == "world" || normalized == "world-3d") {
    return AppSettings::JudgementIndicatorRenderMode::World3D;
  }
  if (normalized == "hud" || normalized == "2d" || normalized == "hud-2d") {
    return AppSettings::JudgementIndicatorRenderMode::Hud2D;
  }
  return fallback;
}

const char *judgementIndicatorRenderModeToString(
    AppSettings::JudgementIndicatorRenderMode mode) {
  switch (mode) {
  case AppSettings::JudgementIndicatorRenderMode::World3D:
    return "3d";
  case AppSettings::JudgementIndicatorRenderMode::Hud2D:
    return "hud";
  }
  return "3d";
}

AppSettings::JudgementCounterPosition
parseJudgementCounterPosition(const std::string &value,
                              AppSettings::JudgementCounterPosition fallback) {
  const std::string normalized = normalizeSettingToken(value);
  if (normalized == "top" || normalized == "0") {
    return AppSettings::JudgementCounterPosition::Top;
  }
  if (normalized == "left" || normalized == "1") {
    return AppSettings::JudgementCounterPosition::Left;
  }
  if (normalized == "right" || normalized == "2") {
    return AppSettings::JudgementCounterPosition::Right;
  }
  return fallback;
}

const char *judgementCounterPositionToString(
    AppSettings::JudgementCounterPosition position) {
  switch (position) {
  case AppSettings::JudgementCounterPosition::Top:
    return "top";
  case AppSettings::JudgementCounterPosition::Left:
    return "left";
  case AppSettings::JudgementCounterPosition::Right:
    return "right";
  }
  return "right";
}

AppSettings::JudgementTimingDisplayCriteria parseJudgementTimingDisplayCriteria(
    const std::string &value,
    AppSettings::JudgementTimingDisplayCriteria fallback) {
  const std::string normalized = normalizeSettingToken(value);
  if (normalized == "great-or-below" || normalized == "great" ||
      normalized == "great-below" || normalized == "0") {
    return AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow;
  }
  if (normalized == "pgreat-or-below" || normalized == "pgreat" ||
      normalized == "pgreat-below" || normalized == "perfect-great" ||
      normalized == "1") {
    return AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow;
  }
  if (normalized == "good-or-below" || normalized == "good" ||
      normalized == "good-below" || normalized == "2") {
    return AppSettings::JudgementTimingDisplayCriteria::GoodOrBelow;
  }
  if (normalized == "bad-or-below" || normalized == "bad" ||
      normalized == "bad-below" || normalized == "3") {
    return AppSettings::JudgementTimingDisplayCriteria::BadOrBelow;
  }
  if (normalized == "off" || normalized == "none" || normalized == "disabled" ||
      normalized == "4") {
    return AppSettings::JudgementTimingDisplayCriteria::Off;
  }
  return fallback;
}

const char *judgementTimingDisplayCriteriaToString(
    AppSettings::JudgementTimingDisplayCriteria criteria) {
  switch (criteria) {
  case AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow:
    return "great_or_below";
  case AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow:
    return "pgreat_or_below";
  case AppSettings::JudgementTimingDisplayCriteria::GoodOrBelow:
    return "good_or_below";
  case AppSettings::JudgementTimingDisplayCriteria::BadOrBelow:
    return "bad_or_below";
  case AppSettings::JudgementTimingDisplayCriteria::Off:
    return "off";
  }
  return "great_or_below";
}

AppSettings::GaugeBarPosition
parseGaugeBarPosition(const std::string &value,
                      AppSettings::GaugeBarPosition fallback) {
  const std::string normalized = normalizeSettingToken(value);
  if (normalized == "world" || normalized == "world-space" ||
      normalized == "3d" || normalized == "0") {
    return AppSettings::GaugeBarPosition::World;
  }
  if (normalized == "left" || normalized == "1") {
    return AppSettings::GaugeBarPosition::Left;
  }
  if (normalized == "right" || normalized == "2") {
    return AppSettings::GaugeBarPosition::Right;
  }
  return fallback;
}

const char *gaugeBarPositionToString(AppSettings::GaugeBarPosition position) {
  switch (position) {
  case AppSettings::GaugeBarPosition::World:
    return "world";
  case AppSettings::GaugeBarPosition::Left:
    return "left";
  case AppSettings::GaugeBarPosition::Right:
    return "right";
  }
  return "world";
}

AppSettings::VisibleTimeBpmStrategy
parseVisibleTimeBpmStrategy(const std::string &value,
                            AppSettings::VisibleTimeBpmStrategy fallback) {
  const std::string normalized = normalizeSettingToken(value);
  if (normalized == "chart" || normalized == "chart-bpm" ||
      normalized == "main" || normalized == "metadata") {
    return AppSettings::VisibleTimeBpmStrategy::Chart;
  }
  if (normalized == "most-prevalent" || normalized == "prevalent" ||
      normalized == "duration") {
    return AppSettings::VisibleTimeBpmStrategy::MostPrevalent;
  }
  return fallback;
}

const char *
visibleTimeBpmStrategyToString(AppSettings::VisibleTimeBpmStrategy strategy) {
  switch (strategy) {
  case AppSettings::VisibleTimeBpmStrategy::Chart:
    return "chart";
  case AppSettings::VisibleTimeBpmStrategy::MostPrevalent:
    return "most_prevalent";
  }
  return "chart";
}

AppSettings::UiThemeMode parseUiThemeMode(const std::string &value,
                                          AppSettings::UiThemeMode fallback) {
  const std::string normalized = normalizeSettingToken(value);
  if (normalized == "dark" || normalized == "0") {
    return AppSettings::UiThemeMode::Dark;
  }
  if (normalized == "light" || normalized == "1") {
    return AppSettings::UiThemeMode::Light;
  }
  return fallback;
}

const char *uiThemeModeToString(AppSettings::UiThemeMode mode) {
  switch (mode) {
  case AppSettings::UiThemeMode::Dark:
    return "dark";
  case AppSettings::UiThemeMode::Light:
    return "light";
  }
  return "dark";
}

std::string parseGaugeTypeId(const std::string &value,
                             const std::string &fallback) {
  if (value == "assisted_easy") {
    return "assisted_easy";
  }
  if (value == "easy") {
    return "easy";
  }
  if (value == "normal") {
    return "normal";
  }
  if (value == "hard") {
    return "hard";
  }
  if (value == "exhard") {
    return "exhard";
  }
  if (value == "hazard") {
    return "hazard";
  }
  if (value == "gas") {
    return "gas";
  }
  if (value == "gas_continue" || value == "gas_survival_to_groove" ||
      value == "gas_best_clear" || value == "gas_select_to_under") {
    return value;
  }
  return fallback;
}

std::string parseGaugeAutoShiftModeId(const std::string &value,
                                      const std::string &fallback) {
  if (value == "none" || value == "continue" || value == "survival_to_groove" ||
      value == "best_clear" || value == "select_to_under") {
    return value;
  }
  return fallback;
}

std::string normalizePlayOptionId(std::string value) {
  value = normalizeUpperOptionToken(std::move(value));
  if (value == "OFF") {
    return AppSettings::kDefaultPlayOption;
  }
  return value;
}

std::string parsePlayOptionId(const std::string &value,
                              const std::string &fallback) {
  const std::string normalized = normalizePlayOptionId(value);
  if (replay::beatorajaReplayOptionIndex(normalized)) {
    return normalized;
  }
  return fallback;
}

std::string normalizeAssistOptionId(std::string value) {
  value = normalizeUpperOptionToken(std::move(value));
  if (value == "DRAG" || value == "DRAG-MODE") {
    return "DRAG";
  }
  return AppSettings::kDefaultAssistOption;
}

std::string parseAssistOptionId(const std::string &value,
                                const std::string &fallback) {
  const std::string normalized = normalizeAssistOptionId(value);
  if (normalized == "OFF" || normalized == "DRAG") {
    return normalized;
  }
  return fallback;
}

std::string parsePacemakerTargetId(const std::string &value,
                                   const std::string &fallback) {
  const std::string normalized = normalizeUpperOptionToken(value);
  if (normalized == "OFF" || normalized == "NONE" || normalized == "DISABLED") {
    return "OFF";
  }
  if (normalized == "BEST" || normalized == "PB" || normalized == "HIGHSCORE" ||
      normalized == "HIGH-SCORE" || normalized == "PERSONAL-BEST") {
    return "BEST";
  }
  if (normalized == "A" || normalized == "AA" || normalized == "AAA" ||
      normalized == "MAX" || normalized == "PERFECT") {
    return normalized == "PERFECT" ? "MAX" : normalized;
  }
  if (normalized == "MAX-" || normalized == "MAX--" ||
      normalized == "MAX-MINUS" || normalized == "MAXMINUS" ||
      normalized == "RATE-MAX-" || normalized == "RATE-MAX--" ||
      normalized == "RATE-MAX-MINUS" || normalized == "RANK-MAX-" ||
      normalized == "RANK-MAX--" || normalized == "RANK-MAX-MINUS") {
    return "MAX-";
  }
  return fallback;
}

float sanitizePlayAreaWidth(float width) {
  return sanitizeFloat(width, AppSettings::kDefaultPlayAreaWidth,
                       AppSettings::kMinPlayAreaWidth,
                       AppSettings::kMaxPlayAreaWidth);
}
} // namespace

void AppSettings::sanitize() {
  skin.sanitize();
  irProviders.try_emplace(std::string(ir::kTachiProviderId),
                          ir::IrProviderSettings{});
  for (auto &[providerId, settings] : irProviders) {
    (void)providerId;
    ir::sanitizeProviderSettings(settings);
  }
  audioVideo.sanitize();
  audioOffsetMs =
      std::clamp(audioOffsetMs, kMinAudioOffsetMs, kMaxAudioOffsetMs);
  visualOffsetMs =
      std::clamp(visualOffsetMs, kMinVisualOffsetMs, kMaxVisualOffsetMs);
  visibleTimeGreenNumber =
      std::clamp(visibleTimeGreenNumber, kMinVisibleTimeGreenNumber,
                 kMaxVisibleTimeGreenNumber);
  gameplayHispeedMultiplier =
      sanitizeFloat(gameplayHispeedMultiplier, 1.0F,
                    kMinGameplayHispeedMultiplier,
                    kMaxGameplayHispeedMultiplier);
  bgaBrightnessPercent = std::clamp(
      bgaBrightnessPercent, kMinBgaBrightnessPercent, kMaxBgaBrightnessPercent);
  bgaBlurStrength = sanitizeFloat(bgaBlurStrength, kDefaultBgaBlurStrength,
                                  kMinBgaBlurStrength, kMaxBgaBlurStrength);
  switch (bgaDisplayMode) {
  case BgaDisplayMode::Fit:
  case BgaDisplayMode::Fill:
  case BgaDisplayMode::Stretch:
    break;
  default:
    bgaDisplayMode = BgaDisplayMode::Fit;
    break;
  }
  laneAngleDegrees = sanitizeFloat(laneAngleDegrees, kDefaultLaneAngleDegrees,
                                   kMinLaneAngleDegrees, kMaxLaneAngleDegrees);
  laneLength = sanitizeFloat(laneLength, kDefaultLaneLength, kMinLaneLength,
                             kMaxLaneLength);
  laneBeamLengthPercent =
      std::clamp(laneBeamLengthPercent, kMinLaneBeamLengthPercent,
                 kMaxLaneBeamLengthPercent);
  noteStartPositionPercent =
      std::clamp(noteStartPositionPercent, kMinNoteStartPositionPercent,
                 kMaxNoteStartPositionPercent);
  playAreaWidth4K = sanitizePlayAreaWidth(playAreaWidth4K);
  playAreaWidth5K = sanitizePlayAreaWidth(playAreaWidth5K);
  playAreaWidth6K = sanitizePlayAreaWidth(playAreaWidth6K);
  playAreaWidth7K = sanitizePlayAreaWidth(playAreaWidth7K);
  playAreaWidth8K = sanitizePlayAreaWidth(playAreaWidth8K);
  playAreaWidth10K = sanitizePlayAreaWidth(playAreaWidth10K);
  playAreaWidth14K = sanitizePlayAreaWidth(playAreaWidth14K);
  judgementIndicatorY =
      sanitizeFloat(judgementIndicatorY, kDefaultJudgementIndicatorY,
                    kMinJudgementIndicatorY, kMaxJudgementIndicatorY);
  judgementIndicatorWidthScale = sanitizeFloat(
      judgementIndicatorWidthScale, kDefaultJudgementIndicatorWidthScale,
      kMinJudgementIndicatorWidthScale, kMaxJudgementIndicatorWidthScale);
  judgementIndicatorRangeMilliseconds =
      judgement_indicator::sanitizeStoredRangeMilliseconds(
          judgementIndicatorRangeMilliseconds);
  judgementTextY = sanitizeFloat(judgementTextY, kDefaultJudgementTextY,
                                 kMinJudgementTextY, kMaxJudgementTextY);
  switch (notePriorityMode) {
  case NotePriorityMode::Lowest:
  case NotePriorityMode::Combo:
  case NotePriorityMode::Duration:
  case NotePriorityMode::Score:
    break;
  default:
    notePriorityMode = NotePriorityMode::Lowest;
    break;
  }
  switch (judgementIndicatorRenderMode) {
  case JudgementIndicatorRenderMode::World3D:
  case JudgementIndicatorRenderMode::Hud2D:
    break;
  default:
    judgementIndicatorRenderMode = JudgementIndicatorRenderMode::World3D;
    break;
  }
  switch (judgementCounterPosition) {
  case JudgementCounterPosition::Top:
  case JudgementCounterPosition::Left:
  case JudgementCounterPosition::Right:
    break;
  default:
    judgementCounterPosition = JudgementCounterPosition::Right;
    break;
  }
  auto sanitizeTimingDisplayCriteria =
      [](JudgementTimingDisplayCriteria &criteria) {
        switch (criteria) {
        case JudgementTimingDisplayCriteria::GreatOrBelow:
        case JudgementTimingDisplayCriteria::PGreatOrBelow:
        case JudgementTimingDisplayCriteria::GoodOrBelow:
        case JudgementTimingDisplayCriteria::BadOrBelow:
        case JudgementTimingDisplayCriteria::Off:
          break;
        default:
          criteria = JudgementTimingDisplayCriteria::GreatOrBelow;
          break;
        }
      };
  sanitizeTimingDisplayCriteria(judgementTimingFastSlowCriteria);
  sanitizeTimingDisplayCriteria(judgementTimingMillisecondsCriteria);
  switch (gaugeBarPosition) {
  case GaugeBarPosition::World:
  case GaugeBarPosition::Left:
  case GaugeBarPosition::Right:
    break;
  default:
    gaugeBarPosition = GaugeBarPosition::World;
    break;
  }
  switch (visibleTimeBpmStrategy) {
  case VisibleTimeBpmStrategy::Chart:
  case VisibleTimeBpmStrategy::MostPrevalent:
    break;
  default:
    visibleTimeBpmStrategy = VisibleTimeBpmStrategy::Chart;
    break;
  }
  switch (uiThemeMode) {
  case UiThemeMode::Dark:
  case UiThemeMode::Light:
    break;
  default:
    uiThemeMode = UiThemeMode::Dark;
    break;
  }
  selectedGameplayRuleset = std::string(gameplayRulesetId(
      gameplayRulesetSelectionOrDefault(selectedGameplayRuleset)));
  selectedGaugeType = parseGaugeTypeId(selectedGaugeType, kDefaultGaugeType);
  selectedGaugeAutoShiftMode =
      parseGaugeAutoShiftModeId(selectedGaugeAutoShiftMode, "none");
  selectedGaugeAutoShiftLowerBound =
      parseGaugeTypeId(selectedGaugeAutoShiftLowerBound, "assisted_easy");
  if (selectedGaugeAutoShiftLowerBound.rfind("gas", 0) == 0) {
    selectedGaugeAutoShiftLowerBound = "assisted_easy";
  }
  selectedPlayOption =
      parsePlayOptionId(selectedPlayOption, kDefaultPlayOption);
  selectedLnMode = long_note_mode::parseId(selectedLnMode, kDefaultLnMode);
  selectedAssistOption =
      parseAssistOptionId(selectedAssistOption, kDefaultAssistOption);
  selectedPacemakerTarget =
      parsePacemakerTargetId(selectedPacemakerTarget, kDefaultPacemakerTarget);
  musicPlayerPlaybackRatePercent =
      ((std::clamp(musicPlayerPlaybackRatePercent, 50, 200) + 2) / 5) * 5;
  switch (musicPlayerPlaybackMode) {
  case audio::PlaybackMode::PitchShift:
  case audio::PlaybackMode::TimeStretch:
    break;
  default:
    musicPlayerPlaybackMode = audio::PlaybackMode::PitchShift;
    break;
  }
  selectedPlaybackRatePercent =
      ((std::clamp(selectedPlaybackRatePercent, 50, 200) + 2) / 5) * 5;
  switch (selectedPlaybackMode) {
  case audio::PlaybackMode::PitchShift:
    break;
  case audio::PlaybackMode::TimeStretch:
  default:
    selectedPlaybackMode = audio::PlaybackMode::PitchShift;
    break;
  }
}

float AppSettings::playAreaWidthForKeyMode(int keyMode) const {
  switch (keyMode) {
  case 4:
    return playAreaWidth4K;
  case 5:
    return playAreaWidth5K;
  case 6:
    return playAreaWidth6K;
  case 7:
    return playAreaWidth7K;
  case 8:
    return playAreaWidth8K;
  case 10:
    return playAreaWidth10K;
  case 14:
    return playAreaWidth14K;
  default:
    return kDefaultPlayAreaWidth;
  }
}

void AppSettings::setPlayAreaWidthForKeyMode(int keyMode, float width) {
  const float sanitized = sanitizePlayAreaWidth(width);
  switch (keyMode) {
  case 4:
    playAreaWidth4K = sanitized;
    break;
  case 5:
    playAreaWidth5K = sanitized;
    break;
  case 6:
    playAreaWidth6K = sanitized;
    break;
  case 7:
    playAreaWidth7K = sanitized;
    break;
  case 8:
    playAreaWidth8K = sanitized;
    break;
  case 10:
    playAreaWidth10K = sanitized;
    break;
  case 14:
    playAreaWidth14K = sanitized;
    break;
  default:
    break;
  }
}

bool AppSettings::loadLegacyCfg(const std::filesystem::path &path,
                                AppSettings &settings,
                                std::vector<std::string> *diagnostics) {
  std::ifstream file(path);
  if (!file.is_open()) {
    if (diagnostics != nullptr) {
      diagnostics->push_back("Unable to open legacy settings file");
    }
    return false;
  }
  return parseLegacyCfg(file, settings, diagnostics);
}

bool AppSettings::parseLegacyCfg(std::istream &file, AppSettings &settings,
                                 std::vector<std::string> *diagnostics) {
  std::string line;
  while (std::getline(file, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }

    const std::string key = trim(line.substr(0, separator));
    const std::string value = trim(line.substr(separator + 1));

    try {
      if (key == "audio_offset_ms") {
        settings.audioOffsetMs = std::stoi(value);
      } else if (key == "visual_offset_ms") {
        settings.visualOffsetMs = std::stoi(value);
      } else if (key == "visible_time_green_number") {
        settings.visibleTimeGreenNumber = std::stoi(value);
      } else if (key == "gameplay_hispeed_multiplier") {
        settings.gameplayHispeedMultiplier = std::stof(value);
      } else if (key == "visible_time_use_milliseconds") {
        bool parsed = settings.visibleTimeUseMilliseconds;
        if (parseBool(value, parsed)) {
          settings.visibleTimeUseMilliseconds = parsed;
        }
      } else if (key == "visible_time_bpm_strategy") {
        settings.visibleTimeBpmStrategy =
            parseVisibleTimeBpmStrategy(value, settings.visibleTimeBpmStrategy);
      } else if (key == "input_keysound_enabled") {
        bool parsed = settings.inputKeysoundEnabled;
        if (parseBool(value, parsed)) {
          settings.inputKeysoundEnabled = parsed;
        }
      } else if (key == "prep_metronome_enabled") {
        bool parsed = settings.prepMetronomeEnabled;
        if (parseBool(value, parsed)) {
          settings.prepMetronomeEnabled = parsed;
        }
      } else if (key == "start_lane_indicators_enabled") {
        bool parsed = settings.startLaneIndicatorsEnabled;
        if (parseBool(value, parsed)) {
          settings.startLaneIndicatorsEnabled = parsed;
        }
      } else if (key == "show_invisible_notes") {
        bool parsed = settings.showInvisibleNotes;
        if (parseBool(value, parsed)) {
          settings.showInvisibleNotes = parsed;
        }
      } else if (key == "touch_visualization_enabled") {
        bool parsed = settings.touchVisualizationEnabled;
        if (parseBool(value, parsed)) {
          settings.touchVisualizationEnabled = parsed;
        }
      } else if (key == "archive_chart_preview_enabled") {
        bool parsed = settings.archiveChartPreviewEnabled;
        if (parseBool(value, parsed)) {
          settings.archiveChartPreviewEnabled = parsed;
        }
      } else if (key == "bga_enabled") {
        bool parsed = settings.bgaEnabled;
        if (parseBool(value, parsed)) {
          settings.bgaEnabled = parsed;
        }
      } else if (key == "bga_brightness_percent") {
        settings.bgaBrightnessPercent = std::stoi(value);
      } else if (key == "bga_blur_strength") {
        settings.bgaBlurStrength = std::stof(value);
      } else if (key == "bga_display_mode") {
        settings.bgaDisplayMode =
            parseBgaDisplayMode(value, settings.bgaDisplayMode);
      } else if (key == "lane_angle_degrees") {
        settings.laneAngleDegrees = std::stof(value);
      } else if (key == "lane_length") {
        settings.laneLength = std::stof(value);
      } else if (key == "lane_beam_length_percent") {
        settings.laneBeamLengthPercent = std::stoi(value);
      } else if (key == "note_start_position_percent") {
        settings.noteStartPositionPercent = std::stoi(value);
      } else if (key == "lane_cover_enabled") {
        bool parsed = settings.laneCoverEnabled;
        if (parseBool(value, parsed)) {
          settings.laneCoverEnabled = parsed;
        }
      } else if (key == "floating_lane_cover_enabled") {
        bool parsed = settings.floatingLaneCoverEnabled;
        if (parseBool(value, parsed)) {
          settings.floatingLaneCoverEnabled = parsed;
        }
      } else if (key == "play_area_width_4k") {
        settings.playAreaWidth4K = std::stof(value);
      } else if (key == "play_area_width_5k") {
        settings.playAreaWidth5K = std::stof(value);
      } else if (key == "play_area_width_6k") {
        settings.playAreaWidth6K = std::stof(value);
      } else if (key == "play_area_width_7k") {
        settings.playAreaWidth7K = std::stof(value);
      } else if (key == "play_area_width_8k") {
        settings.playAreaWidth8K = std::stof(value);
      } else if (key == "play_area_width_10k") {
        settings.playAreaWidth10K = std::stof(value);
      } else if (key == "play_area_width_14k") {
        settings.playAreaWidth14K = std::stof(value);
      } else if (key == "note_priority_mode") {
        settings.notePriorityMode =
            parseNotePriorityMode(value, settings.notePriorityMode);
      } else if (key == "judgement_indicator_enabled") {
        bool parsed = settings.judgementIndicatorEnabled;
        if (parseBool(value, parsed)) {
          settings.judgementIndicatorEnabled = parsed;
        }
      } else if (key == "judgement_indicator_y") {
        settings.judgementIndicatorY = std::stof(value);
      } else if (key == "judgement_indicator_width_scale") {
        settings.judgementIndicatorWidthScale = std::stof(value);
      } else if (key == "judgement_indicator_render_mode") {
        settings.judgementIndicatorRenderMode =
            parseJudgementIndicatorRenderMode(
                value, settings.judgementIndicatorRenderMode);
      } else if (key == "judgement_text_y") {
        settings.judgementTextY = std::stof(value);
      } else if (key == "judgement_counter_enabled") {
        bool parsed = settings.judgementCounterEnabled;
        if (parseBool(value, parsed)) {
          settings.judgementCounterEnabled = parsed;
        }
      } else if (key == "judgement_counter_position") {
        settings.judgementCounterPosition = parseJudgementCounterPosition(
            value, settings.judgementCounterPosition);
      } else if (key == "judgement_timing_fast_slow_criteria") {
        settings.judgementTimingFastSlowCriteria =
            parseJudgementTimingDisplayCriteria(
                value, settings.judgementTimingFastSlowCriteria);
      } else if (key == "judgement_timing_milliseconds_criteria") {
        settings.judgementTimingMillisecondsCriteria =
            parseJudgementTimingDisplayCriteria(
                value, settings.judgementTimingMillisecondsCriteria);
      } else if (key == "gauge_bar_position") {
        settings.gaugeBarPosition =
            parseGaugeBarPosition(value, settings.gaugeBarPosition);
      } else if (key == "ui_theme_mode") {
        settings.uiThemeMode = parseUiThemeMode(value, settings.uiThemeMode);
      } else if (key == "system_playback_show_jacket") {
        bool parsed = settings.systemPlaybackShowJacket;
        if (parseBool(value, parsed)) {
          settings.systemPlaybackShowJacket = parsed;
        }
      } else if (key == "system_playback_show_title") {
        bool parsed = settings.systemPlaybackShowTitle;
        if (parseBool(value, parsed)) {
          settings.systemPlaybackShowTitle = parsed;
        }
      } else if (key == "system_playback_show_artist") {
        bool parsed = settings.systemPlaybackShowArtist;
        if (parseBool(value, parsed)) {
          settings.systemPlaybackShowArtist = parsed;
        }
      } else if (key == "selected_gameplay_ruleset") {
        settings.selectedGameplayRuleset = value;
      } else if (key == "selected_gauge_type") {
        settings.selectedGaugeType =
            parseGaugeTypeId(value, settings.selectedGaugeType);
      } else if (key == "selected_gauge_auto_shift_mode") {
        settings.selectedGaugeAutoShiftMode = parseGaugeAutoShiftModeId(
            value, settings.selectedGaugeAutoShiftMode);
      } else if (key == "selected_gauge_auto_shift_lower_bound") {
        settings.selectedGaugeAutoShiftLowerBound =
            parseGaugeTypeId(value, settings.selectedGaugeAutoShiftLowerBound);
      } else if (key == "selected_play_option") {
        settings.selectedPlayOption =
            parsePlayOptionId(value, settings.selectedPlayOption);
      } else if (key == "selected_ln_mode") {
        settings.selectedLnMode =
            long_note_mode::parseId(value, settings.selectedLnMode);
      } else if (key == "selected_assist_option") {
        settings.selectedAssistOption =
            parseAssistOptionId(value, settings.selectedAssistOption);
      } else if (key == "selected_pacemaker_target") {
        settings.selectedPacemakerTarget =
            parsePacemakerTargetId(value, settings.selectedPacemakerTarget);
      } else if (key == "default_difficulty_tables_seeded") {
        bool parsed = settings.defaultDifficultyTablesSeeded;
        if (parseBool(value, parsed)) {
          settings.defaultDifficultyTablesSeeded = parsed;
        }
      }
    } catch (const std::exception &e) {
      SDL_Log("Ignoring malformed settings line '%s': %s", line.c_str(),
              e.what());
      if (diagnostics != nullptr) {
        diagnostics->push_back("Ignoring malformed settings line '" + line +
                               "': " + e.what());
      }
    }
  }

  if (file.bad()) {
    if (diagnostics != nullptr) {
      diagnostics->push_back("I/O failure while reading legacy settings");
    }
    return false;
  }
  settings.sanitize();
  return true;
}
