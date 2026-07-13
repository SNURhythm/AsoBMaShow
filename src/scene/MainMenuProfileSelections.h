#pragma once

#include "../AppSettings.h"
#include "../AssistOptionUtils.h"
#include "../LongNoteModeUtils.h"
#include "../PlayOptionUtils.h"
#include "play/Pacemaker.h"

#include <string>

namespace main_menu_profile {

inline const char *gaugeSettingId(GaugeType gaugeType) {
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    return "assisted_easy";
  case GaugeType::Easy:
    return "easy";
  case GaugeType::Normal:
    return "normal";
  case GaugeType::Hard:
    return "hard";
  case GaugeType::ExHard:
    return "exhard";
  case GaugeType::Hazard:
    return "hazard";
  default:
    return "normal";
  }
}

inline GaugeType gaugeTypeFromSettingId(const std::string &id,
                                        GaugeType fallback) {
  if (id == "assisted_easy") return GaugeType::AssistedEasy;
  if (id == "easy") return GaugeType::Easy;
  if (id == "normal") return GaugeType::Normal;
  if (id == "hard") return GaugeType::Hard;
  if (id == "exhard") return GaugeType::ExHard;
  if (id == "hazard") return GaugeType::Hazard;
  return fallback;
}

inline const char *gaugeAutoShiftSettingId(GaugeAutoShiftMode mode) {
  switch (mode) {
  case GaugeAutoShiftMode::Continue: return "continue";
  case GaugeAutoShiftMode::SurvivalToGroove:
    return "survival_to_groove";
  case GaugeAutoShiftMode::BestClear: return "best_clear";
  case GaugeAutoShiftMode::SelectToUnder: return "select_to_under";
  case GaugeAutoShiftMode::None:
  default: return "none";
  }
}

inline GaugeAutoShiftMode gaugeAutoShiftFromSettingId(const std::string &id) {
  if (id == "continue") return GaugeAutoShiftMode::Continue;
  if (id == "survival_to_groove") {
    return GaugeAutoShiftMode::SurvivalToGroove;
  }
  if (id == "best_clear") return GaugeAutoShiftMode::BestClear;
  if (id == "select_to_under") {
    return GaugeAutoShiftMode::SelectToUnder;
  }
  return GaugeAutoShiftMode::None;
}

struct Selections {
  GaugeType gaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  std::string playOption = AppSettings::kDefaultPlayOption;
  std::string longNoteMode = AppSettings::kDefaultLnMode;
  std::string assistOption = AppSettings::kDefaultAssistOption;
  std::string pacemakerTarget = AppSettings::kDefaultPacemakerTarget;

  static Selections fromSettings(const AppSettings &settings) {
    Selections selections;
    selections.reload(settings);
    return selections;
  }

  void reload(const AppSettings &settings) {
    gaugeAutoShift =
        gaugeAutoShiftFromSettingId(settings.selectedGaugeAutoShiftMode);
    const bool legacyAutoShift = settings.selectedGaugeType.rfind("gas", 0) == 0;
    if (legacyAutoShift && gaugeAutoShift == GaugeAutoShiftMode::None &&
        (settings.selectedGaugeType == "gas" ||
         settings.selectedGaugeType == "gas_best_clear")) {
      gaugeAutoShift = GaugeAutoShiftMode::BestClear;
    } else if (legacyAutoShift && gaugeAutoShift == GaugeAutoShiftMode::None &&
               settings.selectedGaugeType == "gas_continue") {
      gaugeAutoShift = GaugeAutoShiftMode::Continue;
    } else if (legacyAutoShift && gaugeAutoShift == GaugeAutoShiftMode::None &&
               settings.selectedGaugeType == "gas_survival_to_groove") {
      gaugeAutoShift = GaugeAutoShiftMode::SurvivalToGroove;
    } else if (legacyAutoShift && gaugeAutoShift == GaugeAutoShiftMode::None &&
               settings.selectedGaugeType == "gas_select_to_under") {
      gaugeAutoShift = GaugeAutoShiftMode::SelectToUnder;
    }
    gaugeType = legacyAutoShift
                    ? GaugeType::ExHard
                    : gaugeTypeFromSettingId(settings.selectedGaugeType,
                                             GaugeType::Normal);
    gaugeAutoShiftLowerBound = gaugeTypeFromSettingId(
        settings.selectedGaugeAutoShiftLowerBound,
        GaugeType::AssistedEasy);
    playOption = play_options::normalizePlayOption(settings.selectedPlayOption);
    longNoteMode = long_note_mode::parseId(settings.selectedLnMode,
                                           AppSettings::kDefaultLnMode);
    assistOption = assist_options::normalize(settings.selectedAssistOption);
    pacemakerTarget =
        pacemaker::normalizeTargetId(settings.selectedPacemakerTarget);
  }

  void applyTo(AppSettings &settings) const {
    settings.selectedGaugeType = gaugeSettingId(gaugeType);
    settings.selectedGaugeAutoShiftMode =
        gaugeAutoShiftSettingId(gaugeAutoShift);
    settings.selectedGaugeAutoShiftLowerBound =
        gaugeSettingId(gaugeAutoShiftLowerBound);
    settings.selectedPlayOption = play_options::normalizePlayOption(playOption);
    settings.selectedLnMode =
        long_note_mode::parseId(longNoteMode, AppSettings::kDefaultLnMode);
    settings.selectedAssistOption = assist_options::normalize(assistOption);
    settings.selectedPacemakerTarget =
        pacemaker::normalizeTargetId(pacemakerTarget);
  }

  bool operator==(const Selections &) const = default;
};

} // namespace main_menu_profile
