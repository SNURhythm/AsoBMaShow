#pragma once
#include "../analysis/JudgedPlaybackData.h"
#include "../analysis/JudgedPlaybackContext.h"
#include "../PlayOptionUtils.h"
#include "../ResultPersistenceCoordinator.h"
#include "../ir/IrSubmission.h"
#include "../ir/IrSubmissionSnapshot.h"
#include "../ir/IrResultPresentation.h"
#include "../ir/IrRankingModal.h"
#include "ResultPresentationModel.h"
#include "../practice/PracticeLaunchRequest.h"
#include "../practice/PracticeResultModel.h"
#include "../practice/PracticeSession.h"
#include "Scene.h"
#include "play/RhythmState.h"
#include "play/ReplayResultContext.h"
#include "../bms_parser.hpp"
#include "../skin/ISkin.h"
#include "../skin/SkinTypes.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
  replay::DoublePlayOption doublePlayOption = replay::DoublePlayOption::Normal;
  int longNoteMode = 0;
  std::string assistOption = assist_options::kOff;
  unsigned long long leadInMicros = 0;
  Scene *returnScene = nullptr;
  std::function<void(const JudgedPlaybackData &)> practiceGhostCallback;
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
  std::shared_ptr<const result_persistence::CompletedChartAttempt> attempt;
  std::shared_ptr<const result_persistence::CompletedCourseAttempt>
      courseAttempt;
  std::shared_ptr<const result_persistence::PersistedChartResult> result;
  std::shared_ptr<const ir::IrSubmissionSnapshot> irSnapshot;
  std::shared_ptr<const ir::IrSubmission> irSubmission;
  result_persistence::SaveOutcome outcome;
  std::optional<std::string> previousBestBeforeCreatedAt;
};

using CoursePersistenceCallback = std::function<result_persistence::SaveOutcome(
    const result_persistence::CompletedCourseAttempt &)>;

struct ResultPersistenceRetryCallbacks {
  std::function<result_persistence::SaveOutcome(
      const result_persistence::CompletedChartAttempt &,
      std::span<const ir::IrOutboxDraft>)>
      persistChart;
  CoursePersistenceCallback persistCourse;
};

using ResultPreviousBestQuery = ReplayComparisonQuery;

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
[[nodiscard]] inline play_options::PlayModeDisplayLabel
selectCoursePlayModeDisplayLabel(
    const play_options::PlayModeDisplayLabel &retainedStageDisplay,
    play_options::PlayModeDisplayLabel sessionDisplay,
    bool courseReplayPlayback) {
  if (courseReplayPlayback) {
    return retainedStageDisplay;
  }
  if (sessionDisplay.mode.empty()) {
    sessionDisplay.mode = "COURSE";
  }
  return sessionDisplay;
}

[[nodiscard]] inline bool
cleanupAllowsContinueWithoutSaving(bool cleanupRequired, bool retryAvailable,
                                   const std::function<bool()> &cleanup) {
  if (!cleanupRequired || (cleanup && cleanup())) {
    return true;
  }
  return !retryAvailable;
}

[[nodiscard]] inline bool
hasPersistenceAttempt(const ResultPersistenceOptions &persistence) noexcept {
  return (persistence.attempt != nullptr) !=
         (persistence.courseAttempt != nullptr);
}

[[nodiscard]] inline std::string_view
persistenceAttemptId(const ResultPersistenceOptions &persistence) noexcept {
  if (!hasPersistenceAttempt(persistence)) {
    return {};
  }
  const auto &attemptId = persistence.attempt != nullptr
                              ? persistence.attempt->result.attemptId
                              : persistence.courseAttempt->result.attemptId;
  return attemptId ? std::string_view(*attemptId) : std::string_view{};
}

[[nodiscard]] inline bool
persistCourseAttempt(ResultPersistenceOptions &persistence,
                     result_persistence::CompletedCourseAttempt attempt,
                     const CoursePersistenceCallback &persist) {
  if (persistence.attempt != nullptr || persistence.courseAttempt != nullptr ||
      !persist) {
    return false;
  }
  persistence.courseAttempt =
      std::make_shared<const result_persistence::CompletedCourseAttempt>(
          std::move(attempt));
  persistence.outcome = persist(*persistence.courseAttempt);
  return true;
}

[[nodiscard]] inline bool
retryPersistenceAttempt(ResultPersistenceOptions &persistence,
                        std::span<const ir::IrOutboxDraft> automaticDrafts,
                        const ResultPersistenceRetryCallbacks &callbacks) {
  if (!hasPersistenceAttempt(persistence)) {
    return false;
  }
  if (persistence.courseAttempt != nullptr) {
    if (!callbacks.persistCourse) {
      return false;
    }
    persistence.outcome = callbacks.persistCourse(*persistence.courseAttempt);
    return true;
  }
  if (!callbacks.persistChart) {
    return false;
  }
  persistence.outcome =
      callbacks.persistChart(*persistence.attempt, automaticDrafts);
  return true;
}

[[nodiscard]] inline bool isLowerHexDigest(std::string_view value,
                                           std::size_t size) noexcept {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] inline ResultPreviousBestQuery
previousBestQueryFor(const ResultPersistenceOptions &persistence,
                     bool replayResult,
                     const ReplayResultContext *replayContext) {
  ResultPreviousBestQuery query;
  if (persistence.attempt != nullptr &&
      persistence.attempt->result.attemptId.has_value()) {
    query.excludeAttemptId = persistence.attempt->result.attemptId;
    return query;
  }
  if (persistence.result != nullptr &&
      persistence.result->attemptId.has_value()) {
    query.excludeAttemptId = persistence.result->attemptId;
    return query;
  }
  if (persistence.previousBestBeforeCreatedAt.has_value() &&
      !persistence.previousBestBeforeCreatedAt->empty()) {
    query.beforeCreatedAt = persistence.previousBestBeforeCreatedAt;
    return query;
  }
  if (replayResult && replayContext != nullptr) {
    return replayComparisonQueryFor(replayContext);
  }
  return query;
}

[[nodiscard]] inline std::optional<int>
resultIdForPractice(const ResultPersistenceOptions &persistence,
                    const ReplayResultContext *replayContext) noexcept {
  if (persistence.result != nullptr && persistence.result->resultId > 0) {
    return persistence.result->resultId;
  }
  return replayContext != nullptr && replayContext->resultId > 0
             ? std::optional<int>(replayContext->resultId)
             : std::nullopt;
}

[[nodiscard]] inline bool
isReplayResultSource(const JudgedPlaybackData *presentationReplay,
                     const JudgedPlaybackData *judgedReplay,
                     const replay::ReplayPlaybackData *rawReplay) noexcept {
  return presentationReplay == nullptr &&
         (judgedReplay != nullptr || rawReplay != nullptr);
}

[[nodiscard]] inline std::optional<JudgedPlaybackData>
retrySourceForLocalResult(const bms_parser::ChartMeta &meta,
                          const ScoreProvenance &attemptProvenance,
                          const JudgedPlaybackData *presentationReplay,
                          const JudgedPlaybackData *explicitRetrySource,
                          const replay::ReplayPlaybackData *rawReplayPlayback) {
  if (explicitRetrySource != nullptr) {
    return *explicitRetrySource;
  }
  if (presentationReplay != nullptr) {
    return *presentationReplay;
  }
  if (rawReplayPlayback != nullptr) {
    JudgedPlaybackData result =
        analysis::retrySourceFromProvenance(meta, attemptProvenance);
    result.setup = rawReplayPlayback->setup;
    result.chartMeta.RandomSeed = result.setup.randomSeed;
    result.chartMeta.RandomPrng = result.setup.randomPrng;
    result.chartMeta.RandomValues = result.setup.randomValues;
    return result;
  }
  return analysis::retrySourceFromProvenance(meta, attemptProvenance);
}

[[nodiscard]] inline bool
shouldReuseResultRetryChart(bool samePattern, const bms_parser::Chart *chart,
                            bool chartMatchesRetrySource) noexcept {
  return samePattern && chart != nullptr && chartMatchesRetrySource;
}

[[nodiscard]] inline bool retrySourceProvidesTimingAnalytics(
    const JudgedPlaybackData &retrySource) noexcept {
  return !retrySource.events.empty();
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
  std::optional<JudgedPlaybackData> presentationReplay;
  std::optional<JudgedPlaybackData> retryData;
  std::shared_ptr<const replay::ReplayPlaybackData> rawReplayPlayback;
  std::optional<ReplayResultContext> replayResultContext;
  std::optional<JudgedPlaybackData> analyticsData;
  std::optional<ResultPreviousBestData> previousBest;
  std::optional<int> persistedResultId;
  ResultPersistenceOptions persistenceOptions;
  ResultPracticeOptions practiceOptions;
  ResultCourseOptions courseOptions;
  std::unique_ptr<bms_parser::Chart> ownedReusableRetryChart;
  bool reusableRetryChartMatchesRetrySource = true;
  bms_parser::Chart *reusableRetryChart = nullptr;
  std::string pacemakerTarget;
  std::optional<ResultPacemakerData> pacemakerOverride;
  std::string playModeLabel;
  std::string laneOrderLabel;
  std::string difficultyLabel;
  std::optional<std::string> headerDifficultyLabelOverride;
  std::optional<std::string> currentClearLabelOverride;
  std::optional<int> currentClearRankOverride;
  ResultPresentationModel presentation;
  bool replayResult = false;
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
      const JudgedPlaybackData *replay = nullptr,
      ResultPersistenceOptions persistenceOptions = {},
      const JudgedPlaybackData *retrySource = nullptr,
      ResultPracticeOptions practiceOptions = {}, bool autoPlayResult = false,
      ResultCourseOptions courseOptions = {}, std::string pacemakerTarget = {},
      std::unique_ptr<bms_parser::Chart> ownedReusableRetryChart = nullptr,
      bool reusableRetryChartMatchesRetrySource = true,
      bms_parser::Chart *reusableRetryChart = nullptr,
      std::optional<ResultPacemakerData> pacemakerOverride = std::nullopt,
      const JudgedPlaybackData *analyticsSource = nullptr,
      std::shared_ptr<const replay::ReplayPlaybackData> rawReplayPlayback =
          nullptr,
      std::optional<ReplayResultContext> replayResultContext = std::nullopt);
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
  void recordCourseStageRestTime();
  void startRetry(bool samePattern);
  void startReplay();
  void practiceThisSection();
  void updatePracticeSectionAction();
  void startCourseReplay();
  void startCourseReplayStage(std::shared_ptr<CoursePlaySession> session);
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
