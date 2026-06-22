#pragma once

#include <filesystem>
#include <string>

class AppSettings {
public:
  enum class BgaDisplayMode {
    Fit = 0,
    Fill = 1,
    Stretch = 2,
  };

  enum class NotePriorityMode {
    Lowest = 0,
    Combo = 1,
    Duration = 2,
    Score = 3,
  };

  enum class JudgementIndicatorRenderMode {
    World3D = 0,
    Hud2D = 1,
  };

  enum class VisibleTimeBpmStrategy {
    Chart = 0,
    MostPrevalent = 1,
  };

  enum class JudgementCounterPosition {
    Top = 0,
    Left = 1,
    Right = 2,
  };

  enum class JudgementTimingDisplayCriteria {
    GreatOrBelow = 0,
    PGreatOrBelow = 1,
    GoodOrBelow = 2,
    BadOrBelow = 3,
    Off = 4,
  };

  enum class GaugeBarPosition {
    World = 0,
    Left = 1,
    Right = 2,
  };

  enum class UiThemeMode {
    Dark = 0,
    Light = 1,
  };

  static constexpr int kMinAudioOffsetMs = -300;
  static constexpr int kMaxAudioOffsetMs = 300;
  static constexpr int kMinVisualOffsetMs = -500;
  static constexpr int kMaxVisualOffsetMs = 500;
  static constexpr int kMinVisibleTimeGreenNumber = 60;
  static constexpr int kMaxVisibleTimeGreenNumber = 1200;
  static constexpr int kMinVisibleTimeMs = 100;
  static constexpr int kMaxVisibleTimeMs = 2000;
  static constexpr int kMinBgaBrightnessPercent = 0;
  static constexpr int kMaxBgaBrightnessPercent = 100;
  static constexpr int kDefaultBgaBrightnessPercent = 100;
  static constexpr float kMinBgaBlurStrength = 0.0f;
  static constexpr float kMaxBgaBlurStrength = 8.0f;
  static constexpr float kDefaultBgaBlurStrength = 2.0f;
  static constexpr float kMinLaneAngleDegrees = 4.0f;
  static constexpr float kMaxLaneAngleDegrees = 28.0f;
  static constexpr float kDefaultLaneAngleDegrees = 13.4f;
  static constexpr float kMinLaneLength = 5.0f;
  static constexpr float kMaxLaneLength = 12.0f;
  static constexpr float kDefaultLaneLength = 8.0f;
  static constexpr int kMinLaneBeamLengthPercent = 0;
  static constexpr int kMaxLaneBeamLengthPercent = 100;
  static constexpr int kDefaultLaneBeamLengthPercent = 100;
  static constexpr int kMinNoteStartPositionPercent = 0;
  static constexpr int kMaxNoteStartPositionPercent = 90;
  static constexpr int kDefaultNoteStartPositionPercent = 0;
  static constexpr float kMinPlayAreaWidth = 4.0f;
  static constexpr float kMaxPlayAreaWidth = 12.0f;
  static constexpr float kDefaultPlayAreaWidth = 8.0f;
  static constexpr float kMinJudgementIndicatorY = 0.0f;
  static constexpr float kMaxJudgementIndicatorY = 1.0f;
  static constexpr float kDefaultJudgementIndicatorY = 0.5f;
  static constexpr float kMinJudgementIndicatorWidthScale = 0.5f;
  static constexpr float kMaxJudgementIndicatorWidthScale = 2.0f;
  static constexpr float kDefaultJudgementIndicatorWidthScale = 1.0f;
  static constexpr float kMinJudgementTextY = 0.0f;
  static constexpr float kMaxJudgementTextY = 1.0f;
  static constexpr float kDefaultJudgementTextY = 0.55f;
  static constexpr const char *kDefaultGaugeType = "normal";
  static constexpr const char *kDefaultPlayOption = "NORMAL";
  static constexpr const char *kDefaultAssistOption = "OFF";

  int audioOffsetMs = 0;
  int visualOffsetMs = 0;
  int visibleTimeGreenNumber = 400;
  bool visibleTimeUseMilliseconds = false;
  VisibleTimeBpmStrategy visibleTimeBpmStrategy =
      VisibleTimeBpmStrategy::Chart;
  bool inputKeysoundEnabled = true;
  bool showInvisibleNotes = false;
  bool touchVisualizationEnabled = true;
  bool archiveChartPreviewEnabled = true;
  bool bgaEnabled = true;
  int bgaBrightnessPercent = kDefaultBgaBrightnessPercent;
  float bgaBlurStrength = kDefaultBgaBlurStrength;
  BgaDisplayMode bgaDisplayMode = BgaDisplayMode::Fit;
  float laneAngleDegrees = kDefaultLaneAngleDegrees;
  float laneLength = kDefaultLaneLength;
  int laneBeamLengthPercent = kDefaultLaneBeamLengthPercent;
  int noteStartPositionPercent = kDefaultNoteStartPositionPercent;
  bool floatingLaneCoverEnabled = true;
  float playAreaWidth4K = kDefaultPlayAreaWidth;
  float playAreaWidth5K = kDefaultPlayAreaWidth;
  float playAreaWidth6K = kDefaultPlayAreaWidth;
  float playAreaWidth7K = kDefaultPlayAreaWidth;
  float playAreaWidth8K = kDefaultPlayAreaWidth;
  float playAreaWidth10K = kDefaultPlayAreaWidth;
  float playAreaWidth14K = kDefaultPlayAreaWidth;
  NotePriorityMode notePriorityMode = NotePriorityMode::Lowest;
  bool judgementIndicatorEnabled = true;
  float judgementIndicatorY = kDefaultJudgementIndicatorY;
  float judgementIndicatorWidthScale = kDefaultJudgementIndicatorWidthScale;
  float judgementTextY = kDefaultJudgementTextY;
  JudgementIndicatorRenderMode judgementIndicatorRenderMode =
      JudgementIndicatorRenderMode::World3D;
  bool judgementCounterEnabled = true;
  JudgementCounterPosition judgementCounterPosition =
      JudgementCounterPosition::Right;
  JudgementTimingDisplayCriteria judgementTimingFastSlowCriteria =
      JudgementTimingDisplayCriteria::GreatOrBelow;
  JudgementTimingDisplayCriteria judgementTimingMillisecondsCriteria =
      JudgementTimingDisplayCriteria::GreatOrBelow;
  GaugeBarPosition gaugeBarPosition = GaugeBarPosition::World;
  UiThemeMode uiThemeMode = UiThemeMode::Dark;
  std::string selectedGaugeType = kDefaultGaugeType;
  std::string selectedPlayOption = kDefaultPlayOption;
  std::string selectedAssistOption = kDefaultAssistOption;

  void sanitize();
  float playAreaWidthForKeyMode(int keyMode) const;
  void setPlayAreaWidthForKeyMode(int keyMode, float width);
  bool save() const;
  static AppSettings load();

private:
  static std::filesystem::path configPath();
};
