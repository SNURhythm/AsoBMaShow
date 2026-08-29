#pragma once

#include "../bms_parser.hpp"
#include "../scene/play/SkinGameplayGraphState.h"
#include "../scene/play/RhythmState.h"
#include "../context.h"

#include <optional>
#include <array>
#include <limits>
#include <string>
#include <vector>

class View;
struct ResultPresentationModel;

struct ResultPreviousBestData {
  int score = 0;
  int maxScore = 0;
  int maxCombo = 0;
  int comboBreak = 0;
  std::optional<int> badPoints;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
  std::optional<std::string> attemptId;
};

struct ResultPacemakerData {
  std::string label;
  int targetScore = 0;
  int delta = 0;
  bool usesReplayProgression = false;
};

struct ResultPlayerHistoryData {
  int playCount = 0;
  int clearCount = 0;
  std::array<int, 5> judgementCounts{};
  std::int64_t playDurationSeconds = 0;
};

struct ResultIrRankingEntryData {
  int rank = 0;
  std::string playerName;
  int score = 0;
  int clearType = kClearTypeFailedRank;
  bool currentUser = false;
};

// Result property factories retain access to PlayerConfig and PlayConfig even
// though a result has no live playfield. Keep only that source-facing state in
// the result snapshot so skins do not resolve it through an unrelated numeric
// property family.
struct ResultSkinConfigurationData {
  float gameplayHispeed = 1.0F;
  int notesDisplayTimingMilliseconds = 0;
  int visibleTimeDurationMilliseconds = 667;
  int hispeedFixMode = 0;
  bool bgaEnabled = true;
  bool bpmGuideEnabled = false;
  bool customJudge = false;
  bool showJudgeArea = false;
  bool markProcessedNotes = false;
  bool notesDisplayTimingAutoAdjust = false;
  std::array<int, 4> autoSaveReplay{};
  bool guideSoundEffects = false;
  int extraNoteDepth = 0;
  int mineMode = 0;
  int scrollMode = 0;
  int longNoteModifierMode = 0;
  int sevenToNinePattern = 0;
  int sevenToNineType = 0;
  bool laneCoverEnabled = true;
  bool liftEnabled = false;
  bool hiddenEnabled = false;
  bool hispeedAutoAdjust = false;
  int judgeAlgorithmImageIndex = std::numeric_limits<int>::min();
  // These use PlayerConfig's numeric domain, which differs from Aso's
  // GameplayGaugeTypes enum ordering for auto-shift modes.
  int gaugeAutoShiftImageIndex = 0;
  int bottomShiftableGaugeImageIndex = 0;
  std::string modeFilterName = "ALL";
  std::string sortId = "TITLE";
  std::string difficultyFilterName = "ALL";
  std::string chartReplicationMode = "RIVALCHART";
  std::string irName;
  std::string irAccountName;
  std::string skinTargetId = "MAX";
  std::vector<std::string> skinTargetList;
};

struct ResultSkinData {
  const RhythmState *state;
  const bms_parser::ChartMeta *meta;
  ApplicationContext *context;
  std::optional<ResultSkinConfigurationData> configuration;
  bool irOnline = false;
  // Result artwork selectors describe textures that the selected skin can
  // actually draw. A nonempty chart declaration alone is insufficient when
  // the file cannot be read or decoded during result-resource preparation.
  bool stageFileAvailable = false;
  bool bannerAvailable = false;
  bool backBmpAvailable = false;
  bool chartHasDocument = false;
  // SongReview.favorite bitfield captured from the chart library. Result
  // image selectors 89/90 project its song/chart favourite and invisible
  // pairs just as IntegerPropertyFactory.IndexType does.
  std::optional<int> songReviewFavorite;
  View **outGraphPlaceholder = nullptr;
  bool showControls = true;
  bool showTimingAnalytics = false;
  bool showResultGraph = true;
  std::string playerName;
  std::string tableName;
  std::string tableLevel;
  std::string playModeLabel;
  std::string laneOrderLabel;
  std::string difficultyLabel;
  std::string courseTitle;
  std::vector<std::string> courseTitles;
  std::string skinName;
  std::string skinAuthor;
  std::optional<float> playLevelOverride;
  std::optional<int> keyModeOverride;
  std::optional<GaugeType> gaugeTypeOverride;
  std::optional<int> difficultyOverride;
  // Result image-index properties expose the choices captured with the
  // completed replay, rather than the currently editable player settings.
  std::optional<int> replayRandomOption1P;
  std::optional<int> replayRandomOption2P;
  std::optional<int> replayDoublePlayOption;
  int replayKeyMode = 0;
  std::optional<std::vector<int>> replayLaneShufflePattern1P;
  std::optional<std::vector<int>> replayLaneShufflePattern2P;
  std::vector<ResultIrRankingEntryData> irRankingEntries;
  std::optional<int> irCurrentUserRank;
  std::string chartMd5;
  std::string chartSha256;
  bool autoPlayResult = false;
  bool courseResult = false;
  std::optional<std::string> headerDifficultyLabelOverride;
  std::optional<std::string> currentClearLabelOverride;
  std::optional<int> currentClearRankOverride;
  // ScoreData.date backing lastplay_* properties, in Unix seconds. It is
  // captured only where the result lifecycle provides an authenticated date.
  std::optional<std::int64_t> currentScoreDateUnixSeconds;
  std::optional<ResultPreviousBestData> previousBest;
  std::optional<ResultPreviousBestData> previousLampBest;
  std::optional<ResultPacemakerData> pacemaker;
  std::optional<ResultPlayerHistoryData> playerHistory;
  const ResultPresentationModel *presentation = nullptr;
  // AbstractResult's result-only timing distribution is computed from the
  // completed replay in chart-time milliseconds, bounded to ±150 ms.
  std::optional<double> timingAverageMillis;
  std::optional<double> timingStandardDeviationMillis;
  std::optional<long long> averageJudgeMicros;
  std::vector<int> timingDistribution;
  int timingDistributionCenter = 150;
  // Captured at the gameplay-to-result boundary. The shared immutable graph
  // snapshots keep every graph-capable Beatoraja result object on the same
  // authoritative data that gameplay rendered on its final frame.
  SkinGameplayGraphState gameplayGraph;
};
