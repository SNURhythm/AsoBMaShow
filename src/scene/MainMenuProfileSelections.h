#pragma once

#include "../AppSettings.h"
#include "../AssistOptionUtils.h"
#include "../LongNoteModeUtils.h"
#include "../PlayOptionUtils.h"
#include "play/Pacemaker.h"

#include <string>

namespace main_menu_profile {

inline const char *gaugeSettingId(GaugeType gaugeType, bool autoShift) {
  if (autoShift) {
    return "gas";
  }
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

struct Selections {
  GaugeType gaugeType = GaugeType::Normal;
  bool gaugeAutoShift = false;
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
    gaugeAutoShift = settings.selectedGaugeType == "gas";
    if (gaugeAutoShift) {
      gaugeType = GaugeType::ExHard;
    } else if (settings.selectedGaugeType == "assisted_easy") {
      gaugeType = GaugeType::AssistedEasy;
    } else if (settings.selectedGaugeType == "easy") {
      gaugeType = GaugeType::Easy;
    } else if (settings.selectedGaugeType == "hard") {
      gaugeType = GaugeType::Hard;
    } else if (settings.selectedGaugeType == "exhard") {
      gaugeType = GaugeType::ExHard;
    } else if (settings.selectedGaugeType == "hazard") {
      gaugeType = GaugeType::Hazard;
    } else {
      gaugeType = GaugeType::Normal;
    }
    playOption = play_options::normalizePlayOption(settings.selectedPlayOption);
    longNoteMode = long_note_mode::parseId(settings.selectedLnMode,
                                           AppSettings::kDefaultLnMode);
    assistOption = assist_options::normalize(settings.selectedAssistOption);
    pacemakerTarget =
        pacemaker::normalizeTargetId(settings.selectedPacemakerTarget);
  }

  void applyTo(AppSettings &settings) const {
    settings.selectedGaugeType = gaugeSettingId(gaugeType, gaugeAutoShift);
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
