#pragma once
#include "../ReplayData.h"
#include "Scene.h"
#include "play/RhythmState.h"
#include "../bms_parser.hpp"
#include "../skin/ISkin.h"
#include "../skin/SkinTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>

struct CoursePlaySession;

struct ResultPracticeOptions {
  bool enabled = false;
  unsigned long long startPosition = 0;
  bool autoKeySound = false;
  bool autoPlay = false;
  GaugeType gaugeType = GaugeType::Normal;
  bool gaugeAutoShift = false;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  int longNoteMode = 0;
  std::string assistOption = assist_options::kOff;
  unsigned long long leadInMicros = 0;
  Scene *returnScene = nullptr;
  std::function<void(const ReplayData &)> practiceGhostCallback;
};

enum class ResultCourseMode {
  None,
  Stage,
  CourseResult,
};

struct ResultCourseOptions {
  ResultCourseMode mode = ResultCourseMode::None;
  std::shared_ptr<CoursePlaySession> session = nullptr;
};

class TextView;
class Button;

class ResultScene : public Scene {
public:
  ResultScene(
      ApplicationContext &context, const bms_parser::ChartMeta &meta,
      const RhythmState &state, const ScoreProvenance &attemptProvenance,
      const ReplayData *replay = nullptr, bool shouldSaveScore = true,
      const ReplayData *retrySource = nullptr,
      ResultPracticeOptions practiceOptions = {}, bool autoPlayResult = false,
      ResultCourseOptions courseOptions = {}, std::string pacemakerTarget = {},
      std::unique_ptr<bms_parser::Chart> ownedReusableRetryChart = nullptr,
      bms_parser::Chart *reusableRetryChart = nullptr,
      std::optional<ResultPacemakerData> pacemakerOverride = std::nullopt);
  ~ResultScene() override = default;

  void init() override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  void loadDifficultyLabel();
  void loadPreviousBest();
  void saveScore();
  void saveReplay();
  void addRetryButtons();
  void addCourseButtons();
  void buildCourseExitConfirmation();
  void showCourseExitConfirmation();
  void hideCourseExitConfirmation();
  void recordCourseStageRestTime();
  void startRetry(bool samePattern);
  void startReplay();
  void startCourseReplay();
  void startCourseReplayStage(std::shared_ptr<CoursePlaySession> session);
  void continueCourse();
  void showCourseResult();
  void exportPhoto();
  void exitResult();
  [[nodiscard]] bool isCourseStageResult() const;
  [[nodiscard]] bool isCourseFinalResult() const;
  [[nodiscard]] std::optional<ResultPacemakerData>
  pacemakerDataForCurrentResult() const;

  bms_parser::ChartMeta meta;
  RhythmState resultState;
  const ScoreProvenance attemptProvenance;
  std::optional<ReplayData> replayToSave;
  std::optional<ReplayData> retryData;
  std::optional<ResultPreviousBestData> previousBest;
  ResultPracticeOptions practiceOptions;
  ResultCourseOptions courseOptions;
  std::unique_ptr<bms_parser::Chart> ownedReusableRetryChart;
  bms_parser::Chart *reusableRetryChart = nullptr;
  std::string pacemakerTarget;
  std::optional<ResultPacemakerData> pacemakerOverride;
  std::string playModeLabel;
  std::string laneOrderLabel;
  std::string difficultyLabel;
  std::optional<std::string> headerDifficultyLabelOverride;
  std::optional<std::string> currentClearLabelOverride;
  std::optional<int> currentClearRankOverride;
  View *rootLayout = nullptr;
  View *graphPlaceHolder = nullptr;
  View *courseExitConfirmation = nullptr;
  Button *exportPhotoButton = nullptr;
  TextView *exportPhotoButtonText = nullptr;
  std::unique_ptr<ISkin> skin;
  bool shouldSaveScore = true;
  bool replayResult = false;
  bool autoPlayResult = false;
  bool scoreSaved = false;
  bool replaySaved = false;
  bool previousBestLoaded = false;
  bool resultPhotoExportInProgress = false;
  bool courseTransitionStarted = false;
  bool courseStageRestRecorded = false;
  long long courseStageResultShownMicros = 0;
};
