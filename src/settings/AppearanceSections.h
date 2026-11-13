#pragma once

#include "../AppSettings.h"

namespace settings_edit {
enum class AppearanceSection { Lane, Notes, Effects, Hud, SharedOverlays, LaneCovers };

inline AppSettings::PresentationOrientation otherOrientation(AppSettings::PresentationOrientation value) {
  return value == AppSettings::PresentationOrientation::Portrait
      ? AppSettings::PresentationOrientation::Landscape : AppSettings::PresentationOrientation::Portrait;
}

template<class Map>
inline void copyMode(Map &destination, const Map &source, int mode) {
  if (const auto found = source.find(mode); found != source.end())
    destination.insert_or_assign(mode, found->second);
  else destination.erase(mode);
}

// The section's shared fields and only the requested mode are replaced. Never
// assign a whole presentation: it also owns skin selections and other editors.
inline void copyAppearanceSection(AppSettings::PresentationSettings &destination,
                                  const AppSettings::PresentationSettings &source,
                                  AppearanceSection section, int mode,
                                  AppSettings::PresentationOrientation orientation) {
  switch (section) {
  case AppearanceSection::Lane:
    destination.scratchLaneOnRight = source.scratchLaneOnRight;
    destination.laneAngleDegrees = source.laneAngleDegrees;
    destination.laneLength = source.laneLength;
    copyMode(destination.builtInLanes, source.builtInLanes, mode);
    copyMode(destination.builtInJudgeLines, source.builtInJudgeLines, mode);
    {
      float AppSettings::PresentationSettings::*width = nullptr;
      switch (mode) {
      case 7: width = &AppSettings::PresentationSettings::playAreaWidth7K; break;
      case -5: width = &AppSettings::PresentationSettings::playAreaWidthScratchless5K; break;
      case -7: width = &AppSettings::PresentationSettings::playAreaWidthScratchless7K; break;
      case 4: width = &AppSettings::PresentationSettings::playAreaWidth4K; break;
      case 5: width = &AppSettings::PresentationSettings::playAreaWidth5K; break;
      case 6: width = &AppSettings::PresentationSettings::playAreaWidth6K; break;
      case 8: width = &AppSettings::PresentationSettings::playAreaWidth8K; break;
      case 10: width = &AppSettings::PresentationSettings::playAreaWidth10K; break;
      case 14: width = &AppSettings::PresentationSettings::playAreaWidth14K; break;
      default: break;
      }
      const auto policy = player_settings::presentationGeometryPolicy(orientation);
      if (width) destination.*width = std::clamp(source.*width, policy.width.minimum, policy.width.maximum);
      destination.laneAngleDegrees = std::clamp(destination.laneAngleDegrees, policy.angle.minimum, policy.angle.maximum);
      destination.laneLength = std::clamp(destination.laneLength, policy.length.minimum, policy.length.maximum);
    }
    break;
  case AppearanceSection::Notes:
    copyMode(destination.builtInNotes, source.builtInNotes, mode);
    break;
  case AppearanceSection::Effects:
    destination.laneBeamLengthPercent = source.laneBeamLengthPercent;
    copyMode(destination.keybombs, source.keybombs, mode);
    break;
  case AppearanceSection::Hud:
    destination.judgementTextVisibility = source.judgementTextVisibility;
    destination.judgementTextY = source.judgementTextY;
    destination.judgementTimingY = source.judgementTimingY;
    destination.judgementTextSizePercent = source.judgementTextSizePercent;
    destination.judgementTextBold = source.judgementTextBold;
    destination.judgementComboSeparated = source.judgementComboSeparated;
    destination.comboTextY = source.comboTextY;
    destination.comboTextSizePercent = source.comboTextSizePercent;
    destination.comboTextBold = source.comboTextBold;
    destination.judgementTimingSizePercent = source.judgementTimingSizePercent;
    destination.judgementTimingBold = source.judgementTimingBold;
    destination.pacemakerDiffY = source.pacemakerDiffY;
    destination.pacemakerDiffSizePercent = source.pacemakerDiffSizePercent;
    destination.pacemakerDiffBold = source.pacemakerDiffBold;
    destination.judgementTimingFastSlowCriteria = source.judgementTimingFastSlowCriteria;
    destination.judgementTimingMillisecondsCriteria = source.judgementTimingMillisecondsCriteria;
    destination.gaugeBarPosition = source.gaugeBarPosition;
    destination.judgementIndicatorRenderMode = source.judgementIndicatorRenderMode;
    break;
  case AppearanceSection::SharedOverlays:
    destination.judgementIndicatorEnabled = source.judgementIndicatorEnabled;
    destination.judgementIndicatorY = source.judgementIndicatorY;
    destination.judgementIndicatorWidthScale = source.judgementIndicatorWidthScale;
    destination.judgementIndicatorLineThicknessPercent = source.judgementIndicatorLineThicknessPercent;
    destination.judgementIndicatorEventHeightPercent = source.judgementIndicatorEventHeightPercent;
    destination.judgementIndicatorEventThicknessPercent = source.judgementIndicatorEventThicknessPercent;
    destination.judgementIndicatorColorization = source.judgementIndicatorColorization;
    destination.judgementIndicatorRangeMilliseconds = source.judgementIndicatorRangeMilliseconds;
    destination.judgementCounterEnabled = source.judgementCounterEnabled;
    destination.judgementCounterPosition = source.judgementCounterPosition;
    break;
  case AppearanceSection::LaneCovers:
    destination.setLaneCoverState(source.laneCoverState());
    break;
  }
}

inline void resetAppearanceSection(AppSettings::PresentationSettings &destination,
                                   AppearanceSection section, int mode,
                                   AppSettings::PresentationOrientation orientation) {
  copyAppearanceSection(destination, AppSettings::PresentationSettings(orientation), section, mode, orientation);
}
} // namespace settings_edit
