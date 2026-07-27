#pragma once
#include "../ReplayData.h"
#include "../ResultPersistenceCoordinator.h"
#include "../ir/IrSubmission.h"
#include "../ir/IrResultPresentation.h"
#include "../ir/IrRankingModal.h"
#include "ResultPresentationModel.h"
#include "../practice/PracticeLaunchRequest.h"
#include "../practice/PracticeResultModel.h"
#include "../practice/PracticeSession.h"
#include "Scene.h"
#include "play/RhythmState.h"
#include "../bms_parser.hpp"
#include "../skin/ISkin.h"
#include "../skin/SkinTypes.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

struct CoursePlaySession;

struct ResultPracticeOptions {
  bool enabled = false;
  std::shared_ptr<practice::Session> session = nullptr;
  unsigned long long startPosition = 0;
  bool autoKeySound = false;
  bool autoPlay = false;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
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

[[nodiscard]] inline std::shared_ptr<practice::Session>
freshPracticeSessionForRetry(
    const std::shared_ptr<practice::Session> &session) {
  return session == nullptr
             ? nullptr
             : std::make_shared<practice::Session>(session->configuration());
}

enum class ResultCourseMode {
  None,
  Stage,
  CourseResult,
};

struct ResultCourseOptions {
  ResultCourseMode mode = ResultCourseMode::None;
  std::shared_ptr<CoursePlaySession> session = nullptr;
  bool savedResultBrowsing = false;
};

struct ResultPersistenceOptions {
  std::shared_ptr<const result_persistence::ChartResultAttempt> attempt;
  std::shared_ptr<const ir::IrSubmission> irSubmission;
  result_persistence::SaveOutcome outcome;
};

struct ResultRemoteOptions {
  ir::IrRemoteScore score;
  std::optional<ir::IrChartQuery> rankingQuery;
  std::string providerId;
  std::string serverOrigin;
};

struct ResultSceneActionAvailability {
  bool back = false;
  bool rankings = false;
  bool exportPhoto = false;
  bool readOnlyIrUploaded = false;
  bool persistence = false;
  bool retry = false;
  bool retrySame = false;
  bool replay = false;
  bool practice = false;
  bool course = false;
  bool irSubmit = false;
  bool irRetry = false;
};

[[nodiscard]] inline ResultSceneActionAvailability
remoteResultSceneActions(bool rankingsAvailable) noexcept {
  return {.back = true,
          .rankings = rankingsAvailable,
          .exportPhoto = true,
          .readOnlyIrUploaded = true};
}

namespace result_scene_detail {
[[nodiscard]] inline bool isLowerHexDigest(std::string_view value,
                                           std::size_t size) noexcept {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}
} // namespace result_scene_detail

[[nodiscard]] inline std::optional<ir::IrChartQuery>
makeRemoteResultRankingQuery(const ir::IrRemoteScore &score) noexcept {
  try {
    std::string diagnostic;
    if (!ir::validateIrRemoteScore(score, diagnostic) ||
        (score.game != "bms-7k" && score.game != "bms-14k") ||
        score.noteCount <= 0 ||
        !result_scene_detail::isLowerHexDigest(score.chartSha256, 64) ||
        (!score.chartMd5.empty() &&
         !result_scene_detail::isLowerHexDigest(score.chartMd5, 32))) {
      return std::nullopt;
    }
    return ir::IrChartQuery{
        .keyMode = score.game == "bms-14k" ? 14 : 7,
        .chartMd5 = score.chartMd5,
        .chartSha256 = score.chartSha256,
        .totalNotes = score.noteCount,
    };
  } catch (...) {
    return std::nullopt;
  }
}

struct LocalResultSource {
  bms_parser::ChartMeta meta;
  RhythmState resultState;
  ScoreProvenance attemptProvenance;
  std::optional<ReplayData> presentationReplay;
  std::optional<ReplayData> retryData;
  std::optional<ReplayData> analyticsData;
  std::optional<ResultPreviousBestData> previousBest;
  ResultPersistenceOptions persistenceOptions;
  ResultPracticeOptions practiceOptions;
  ResultCourseOptions courseOptions;
  std::unique_ptr<bms_parser::Chart> ownedReusableRetryChart;
  bms_parser::Chart *reusableRetryChart = nullptr;
  std::string pacemakerTarget;
  std::optional<ResultPacemakerData> pacemakerOverride;
  std::optional<std::string> modernReplayAttemptId;
  std::string playModeLabel;
  std::string laneOrderLabel;
  std::string difficultyLabel;
  std::optional<std::string> headerDifficultyLabelOverride;
  std::optional<std::string> currentClearLabelOverride;
  std::optional<int> currentClearRankOverride;
  ResultPresentationModel presentation;
  bool replayResult = false;
  bool retrySameAllowed = true;
  bool autoPlayResult = false;
  bool previousBestLoaded = false;
  bool persistenceContinueChosen = false;
  bool courseTransitionStarted = false;
  bool courseStageRestRecorded = false;
  long long courseStageResultShownMicros = 0;
  std::uint64_t irObservedSnapshotRevision = 0;
  bool irObservedSnapshotInitialized = false;
  std::string irActionDiagnostic;
};

struct RemoteResultSource {
  const ir::IrRemoteScore score;
  const std::optional<ir::IrChartQuery> rankingQuery;
  const std::string providerId;
  const std::string serverOrigin;
  const ResultPresentationModel presentation;
};

[[nodiscard]] inline RemoteResultSource
makeResultRemoteSource(ResultRemoteOptions remote) {
  std::string diagnostic;
  if (!ir::validateIrRemoteScore(remote.score, diagnostic)) {
    throw std::invalid_argument(
        diagnostic.empty() ? "remote result score is invalid" : diagnostic);
  }
  const auto normalizedOrigin = ir::normalizeServerOrigin(remote.serverOrigin);
  if (remote.providerId != ir::kTachiProviderId || !normalizedOrigin ||
      *normalizedOrigin != remote.serverOrigin) {
    throw std::invalid_argument("remote result origin identity is invalid");
  }
  const auto exactRankingQuery = makeRemoteResultRankingQuery(remote.score);
  if (remote.rankingQuery != exactRankingQuery) {
    throw std::invalid_argument("remote result ranking identity is invalid");
  }
  ResultPresentationModel presentation =
      makeRemoteResultPresentation(remote.score);
  return {.score = std::move(remote.score),
          .rankingQuery = std::move(remote.rankingQuery),
          .providerId = std::move(remote.providerId),
          .serverOrigin = std::move(remote.serverOrigin),
          .presentation = std::move(presentation)};
}

class TextView;
class Button;
class BlockingOverlayView;
class PracticeAnalyticsView;
class OverlayPortal;

class ResultScene : public Scene {
public:
  ResultScene(
      ApplicationContext &context, const bms_parser::ChartMeta &meta,
      const RhythmState &state, const ScoreProvenance &attemptProvenance,
      const ReplayData *replay = nullptr,
      ResultPersistenceOptions persistenceOptions = {},
      const ReplayData *retrySource = nullptr,
      ResultPracticeOptions practiceOptions = {}, bool autoPlayResult = false,
      ResultCourseOptions courseOptions = {}, std::string pacemakerTarget = {},
      std::unique_ptr<bms_parser::Chart> ownedReusableRetryChart = nullptr,
      bms_parser::Chart *reusableRetryChart = nullptr,
      std::optional<ResultPacemakerData> pacemakerOverride = std::nullopt,
      const ReplayData *analyticsSource = nullptr,
      std::optional<std::string> modernReplayAttemptId = std::nullopt,
      bool retrySameAllowed = true);
  ResultScene(ApplicationContext &context, ResultRemoteOptions remote);
  ~ResultScene() override = default;

  void init() override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  void loadDifficultyLabel();
  void loadPreviousBest();
  void saveCourseScore();
  void saveCourseReplay();
  bool persistModernCourseResult();
  void addResultPersistenceStatus();
  void retryResultPersistence();
  void continueWithoutSaving();
  void applyResultPersistenceReceipt();
  void updateResultPersistencePresentation();
  void addIrResultStatus();
  void updateIrResultPresentation(bool force = false);
  void submitIrResult();
  void retryIrResult();
  void openRankings();
  void refreshRankingsButton();
  [[nodiscard]] bool rankingsAvailable() const;
  [[nodiscard]] ir::IrResultPresentation makeIrResultPresentation() const;
  void refreshResultSummary();
  void addTimingAnalytics(std::optional<practice::ResultModel> analyticsModel);
  void addRetryButtons();
  void addRemoteButtons();
  void addRemoteIrStatus();
  void addCourseButtons();
  void buildCourseExitConfirmation();
  void showCourseExitConfirmation();
  void hideCourseExitConfirmation();
  [[nodiscard]] bool recordCourseStageRestTime();
  void startRetry(bool samePattern);
  void startReplay();
  void practiceThisSection();
  void updatePracticeSectionAction();
  void startCourseReplay();
  void startCourseReplayStage(std::shared_ptr<CoursePlaySession> session);
  void startModernCourseRetrySame();
  void startModernCourseRetrySameStage(
      std::shared_ptr<CoursePlaySession> session);
  void continueCourse();
  void showSavedCourseStage();
  void showCourseResult();
  void exportPhoto();
  void exitResult();
  [[nodiscard]] bool isCourseStageResult() const;
  [[nodiscard]] bool isCourseFinalResult() const;
  [[nodiscard]] bool persistenceDecisionRequired() const;
  [[nodiscard]] ResultSkinData makeResultSkinData() const;
  [[nodiscard]] std::optional<ResultPacemakerData>
  pacemakerDataForCurrentResult() const;
  [[nodiscard]] std::optional<practice::ResultModel>
  makeTimingAnalyticsModel() const;
  [[nodiscard]] std::optional<practice::LaunchRequest>
  selectedPracticeLaunchRequest() const;
  [[nodiscard]] practice::LaunchRequest
  makePracticeLaunchRequest(long long startMicros, long long endMicros) const;
  [[nodiscard]] LocalResultSource *localSource() noexcept;
  [[nodiscard]] const LocalResultSource *localSource() const noexcept;
  [[nodiscard]] RemoteResultSource *remoteSource() noexcept;
  [[nodiscard]] const RemoteResultSource *remoteSource() const noexcept;
  void rebuildLocalPresentation(
      std::optional<practice::ResultModel> analyticsModel = std::nullopt);

  std::variant<LocalResultSource, RemoteResultSource> source;
  View *rootLayout = nullptr;
  View *graphPlaceHolder = nullptr;
  View *normalResultActions = nullptr;
  View *resultPersistenceStatus = nullptr;
  TextView *persistenceStatusMessage = nullptr;
  Button *persistenceRetryButton = nullptr;
  Button *persistenceDetailsButton = nullptr;
  BlockingOverlayView *persistenceDetailsModalRoot = nullptr;
  TextView *persistenceDetailsStateText = nullptr;
  TextView *persistenceDetailsReasonText = nullptr;
  TextView *persistenceDetailsReferenceText = nullptr;
  View *irResultStatus = nullptr;
  TextView *irResultStatusText = nullptr;
  TextView *irResultDetailText = nullptr;
  Button *irResultSubmitButton = nullptr;
  Button *irResultRetryButton = nullptr;
  OverlayPortal *rankingOverlayPortal = nullptr;
  Button *rankingsButton = nullptr;
  std::unique_ptr<ir::IrRankingModal> rankingsModal;
  PracticeAnalyticsView *timingAnalyticsView = nullptr;
  View *courseExitConfirmation = nullptr;
  Button *exportPhotoButton = nullptr;
  TextView *exportPhotoButtonText = nullptr;
  Button *practiceSectionButton = nullptr;
  TextView *practiceSectionButtonText = nullptr;
  std::unique_ptr<ISkin> skin;
  bool resultPhotoExportInProgress = false;
};
