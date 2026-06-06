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

  static constexpr int kMinInputOffsetMs = -300;
  static constexpr int kMaxInputOffsetMs = 300;
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
  static constexpr float kMinJudgementIndicatorY = 0.0f;
  static constexpr float kMaxJudgementIndicatorY = 1.0f;
  static constexpr float kDefaultJudgementIndicatorY = 0.5f;
  static constexpr const char *kDefaultGaugeType = "normal";
  static constexpr const char *kDefaultPlayOption = "NORMAL";

  int inputOffsetMs = 0;
  int visualOffsetMs = 0;
  int visibleTimeGreenNumber = 400;
  bool visibleTimeUseMilliseconds = false;
  bool inputKeysoundEnabled = true;
  bool bgaEnabled = true;
  int bgaBrightnessPercent = kDefaultBgaBrightnessPercent;
  float bgaBlurStrength = kDefaultBgaBlurStrength;
  BgaDisplayMode bgaDisplayMode = BgaDisplayMode::Fit;
  float laneAngleDegrees = kDefaultLaneAngleDegrees;
  float laneLength = kDefaultLaneLength;
  NotePriorityMode notePriorityMode = NotePriorityMode::Lowest;
  bool judgementIndicatorEnabled = true;
  float judgementIndicatorY = kDefaultJudgementIndicatorY;
  JudgementIndicatorRenderMode judgementIndicatorRenderMode =
      JudgementIndicatorRenderMode::World3D;
  std::string selectedGaugeType = kDefaultGaugeType;
  std::string selectedPlayOption = kDefaultPlayOption;

  void sanitize();
  bool save() const;
  static AppSettings load();

private:
  static std::filesystem::path configPath();
};
