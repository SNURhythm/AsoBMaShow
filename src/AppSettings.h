#pragma once

#include "JudgementIndicatorRange.h"
#include "audio/PlaybackRate.h"
#include "ir/IrProfileSettings.h"
#include "settings/AudioVideoSettings.h"
#include "skin/SkinProfileSettings.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

class AppSettingsStore;

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

  // Exact PlayConfig.fixhispeed values from pinned Beatoraja.  Fixed modes
  // store duration; OFF stores a raw Hi-Speed value instead.
  enum class HiSpeedFixMode {
    Off = 0,
    Start = 1,
    Max = 2,
    Main = 3,
    Min = 4,
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
  // Beatoraja PlayConfig.DURATION_MIN/MAX.  Duration is canonical in
  // milliseconds; green number is the derived IntegerProperty 313 value.
  static constexpr int kMinVisibleTimeMs = 1;
  static constexpr int kMaxVisibleTimeMs = 10000;
  static constexpr int kDefaultVisibleTimeDurationMilliseconds = 500;
  static constexpr int kMinVisibleTimeGreenNumber = 0;
  static constexpr int kMaxVisibleTimeGreenNumber = 6000;
  static constexpr float kMinGameplayHispeed = 0.01F;
  static constexpr float kMaxGameplayHispeed = 20.0F;
  static constexpr float kDefaultHispeedMargin = 0.25F;
  static constexpr float kMaxHispeedMargin = 10.0F;
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
  static constexpr int kMaxNoteStartPositionPercent = 100;
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
  static constexpr int kMinJudgementIndicatorRangeMilliseconds =
      judgement_indicator::kMinRangeMilliseconds;
  static constexpr int kMaxJudgementIndicatorRangeMilliseconds =
      judgement_indicator::kMaxRangeMilliseconds;
  static constexpr int kDefaultJudgementIndicatorRangeMilliseconds =
      judgement_indicator::kDefaultRangeMilliseconds;
  static constexpr float kMinJudgementTextY = 0.0f;
  static constexpr float kMaxJudgementTextY = 1.0f;
  static constexpr float kDefaultJudgementTextY = 0.55f;
  static constexpr const char *kDefaultGaugeType = "normal";
  static constexpr const char *kDefaultPlayOption = "NORMAL";
  static constexpr const char *kDefaultLnMode = "LN";
  static constexpr const char *kDefaultAssistOption = "OFF";
  static constexpr const char *kDefaultPacemakerTarget = "BEST";
  // PlayerConfig strings are copied into the per-frame skin authority. Keep
  // corrupt settings from turning that snapshot into unbounded allocation
  // work while retaining ample room for authored display names and targets.
  static constexpr std::size_t kMaximumSkinPropertyStringBytes = 256;
  static constexpr std::size_t kMaximumSkinTargetListEntries = 256;
  static constexpr std::size_t kMaximumSkinTargetListBytes = 16 * 1024;

  player_settings::AudioVideoSettings audioVideo =
      player_settings::defaultAudioVideoSettingsForPlatform();
  int audioOffsetMs = 0;
  int visualOffsetMs = 0;
  // Matches Beatoraja PlayConfig.duration. Green number is the derived live
  // LaneRenderer duration, not a separate stored setting.
  int visibleTimeDurationMilliseconds =
      kDefaultVisibleTimeDurationMilliseconds;
  // Matches PlayConfig.hispeed; it is persisted and used only in OFF mode.
  float gameplayHispeed = 1.0F;
  // Matches PlayConfig.hispeedmargin.
  float hispeedMargin = kDefaultHispeedMargin;
  bool visibleTimeUseMilliseconds = false;
  HiSpeedFixMode hispeedFixMode = HiSpeedFixMode::Main;
  bool inputKeysoundEnabled = true;
  bool prepMetronomeEnabled = false;
  bool startLaneIndicatorsEnabled = true;
  bool showInvisibleNotes = false;
  // PlayerConfig.showpastnote. Its narrow LaneRenderer condition is applied
  // by playfield projection rather than broadening past-note rendering.
  bool showPastNotes = false;
  // Beatoraja PlayerConfig.markprocessednote. When enabled, judged normal
  // notes use SkinNote's processed-note visual instead of the normal visual.
  bool markProcessedNotes = false;
  // PlayerConfig.isCustomJudge() and isShowjudgearea(), retained for the
  // matching gameplay skin image-index properties.
  bool customJudge = false;
  bool showJudgeArea = false;
  // Remaining raw PlayerConfig/PlayConfig values consumed by pinned
  // IntegerPropertyFactory image indexes. They remain separate from Aso's
  // gameplay options because their source semantics are configuration state.
  bool notesDisplayTimingAutoAdjust = false;
  // PlayerConfig.judgetiming, distinct from Aso's visual calibration. This
  // moves only LaneRenderer's display clock and can be mutated live by the
  // source auto-adjust rule.
  int notesDisplayTimingMilliseconds = 0;
  std::array<int, 4> autoSaveReplay{};
  bool guideSoundEffects = false;
  int extraNoteDepth = 0;
  int mineMode = 0;
  int scrollMode = 0;
  int longNoteModifierMode = 0;
  int sevenToNinePattern = 0;
  int sevenToNineType = 0;
  bool constantScroll = false;
  // PlayConfig.constantFadeinTime. The source default is 100 ms.
  int constantFadeInMilliseconds = 100;
  bool touchVisualizationEnabled = true;
  bool archiveChartPreviewEnabled = true;
  bool findBmsSkipUnarchivingForNonSolidArchives = false;
  bool bgaEnabled = true;
  int bgaBrightnessPercent = kDefaultBgaBrightnessPercent;
  float bgaBlurStrength = kDefaultBgaBlurStrength;
  BgaDisplayMode bgaDisplayMode = BgaDisplayMode::Fit;
  float laneAngleDegrees = kDefaultLaneAngleDegrees;
  float laneLength = kDefaultLaneLength;
  int laneBeamLengthPercent = kDefaultLaneBeamLengthPercent;
  int noteStartPositionPercent = kDefaultNoteStartPositionPercent;
  bool laneCoverEnabled = true;
  // PlayConfig's Lift/HIDDEN configuration. The pinned source defaults each
  // ratio to 0.1 while both planes begin disabled.
  bool liftEnabled = false;
  float liftRatio = 0.1F;
  bool hiddenEnabled = false;
  float hiddenRatio = 0.1F;
  // Matches PlayConfig.hispeedautoadjust. When lane cover changes during play,
  // keep the green number at the current BPM instead of the configured
  // reference BPM.
  bool hispeedAutoAdjust = false;
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
  int judgementIndicatorRangeMilliseconds =
      kDefaultJudgementIndicatorRangeMilliseconds;
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
  bool systemPlaybackShowJacket = true;
  bool systemPlaybackShowTitle = true;
  bool systemPlaybackShowArtist = true;
  int musicPlayerPlaybackRatePercent = 100;
  audio::PlaybackMode musicPlayerPlaybackMode = audio::PlaybackMode::PitchShift;
  bool gameplayClubModeEnabled = false;
  bool musicPlayerClubModeEnabled = false;
  std::string selectedGameplayRuleset = "lr2";
  std::string selectedGaugeType = kDefaultGaugeType;
  std::string selectedGaugeAutoShiftMode = "best_clear";
  std::string selectedGaugeAutoShiftLowerBound = "assisted_easy";
  std::string selectedPlayOption = kDefaultPlayOption;
  std::string selectedLnMode = kDefaultLnMode;
  std::string selectedAssistOption = kDefaultAssistOption;
  std::string selectedPacemakerTarget = kDefaultPacemakerTarget;
  // Direct StringPropertyFactory PlayerConfig values. Mode and difficulty
  // retain ModeFilter/DifficultyFilter display text, while sort and chart
  // replication retain their source identifiers verbatim.
  std::string skinModeFilterName = "ALL";
  std::string skinSortId = "TITLE";
  std::string skinDifficultyFilterName = "ALL";
  std::string skinChartReplicationMode = "RIVALCHART";
  // Raw PlayerConfig.targetid and targetlist. TargetProperty performs target
  // lookup only when StringPropertyFactory asks for a neighbouring label, so
  // these remain independent from Aso's selectable pacemaker targets.
  std::string skinTargetId = "MAX";
  std::vector<std::string> skinTargetList = {
      "RATE_A-", "RATE_A", "RATE_A+", "RATE_AA-", "RATE_AA", "RATE_AA+",
      "RATE_AAA-", "RATE_AAA", "RATE_AAA+", "RATE_MAX-", "MAX",
      "RANK_NEXT", "IR_NEXT_1", "IR_NEXT_2", "IR_NEXT_3", "IR_NEXT_4",
      "IR_NEXT_5", "IR_NEXT_10", "IR_RANK_1", "IR_RANK_5", "IR_RANK_10",
      "IR_RANK_20", "IR_RANK_30", "IR_RANK_40", "IR_RANK_50",
      "IR_RANKRATE_5", "IR_RANKRATE_10", "IR_RANKRATE_15",
      "IR_RANKRATE_20", "IR_RANKRATE_25", "IR_RANKRATE_30",
      "IR_RANKRATE_35", "IR_RANKRATE_40", "IR_RANKRATE_45",
      "IR_RANKRATE_50", "RIVAL_RANK_1", "RIVAL_RANK_2", "RIVAL_RANK_3",
      "RIVAL_NEXT_1", "RIVAL_NEXT_2", "RIVAL_NEXT_3"};
  int selectedPlaybackRatePercent = 100;
  audio::PlaybackMode selectedPlaybackMode = audio::PlaybackMode::PitchShift;
  bool defaultDifficultyTablesSeeded = false;
  std::map<std::string, ir::IrProviderSettings> irProviders = {
      {std::string(ir::kTachiProviderId), ir::IrProviderSettings{}},
  };
  skin::SkinProfileSettings skin;

  void sanitize();
  float playAreaWidthForKeyMode(int keyMode) const;
  void setPlayAreaWidthForKeyMode(int keyMode, float width);
  [[nodiscard]] static constexpr int
  durationMillisecondsToGreenNumber(int milliseconds) noexcept {
    return (std::clamp(milliseconds, kMinVisibleTimeMs,
                       kMaxVisibleTimeMs) *
            3) /
           5;
  }
  [[nodiscard]] static constexpr int
  greenNumberToDurationMilliseconds(int greenNumber) noexcept {
    const int bounded = std::clamp(greenNumber, kMinVisibleTimeGreenNumber,
                                   kMaxVisibleTimeGreenNumber);
    // IntegerPropertyFactory evaluates `duration * 3 / 5`. Choose the
    // smallest integral duration that derives the requested green value.
    return std::clamp((bounded * 5 + 2) / 3, kMinVisibleTimeMs,
                      kMaxVisibleTimeMs);
  }
  [[nodiscard]] constexpr int visibleTimeGreenNumber() const noexcept {
    return durationMillisecondsToGreenNumber(visibleTimeDurationMilliseconds);
  }
  constexpr void setVisibleTimeGreenNumber(int greenNumber) noexcept {
    visibleTimeDurationMilliseconds =
        greenNumberToDurationMilliseconds(greenNumber);
  }
  bool operator==(const AppSettings &) const = default;

private:
  friend class AppSettingsStore;
  static bool parseLegacyCfg(std::istream &input, AppSettings &settings,
                             std::vector<std::string> *diagnostics = nullptr);
  static bool loadLegacyCfg(const std::filesystem::path &path,
                            AppSettings &settings,
                            std::vector<std::string> *diagnostics = nullptr);
};
