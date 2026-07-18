#include "ResultScene.h"
#include "../CourseConstraintUtils.h"
#include "../CourseIdentity.h"
#include "../CoursePlaySession.h"
#include "../PlayOptionUtils.h"
#include "../repositories/ReplayRepository.h"
#include "../ResultImageExporter.h"
#include "../ResultPresentationUtils.h"
#include "../repositories/ScoreRepository.h"
#include "../path.h"
#include "../practice/PracticeLaunchRequest.h"
#include "../practice/PracticeResultModel.h"
#include "../view/Button.h"
#include "../view/OverlayPortal.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"
#include "play/GamePlayScene.h"
#include "play/Pacemaker.h"
#include "ChartViewerScene.h"
#include "PracticeAnalyticsPresentation.h"
#include "PracticeAnalyticsView.h"

#include "../rendering/Color.h"
#include "../rendering/SimpleBatchRenderer.h"
#include "../rendering/common.h"
#include "bgfx/bgfx.h"
#include "../skin/DefaultSkin.h"
#include "../skin/SkinTypes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace {
long long nowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

Color resultGaugeLineColor(float value) {
  if (value > 80.0f) {
    return ui_theme::withAlpha(ui_theme::cyan(), 210);
  }
  if (value > 30.0f) {
    return ui_theme::withAlpha(ui_theme::lime(), 210);
  }
  return ui_theme::withAlpha(ui_theme::coral(), 210);
}

void drawResultGaugeLineGraph(rendering::SimpleBatchRenderer &batch,
                              const RhythmState &resultState, float x, float y,
                              float w, float h) {
  batch.addRect(x, y, w, h, ui_theme::resultPanelSubtle().toABGR());

  const float padding = 8.0f;
  const float graphX = x + padding;
  const float graphY = y + padding;
  const float graphW = std::max(1.0f, w - padding * 2.0f);
  const float graphH = std::max(1.0f, h - padding * 2.0f);
  const float gaugeMaximum =
      gaugeMaximumValue(resultState.gaugeType, resultState.gaugeProfile);
  auto clampedValue = [gaugeMaximum](float value) {
    return std::clamp(value, 0.0f, gaugeMaximum);
  };
  auto valueY = [&](float value) {
    return graphY + graphH - (clampedValue(value) / gaugeMaximum) * graphH;
  };

  const uint32_t guideColor = ui_theme::hairlineSubtle().toABGR();
  batch.addLine(graphX, valueY(80.0f), graphX + graphW, valueY(80.0f), 1.0f,
                guideColor);
  batch.addLine(graphX, valueY(30.0f), graphX + graphW, valueY(30.0f), 1.0f,
                guideColor);

  const size_t count = resultState.gaugeHistory.size();
  if (count == 1) {
    const float value = clampedValue(resultState.gaugeHistory.front());
    batch.addCircle(graphX, valueY(value), 3.5f,
                    resultGaugeLineColor(value).toABGR());
    return;
  }

  for (size_t i = 1; i < count; ++i) {
    const float prevValue = clampedValue(resultState.gaugeHistory[i - 1]);
    const float value = clampedValue(resultState.gaugeHistory[i]);
    const float x0 =
        graphX + (static_cast<float>(i - 1) / static_cast<float>(count - 1)) *
                     graphW;
    const float x1 =
        graphX + (static_cast<float>(i) / static_cast<float>(count - 1)) *
                     graphW;
    batch.addLine(x0, valueY(prevValue), x1, valueY(value), 3.0f,
                  resultGaugeLineColor(value).toABGR());
  }

  const size_t markerStep = std::max<size_t>(1, count / 40);
  for (size_t i = 0; i < count; i += markerStep) {
    const float value = clampedValue(resultState.gaugeHistory[i]);
    const float pointX =
        graphX + (static_cast<float>(i) / static_cast<float>(count - 1)) *
                     graphW;
    batch.addCircle(pointX, valueY(value), 2.5f,
                    resultGaugeLineColor(value).toABGR());
  }
}

play_options::PlayModeDisplayLabel resultPlayModeDisplayLabel(
    const bms_parser::ChartMeta &meta,
    const std::optional<ReplayData> &presentationReplay,
    const std::optional<ReplayData> &retryData,
    const ResultPracticeOptions &practiceOptions) {
  if (practiceOptions.enabled) {
    return play_options::formatPlayModeDisplayLabel(
        meta, practiceOptions.playOption, practiceOptions.playOptionSeed,
        practiceOptions.playOption2, practiceOptions.playOption2Seed);
  }
  if (presentationReplay.has_value()) {
    return play_options::formatPlayModeDisplayLabel(*presentationReplay);
  }
  if (retryData.has_value()) {
    return play_options::formatPlayModeDisplayLabel(*retryData);
  }
  return play_options::formatPlayModeDisplayLabel(meta, std::nullopt);
}

int totalNotesForCourse(const CoursePlaySession &session) {
  int total = 0;
  const size_t count =
      std::max(session.entries.size(), session.completedResults.size());
  for (size_t i = 0; i < count; ++i) {
    if (i < session.completedResults.size()) {
      total += std::max(0, session.completedResults[i].meta.TotalNotes);
    } else {
      total += std::max(0, session.entries[i].meta.TotalNotes);
    }
  }
  return total;
}

long long totalPlayLengthForCourse(const CoursePlaySession &session) {
  long long total = 0;
  for (const auto &entry : session.entries) {
    total += std::max(0LL, entry.meta.PlayLength);
  }
  return total;
}

bms_parser::ChartMeta courseResultMetaForSession(
    const CoursePlaySession &session) {
  return result_presentation::courseResultMeta(
      session.courseName, session.courseGroupName, session.entries.size(),
      totalNotesForCourse(session), totalPlayLengthForCourse(session));
}

void appendMissingCourseGaugeHistory(RhythmState &state,
                                     const CoursePlaySession &session,
                                     std::size_t startIndex) {
  for (std::size_t i = startIndex; i < session.entries.size(); ++i) {
    const long long playLength =
        std::max(0LL, session.entries[i].meta.PlayLength);
    const int samples =
        std::max(1, static_cast<int>((playLength + 500000LL) / 500000LL));
    for (int sample = 0; sample < samples; ++sample) {
      state.gaugeHistory.push_back(0.0f);
    }
  }
}

RhythmState courseResultStateForSession(const CoursePlaySession &session) {
  RhythmState aggregate(nullptr, false);
  aggregate.configureGauge(session.gaugeType, session.gaugeAutoShift,
                           session.gaugeProfile,
                           session.gaugeAutoShiftLowerBound);
  if (session.carriedGauge.has_value()) {
    aggregate.restoreGaugeState(*session.carriedGauge);
  }

  aggregate.resetJudgeCounts();
  aggregate.comboBreak = 0;
  aggregate.maxCombo = session.maxCombo;
  aggregate.fastCount = 0;
  aggregate.slowCount = 0;
  aggregate.gaugeHistory.clear();

  for (const auto &result : session.completedResults) {
    for (int i = 0; i < JudgementCount; ++i) {
      aggregate.addJudgeCountFrom(result.state, static_cast<Judgement>(i));
    }
    aggregate.comboBreak += result.state.comboBreak;
    aggregate.fastCount += result.state.fastCount;
    aggregate.slowCount += result.state.slowCount;
    aggregate.gaugeHistory.insert(aggregate.gaugeHistory.end(),
                                  result.state.gaugeHistory.begin(),
                                  result.state.gaugeHistory.end());
  }
  appendMissingCourseGaugeHistory(aggregate, session,
                                  session.completedResults.size());

  if (!session.completedResults.empty()) {
    aggregate.combo = session.carriedCombo;
  }
  if (session.completedResults.size() < session.entries.size()) {
    aggregate.currentGauge = 0.0f;
    aggregate.gaugeValues[gaugeTypeIndex(aggregate.gaugeType)] = 0.0f;
    aggregate.gaugeSurvivalFailed[gaugeTypeIndex(aggregate.gaugeType)] = true;
  }
  return aggregate;
}

std::optional<CourseReplayData>
courseReplayDataForSession(const CoursePlaySession &session,
                           const RhythmState &resultState,
                           const ScoreProvenance &provenance) {
  const std::size_t completedCharts = session.completedResults.size();
  const std::size_t totalCharts = session.entries.size();
  if (completedCharts == 0 || completedCharts > totalCharts ||
      totalCharts > static_cast<std::size_t>(
                        replay_summary_scan::kMaxCourseStagesPerCandidate)) {
    return std::nullopt;
  }
  std::vector<bms_parser::ChartMeta> expectedMetas;
  expectedMetas.reserve(completedCharts);
  for (std::size_t index = 0; index < completedCharts; ++index) {
    expectedMetas.push_back(session.entries[index].meta);
  }
  auto preparedStages = course_replay::prepareCompletedPrefixForSave(
      session.replayStages, expectedMetas, completedCharts);
  if (!preparedStages.has_value()) {
    return std::nullopt;
  }

  CourseReplayData replay;
  replay.courseId = session.courseId;
  replay.courseKey = session.courseKey;
  if (replay.courseKey.empty()) {
    replay.courseKey = course_identity::makeCourseKey(session);
  }
  if (replay.courseKey.empty()) {
    return std::nullopt;
  }
  replay.courseName = session.courseName;
  replay.courseGroupName = session.courseGroupName;
  replay.constraintJson = session.constraintJson;
  replay.requestedPlayOption = session.requestedPlayOption;
  replay.assistOption = assist_options::normalize(session.assistOption);
  replay.initialGaugeType = session.gaugeType;
  replay.gaugeProfile = session.gaugeProfile;
  replay.gaugeAutoShift = session.gaugeAutoShift;
  replay.gaugeAutoShiftLowerBound = session.gaugeAutoShiftLowerBound;
  replay.longNoteMode = normalizeChartLongNoteModeValue(session.longNoteMode);
  replay.finalScore = resultState.getScore();
  replay.maxCombo = session.maxCombo;
  replay.finalGauge = resultState.currentGauge;
  replay.clearType = resultState.getClearTypeRank();
  replay.completedCharts = static_cast<int>(completedCharts);
  replay.totalCharts = static_cast<int>(totalCharts);
  replay.provenance = provenance;
  const bms_parser::ChartMeta courseMeta = courseResultMetaForSession(session);
  const bool fullCombo = result_presentation::isFullComboCourseResult(
      replay.completedCharts, replay.totalCharts, session.entries.size(),
      resultState, courseMeta);
  replay.clearType = clear_policy::fullComboRankForPlayback(
      replay.clearType, fullCombo, provenance.playback);

  replay.stages = std::move(*preparedStages);
  return replay;
}
} // namespace

ResultScene::ResultScene(
    ApplicationContext &context, const bms_parser::ChartMeta &meta,
    const RhythmState &state, const ScoreProvenance &attemptProvenance,
    const ReplayData *replay, ResultPersistenceOptions persistenceOptions,
    const ReplayData *retrySource, ResultPracticeOptions practiceOptions,
    bool autoPlayResult, ResultCourseOptions courseOptions,
    std::string pacemakerTarget,
    std::unique_ptr<bms_parser::Chart> ownedReusableRetryChart,
    bms_parser::Chart *reusableRetryChart,
    std::optional<ResultPacemakerData> pacemakerOverride,
    const ReplayData *analyticsSource)
    : Scene(context), meta(meta), resultState(state),
      attemptProvenance(attemptProvenance),
      presentationReplay(replay != nullptr ? std::optional<ReplayData>(*replay)
                                           : std::nullopt),
      retryData(retrySource != nullptr
                    ? std::optional<ReplayData>(*retrySource)
                    : (replay != nullptr ? std::optional<ReplayData>(*replay)
                                         : std::nullopt)),
      analyticsData(analyticsSource != nullptr
                        ? std::optional<ReplayData>(*analyticsSource)
                        : std::nullopt),
      persistenceOptions(std::move(persistenceOptions)),
      practiceOptions(std::move(practiceOptions)),
      courseOptions(std::move(courseOptions)),
      ownedReusableRetryChart(std::move(ownedReusableRetryChart)),
      reusableRetryChart(this->ownedReusableRetryChart != nullptr
                             ? this->ownedReusableRetryChart.get()
                             : reusableRetryChart),
      pacemakerTarget(pacemaker::normalizeTargetId(
          pacemakerTarget.empty() ? context.settings.selectedPacemakerTarget
                                  : pacemakerTarget)),
      pacemakerOverride(std::move(pacemakerOverride)),
      replayResult(replay == nullptr && retrySource != nullptr &&
                   !this->practiceOptions.enabled &&
                   this->courseOptions.mode == ResultCourseMode::None),
      autoPlayResult(autoPlayResult ||
                     (retrySource != nullptr && retrySource->autoPlay)) {
  applyResultPersistenceReceipt();
  const play_options::PlayModeDisplayLabel display = resultPlayModeDisplayLabel(
      this->meta, presentationReplay, retryData, this->practiceOptions);
  playModeLabel = display.mode;
  laneOrderLabel = display.laneOrder;
  if (this->courseOptions.session != nullptr) {
    const auto courseDisplay = play_options::formatPlayModeDisplayLabel(
        this->meta, this->courseOptions.session->playOption,
        this->courseOptions.session->playOptionSeed,
        this->courseOptions.session->playOption2,
        this->courseOptions.session->playOption2Seed);
    playModeLabel = courseDisplay.mode.empty() ? "COURSE" : courseDisplay.mode;
    laneOrderLabel = courseDisplay.laneOrder;
  }
  if (isCourseStageResult()) {
    currentClearLabelOverride = "NO PLAY";
    currentClearRankOverride = kNoClearTypeRank;
  } else if (isCourseFinalResult()) {
    headerDifficultyLabelOverride = "COURSE";
    const auto &session = *this->courseOptions.session;
    const bms_parser::ChartMeta courseMeta = courseResultMetaForSession(session);
    const bool fullCombo = result_presentation::isFullComboCourseResult(
        static_cast<int>(session.completedResults.size()),
        static_cast<int>(session.entries.size()), session.entries.size(),
        resultState, courseMeta);
    if (fullCombo) {
      const int clearRank = clear_policy::fullComboRankForPlayback(
          resultState.getClearTypeRank(), true, attemptProvenance.playback);
      currentClearLabelOverride = clearTypeRankToLabel(clearRank);
      currentClearRankOverride = clearRank;
    }
  }
  skin = std::make_unique<DefaultSkin>();
}

bool ResultScene::isCourseStageResult() const {
  return courseOptions.mode == ResultCourseMode::Stage &&
         courseOptions.session != nullptr;
}

bool ResultScene::isCourseFinalResult() const {
  return courseOptions.mode == ResultCourseMode::CourseResult &&
         courseOptions.session != nullptr;
}

std::optional<ResultPacemakerData>
ResultScene::pacemakerDataForCurrentResult() const {
  if (isCourseStageResult() || isCourseFinalResult()) {
    return std::nullopt;
  }
  if (pacemakerOverride.has_value()) {
    return pacemakerOverride;
  }
  return result_presentation::pacemakerDataForResult(
      meta, resultState, pacemakerTarget, previousBest);
}

ResultSkinData ResultScene::makeResultSkinData() const {
  ResultSkinData data = {&resultState, &meta, &context};
  data.playModeLabel = playModeLabel;
  data.laneOrderLabel = laneOrderLabel;
  data.difficultyLabel = difficultyLabel;
  data.headerDifficultyLabelOverride = headerDifficultyLabelOverride;
  if (autoPlayResult) {
    data.currentClearLabelOverride = "AUTO PLAY";
  }
  if (currentClearLabelOverride.has_value()) {
    data.currentClearLabelOverride = currentClearLabelOverride;
  }
  data.currentClearRankOverride = currentClearRankOverride;
  data.previousBest = previousBest;
  data.pacemaker = pacemakerDataForCurrentResult();
  return data;
}

void ResultScene::saveCourseScore() {
  if (!isCourseFinalResult() || courseOptions.session == nullptr ||
      courseOptions.session->courseReplayPlayback ||
      courseOptions.session->courseScoreSaved) {
    return;
  }

  const int completedCharts =
      static_cast<int>(courseOptions.session->completedResults.size());
  const int totalCharts =
      static_cast<int>(courseOptions.session->entries.size());
  if (context.scoreRepository.SaveCourseScore(
          *courseOptions.session, resultState, completedCharts, totalCharts,
          attemptProvenance)) {
    courseOptions.session->courseScoreSaved = true;
  } else {
    SDL_Log("Failed to save course score: %s",
            courseOptions.session->courseName.c_str());
  }
}

void ResultScene::loadPreviousBest() {
  if (previousBestLoaded) {
    return;
  }
  previousBestLoaded = true;
  previousBest.reset();

  std::optional<std::string> beforeCreatedAt;
  std::optional<std::string> excludeAttemptId;
  const auto *receipt =
      persistenceOptions.attempt == nullptr
          ? nullptr
          : persistenceOptions.outcome.validatedReceiptFor(
                *persistenceOptions.attempt);
  if (receipt != nullptr) {
    excludeAttemptId = persistenceOptions.attempt->attemptId;
  } else if (replayResult && retryData.has_value() && !retryData->autoPlay &&
             !retryData->createdAt.empty()) {
    beforeCreatedAt = retryData->createdAt;
  }

  const auto best = isCourseFinalResult()
                        ? context.scoreRepository.LoadBestCourseScore(
                              *courseOptions.session)
                        : context.scoreRepository.LoadBestScore(
                              meta, beforeCreatedAt, excludeAttemptId);
  if (best.has_value()) {
    previousBest = result_presentation::previousBestDataFromSnapshot(*best);
  }
}

void ResultScene::loadDifficultyLabel() {
  if (isCourseFinalResult()) {
    difficultyLabel = "Course";
    return;
  }
  difficultyLabel =
      result_presentation::difficultyLabelForChart(context.chartRepository,
                                                    meta);
}

void ResultScene::saveCourseReplay() {
  auto session = courseOptions.session;
  if (!isCourseFinalResult() || session == nullptr ||
      session->courseReplayPlayback || session->courseReplaySaved) {
    return;
  }

  auto pendingCourseReplay =
      courseReplayDataForSession(*session, resultState, attemptProvenance);
  if (!pendingCourseReplay.has_value()) {
    SDL_Log("Refusing incomplete or non-contiguous course replay: %s",
            session->courseName.c_str());
    return;
  }
  auto courseReplay =
      std::make_shared<CourseReplayData>(std::move(*pendingCourseReplay));

  auto replayId = context.replayRepository.SaveCourseReplay(*courseReplay);
  if (!replayId.has_value()) {
    SDL_Log("Failed to save course replay: %s", session->courseName.c_str());
    return;
  }

  courseReplay->id = *replayId;
  session->savedCourseReplayId = *replayId;
  session->courseReplaySaved = true;
  session->courseReplayData = std::move(courseReplay);
}

void ResultScene::applyResultPersistenceReceipt() {
  const auto *receipt =
      persistenceOptions.attempt == nullptr
          ? nullptr
          : persistenceOptions.outcome.validatedReceiptFor(
                *persistenceOptions.attempt);
  if (receipt == nullptr) {
    return;
  }

  const auto applyReceipt = [receipt](std::optional<ReplayData> &replay) {
    if (!replay.has_value()) {
      return;
    }
    replay->id = receipt->replayId;
    replay->createdAt = receipt->createdAt;
  };
  applyReceipt(presentationReplay);
  applyReceipt(retryData);
}

bool ResultScene::persistenceDecisionRequired() const {
  return persistenceOptions.outcome.requiresUserDecision(
      persistenceOptions.attempt != nullptr, persistenceContinueChosen);
}

std::optional<practice::ResultModel>
ResultScene::makeTimingAnalyticsModel() const {
  if (reusableRetryChart == nullptr || isCourseStageResult() ||
      isCourseFinalResult()) {
    return std::nullopt;
  }
  std::span<const ReplayData> completedAttempts;
  std::size_t abandonedAttempts = 0;
  std::array<ReplayData, 1> singleAttempt;
  if (practiceOptions.session != nullptr) {
    completedAttempts = practiceOptions.session->completedAttempts();
    abandonedAttempts = practiceOptions.session->abandonedAttemptCount();
  } else if (analyticsData.has_value()) {
    singleAttempt.front() = *analyticsData;
    completedAttempts = singleAttempt;
  } else if (presentationReplay.has_value()) {
    singleAttempt.front() = *presentationReplay;
    singleAttempt.front().autoPlay =
        singleAttempt.front().autoPlay || autoPlayResult;
    completedAttempts = singleAttempt;
  } else if (retryData.has_value()) {
    singleAttempt.front() = *retryData;
    singleAttempt.front().autoPlay =
        singleAttempt.front().autoPlay || autoPlayResult;
    completedAttempts = singleAttempt;
  }
  if (completedAttempts.empty()) {
    return std::nullopt;
  }

  return practice::ResultModel(*reusableRetryChart, completedAttempts,
                               abandonedAttempts);
}

void ResultScene::addTimingAnalytics(
    std::optional<practice::ResultModel> analyticsModel) {
  View *host =
      rootLayout == nullptr
          ? nullptr
          : rootLayout->findViewByName("timingAnalytics");
  if (rootLayout == nullptr || !analyticsModel.has_value()) {
    if (host != nullptr) {
      host->setDisplay(YGDisplayNone);
    }
    return;
  }

  if (host == nullptr) {
    host = new View();
    host->setName("timingAnalytics");
    host->setWidthPercent(100.0f);
    host->setHeight(
        practice_analytics_presentation::kPreferredAnalyticsHeight);
    host->setMinHeight(
        practice_analytics_presentation::kMinimumAnalyticsHeight);
    host->setFlexShrink(1.0f);
    host->setFlexDirection(FlexDirection::Column);
    host->setAlignItems(YGAlignStretch);
    View *actionSibling = nullptr;
    for (View *child : rootLayout->getChildren()) {
      if (child != nullptr &&
          (child->getName() == "resultActions" ||
           child->findViewByName("resultActions") != nullptr)) {
        actionSibling = child;
        break;
      }
    }
    if (actionSibling != nullptr) {
      rootLayout->insertViewBefore(host, actionSibling);
    } else {
      rootLayout->addView(host);
    }
  }

  timingAnalyticsView =
      new PracticeAnalyticsView(std::move(*analyticsModel));
  host->addView(timingAnalyticsView);
}

void ResultScene::addResultPersistenceStatus() {
  if (rootLayout == nullptr) {
    return;
  }

  normalResultActions = rootLayout->findViewByName("resultActions");
  const bool hasPersistenceResult =
      persistenceOptions.attempt != nullptr ||
      !persistenceOptions.outcome.userMessage.empty();
  if (!hasPersistenceResult) {
    return;
  }

  auto *status = new View();
  status->setName("resultPersistenceStatus");
  status->setWidthPercent(100.0f);
  status->setPadding(Edge::All, 18);
  status->setFlexDirection(FlexDirection::Column);
  status->setAlignItems(YGAlignCenter);
  status->setGap(14);
  status->setBackgroundColor(ui_theme::resultPanelStrong());
  status->setCornerRadius(ui_theme::panelRadius());
  status->setBorderColor(ui_theme::withAlpha(ui_theme::coral(), 180));
  status->setBorderWidth(1);
  status->setShadow(ui_theme::cardShadow(), ui_theme::kCardShadow);

  persistenceStatusMessage = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  persistenceStatusMessage->setText(persistenceOptions.outcome.userMessage);
  persistenceStatusMessage->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  persistenceStatusMessage->setAlign(TextView::CENTER);
  persistenceStatusMessage->setWrap(true);
  persistenceStatusMessage->setWidthPercent(100.0f);
  persistenceStatusMessage->setHeight(58);
  status->addView(persistenceStatusMessage);

  auto *actions = new View();
  actions->setFlexDirection(FlexDirection::Row);
  actions->setAlignItems(YGAlignCenter);
  actions->setJustifyContent(YGJustifyCenter);
  actions->setFlexWrap(YGWrapWrap);
  actions->setGap(14);

  const auto makeButton = [](const std::string &label, Color normal,
                             Color hover, Color pressed, Color border,
                             std::function<void()> onClick) {
    auto *button = new Button();
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
    text->setText(label);
    text->setAlign(TextView::CENTER);
    text->setVAlign(TextView::MIDDLE);
    text->setColor(ui_theme::sdl(ui_theme::textOn(normal)));
    button->setContentView(text);
    button->setOnClickListener(std::move(onClick));
    button->setSize(292, 60);
    button->setCornerRadius(ui_theme::controlRadius());
    button->setBackgroundColors(normal, hover, pressed);
    button->setBorderColors(ui_theme::withAlpha(border, 150),
                            ui_theme::withAlpha(border, 190),
                            ui_theme::withAlpha(border, 220));
    button->setStyledBorderWidth(1);
    return button;
  };

  persistenceRetryButton = makeButton(
      "Retry Save", ui_theme::primaryAction(), ui_theme::primaryActionHover(),
      ui_theme::primaryActionPressed(), ui_theme::cyan(),
      [this]() { retryResultPersistence(); });
  persistenceRetryButton->setEnabled(persistenceOptions.attempt != nullptr &&
                                     persistenceOptions.outcome.retryable());
  actions->addView(persistenceRetryButton);
  actions->addView(makeButton(
      "Continue Without Saving", ui_theme::warningAction(),
      ui_theme::warningActionHover(), ui_theme::warningActionPressed(),
      ui_theme::coral(), [this]() { continueWithoutSaving(); }));
  status->addView(actions);

  if (normalResultActions != nullptr) {
    rootLayout->insertViewBefore(status, normalResultActions);
  } else {
    rootLayout->addView(status);
  }
  resultPersistenceStatus = status;
}

ir::IrResultPresentation ResultScene::makeIrResultPresentation() const {
  ir::IrDriverCapabilities capabilities;
  if (const auto driver = context.irDrivers.find(ir::kTachiProviderId)) {
    capabilities = driver->capabilities();
  }
  ir::IrProviderSettings settings;
  if (const auto found = context.settings.irProviders.find(
          std::string(ir::kTachiProviderId));
      found != context.settings.irProviders.end()) {
    settings = found->second;
  }
  ir::IrAttemptStatusSnapshot snapshot;
  if (persistenceOptions.irSubmission && context.irSubmissionService) {
    snapshot = context.irSubmissionService->status(
        ir::kTachiProviderId, persistenceOptions.irSubmission->attemptId);
  }
  std::optional<ir::BuildDraftOutcome> draftOutcome;
  if (persistenceOptions.irSubmission) {
    draftOutcome = context.irDrivers.buildDraft(
        ir::kTachiProviderId, *persistenceOptions.irSubmission);
  }
  return ir::makeIrResultPresentation(
      {.providerId = std::string(ir::kTachiProviderId),
       .providerDisplayName = "Bokutachi",
       .capabilities = capabilities,
       .settings = std::move(settings),
       .saveOutcome = persistenceOptions.outcome,
       .submission = persistenceOptions.irSubmission,
       .draftOutcome = std::move(draftOutcome),
       .snapshot = std::move(snapshot)});
}

void ResultScene::addIrResultStatus() {
  if (rootLayout == nullptr || irResultStatus != nullptr) {
    return;
  }
  normalResultActions = rootLayout->findViewByName("resultActions");

  auto *status = new View();
  status->setName("irResultStatus");
  status->setWidthPercent(100.0F);
  status->setHeight(72.0F);
  status->setMinHeight(72.0F);
  status->setFlexShrink(0.0F);
  status->setPadding(Edge::Top, 8.0F);
  status->setPadding(Edge::Bottom, 8.0F);
  status->setPadding(Edge::Left, 14.0F);
  status->setPadding(Edge::Right, 14.0F);
  status->setFlexDirection(FlexDirection::Row);
  status->setAlignItems(YGAlignCenter);
  status->setGap(16.0F);
  status->setBackgroundColor(ui_theme::resultPanelStrong());
  status->setCornerRadius(ui_theme::panelRadius());
  status->setBorderColor(ui_theme::hairlineStrong());
  status->setBorderWidth(1);

  auto *copy = new View();
  copy->setFlexDirection(FlexDirection::Column);
  copy->setFlex(1.0F);
  copy->setMinWidth(0.0F);
  copy->setGap(2.0F);
  irResultStatusText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  irResultStatusText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  irResultStatusText->setWrap(true);
  irResultStatusText->setWidthPercent(100.0F);
  copy->addView(irResultStatusText);
  irResultDetailText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  irResultDetailText->setColor(ui_theme::sdl(ui_theme::textSecondary()));
  irResultDetailText->setWrap(true);
  irResultDetailText->setWidthPercent(100.0F);
  copy->addView(irResultDetailText);
  status->addView(copy);

  const auto makeAction = [](const std::string &label, const Color &accent,
                             std::function<void()> action) {
    auto *button = new Button(0, 0, 196, 52);
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 19);
    text->setText(label);
    text->setAlign(TextView::CENTER);
    text->setVAlign(TextView::MIDDLE);
    text->setColor(ui_theme::sdl(ui_theme::textPrimary()));
    button->setContentView(text);
    button->setSize(196, 52);
    button->setCornerRadius(ui_theme::controlRadius());
    button->setBackgroundColors(
        ui_theme::withAlpha(accent, 72), ui_theme::withAlpha(accent, 104),
        ui_theme::withAlpha(accent, 136));
    button->setBorderColors(
        ui_theme::withAlpha(accent, 170), ui_theme::withAlpha(accent, 210),
        accent);
    button->setStyledBorderWidth(1);
    button->setOnClickListener(std::move(action));
    return button;
  };
  irResultSubmitButton = makeAction(
      "Submit", ui_theme::cyan(), [this]() { submitIrResult(); });
  status->addView(irResultSubmitButton);
  irResultRetryButton = makeAction(
      "Retry", ui_theme::lime(), [this]() { retryIrResult(); });
  status->addView(irResultRetryButton);

  status->setDisplay(YGDisplayNone);
  status->setVisible(false);
  if (normalResultActions != nullptr) {
    rootLayout->insertViewBefore(status, normalResultActions);
  } else {
    rootLayout->addView(status);
  }
  irResultStatus = status;
}

void ResultScene::updateIrResultPresentation(bool force) {
  const ir::IrResultPresentation presentation = makeIrResultPresentation();
  if (!force && irObservedSnapshotInitialized &&
      presentation.snapshotRevision == irObservedSnapshotRevision) {
    return;
  }
  irObservedSnapshotInitialized = true;
  irObservedSnapshotRevision = presentation.snapshotRevision;
  if (irResultStatus == nullptr) {
    addIrResultStatus();
  }
  if (irResultStatus == nullptr) {
    return;
  }

  irResultStatus->setVisible(presentation.visible);
  irResultStatus->setDisplay(presentation.visible ? YGDisplayFlex
                                                  : YGDisplayNone);
  if (!presentation.visible) {
    if (rootLayout != nullptr) {
      rootLayout->applyYogaLayout();
    }
    return;
  }

  if (auto *visuals = rootLayout->findViewByName("resultVisuals")) {
    visuals->setMinHeight(176.0F);
  }
  if (irResultStatusText != nullptr) {
    irResultStatusText->setText(presentation.providerDisplayName + " · " +
                                presentation.statusText);
  }
  if (irResultDetailText != nullptr) {
    irResultDetailText->setText(irActionDiagnostic.empty()
                                    ? presentation.detailText
                                    : irActionDiagnostic);
  }
  if (irResultSubmitButton != nullptr) {
    irResultSubmitButton->setVisible(presentation.showSubmit);
    irResultSubmitButton->setDisplay(presentation.showSubmit ? YGDisplayFlex
                                                             : YGDisplayNone);
    irResultSubmitButton->setEnabled(presentation.canSubmit);
  }
  if (irResultRetryButton != nullptr) {
    irResultRetryButton->setVisible(presentation.canRetry);
    irResultRetryButton->setDisplay(presentation.canRetry ? YGDisplayFlex
                                                          : YGDisplayNone);
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void ResultScene::submitIrResult() {
  const ir::IrResultPresentation presentation = makeIrResultPresentation();
  if (!presentation.canSubmit || !persistenceOptions.irSubmission) {
    return;
  }
  if (!context.irSubmissionService) {
    irActionDiagnostic = "The IR submission service is unavailable.";
    updateIrResultPresentation(true);
    return;
  }
  const auto draft = context.irDrivers.buildDraft(
      ir::kTachiProviderId, *persistenceOptions.irSubmission);
  if (draft.status != ir::BuildDraftStatus::Built || !draft.draft) {
    irActionDiagnostic = draft.diagnostic.empty()
                             ? "This result could not be prepared for IR."
                             : ir::sanitizeDiagnostic(draft.diagnostic);
    updateIrResultPresentation(true);
    return;
  }
  const auto enqueued = context.irSubmissionService->enqueueManual(*draft.draft);
  if (enqueued.status == ir::IrOutboxInsertStatus::Inserted ||
      enqueued.status == ir::IrOutboxInsertStatus::AlreadyExists) {
    irActionDiagnostic.clear();
  } else {
    irActionDiagnostic =
        enqueued.diagnostic.empty()
            ? "This result could not be added to the submission queue."
            : ir::sanitizeDiagnostic(enqueued.diagnostic);
  }
  updateIrResultPresentation(true);
}

void ResultScene::retryIrResult() {
  const ir::IrResultPresentation presentation = makeIrResultPresentation();
  if (!presentation.canRetry || presentation.rowId <= 0) {
    return;
  }
  if (!context.irSubmissionService) {
    irActionDiagnostic = "The IR submission service is unavailable.";
    updateIrResultPresentation(true);
    return;
  }
  const auto retried = context.irSubmissionService->retry(presentation.rowId);
  if (retried.status == ir::IrOutboxMutationStatus::Updated) {
    irActionDiagnostic.clear();
  } else {
    irActionDiagnostic =
        retried.diagnostic.empty()
            ? "This submission could not be scheduled for retry."
            : ir::sanitizeDiagnostic(retried.diagnostic);
  }
  updateIrResultPresentation(true);
}

void ResultScene::retryResultPersistence() {
  if (persistenceOptions.attempt == nullptr ||
      !persistenceOptions.outcome.retryable()) {
    return;
  }

  persistenceContinueChosen = false;
  std::vector<ir::IrOutboxDraft> automaticDrafts;
  if (persistenceOptions.irSubmission) {
    automaticDrafts = context.irDrivers.buildAutomaticDrafts(
        context.settings.irProviders, *persistenceOptions.irSubmission);
  }
  persistenceOptions.outcome = context.resultPersistence.persist(
      *persistenceOptions.attempt, automaticDrafts);
  if (persistenceOptions.outcome.saved() && !automaticDrafts.empty() &&
      context.irSubmissionService) {
    context.irSubmissionService->notifyOutboxChanged();
  }
  SDL_Log("Result persistence retry state=%d diagnostic=%s",
          static_cast<int>(persistenceOptions.outcome.state),
          persistenceOptions.outcome.diagnostic.c_str());
  applyResultPersistenceReceipt();
  previousBestLoaded = false;
  loadPreviousBest();
  defer(
      [this]() {
        refreshResultSummary();
        updateResultPersistencePresentation();
        updateIrResultPresentation(true);
        return true;
      },
      0, true);
}

void ResultScene::continueWithoutSaving() {
  persistenceContinueChosen = true;
  updateResultPersistencePresentation();
}

void ResultScene::updateResultPersistencePresentation() {
  const bool decisionRequired = persistenceDecisionRequired();
  if (normalResultActions != nullptr) {
    normalResultActions->setVisible(!decisionRequired);
    normalResultActions->setDisplay(decisionRequired ? YGDisplayNone
                                                     : YGDisplayFlex);
  }
  if (resultPersistenceStatus != nullptr) {
    resultPersistenceStatus->setVisible(decisionRequired);
    resultPersistenceStatus->setDisplay(decisionRequired ? YGDisplayFlex
                                                         : YGDisplayNone);
  }
  if (persistenceStatusMessage != nullptr) {
    persistenceStatusMessage->setText(persistenceOptions.outcome.userMessage);
  }
  if (persistenceRetryButton != nullptr) {
    persistenceRetryButton->setEnabled(persistenceOptions.attempt != nullptr &&
                                       persistenceOptions.outcome.retryable());
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void ResultScene::refreshResultSummary() {
  if (rootLayout == nullptr || skin == nullptr) {
    return;
  }
  View *summary = rootLayout->findViewByName("resultSummary");
  if (summary == nullptr) {
    return;
  }
  ResultSkinData data = makeResultSkinData();
  skin->rebuildLayoutSection("ResultSummary", summary, &data);
  rootLayout->applyYogaLayout();
}

void ResultScene::addRetryButtons() {
  if (rootLayout == nullptr) {
    return;
  }

  View *actionHost = rootLayout->findViewByName("resultActions");
  if (actionHost == nullptr) {
    actionHost = rootLayout;
  }

  auto retryRow = new View();
  retryRow->setFlexDirection(FlexDirection::Row);
  retryRow->setAlignItems(YGAlignCenter);
  retryRow->setJustifyContent(YGJustifyCenter);
  retryRow->setFlexWrap(YGWrapWrap);
  retryRow->setGap(14);

  auto makeButton = [this](const std::string &label, bool samePattern,
                           bool replay, Color normal, Color hover,
                           Color pressed, Color border) {
    auto button = new Button();
    auto text = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
    text->setText(label);
    text->setAlign(TextView::CENTER);
    text->setVAlign(TextView::MIDDLE);
    text->setColor(ui_theme::sdl(ui_theme::textOn(normal)));
    button->setContentView(text);
    button->setOnClickListener([this, samePattern, replay]() {
      if (replay) {
        startReplay();
      } else {
        startRetry(samePattern);
      }
    });
    button->setSize(232, 64);
    button->setCornerRadius(ui_theme::controlRadius());
    button->setBackgroundColors(normal, hover, pressed);
    button->setBorderColors(ui_theme::withAlpha(border, 150),
                            ui_theme::withAlpha(border, 190),
                            ui_theme::withAlpha(border, 220));
    button->setStyledBorderWidth(1);
    return button;
  };

  if (replayResult) {
    retryRow->addView(makeButton("Replay", true, true, ui_theme::infoAction(),
                                 ui_theme::infoActionHover(),
                                 ui_theme::infoActionPressed(),
                                 ui_theme::cyan()));
  } else if (practiceOptions.enabled) {
    retryRow->addView(makeButton("Retry", true, false,
                                 ui_theme::primaryAction(),
                                 ui_theme::primaryActionHover(),
                                 ui_theme::primaryActionPressed(),
                                 ui_theme::cyan()));
  } else {
    const bool canRetrySame =
        retryData.has_value()
            ? play_options::hasSamePatternRandomization(*retryData)
            : play_options::hasSamePatternRandomization(meta);
    retryRow->addView(makeButton("Retry", !canRetrySame, false,
                                 ui_theme::primaryAction(),
                                 ui_theme::primaryActionHover(),
                                 ui_theme::primaryActionPressed(),
                                 ui_theme::cyan()));
    if (canRetrySame) {
      retryRow->addView(makeButton("Retry Same", true, false,
                                   ui_theme::successAction(),
                                   ui_theme::successActionHover(),
                                   ui_theme::successActionPressed(),
                                   ui_theme::lime()));
    }
  }

  rankingsButton = new Button();
  auto *rankingsText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  rankingsText->setText("Rankings");
  rankingsText->setAlign(TextView::CENTER);
  rankingsText->setVAlign(TextView::MIDDLE);
  rankingsText->setColor(
      ui_theme::sdl(ui_theme::textOn(ui_theme::infoAction())));
  rankingsButton->setContentView(rankingsText);
  rankingsButton->setOnClickListener([this]() { openRankings(); });
  rankingsButton->setSize(232, 64);
  rankingsButton->setCornerRadius(ui_theme::controlRadius());
  rankingsButton->setBackgroundColors(ui_theme::infoAction(),
                                       ui_theme::infoActionHover(),
                                       ui_theme::infoActionPressed());
  rankingsButton->setBorderColors(
      ui_theme::withAlpha(ui_theme::cyan(), 150),
      ui_theme::withAlpha(ui_theme::cyan(), 190),
      ui_theme::withAlpha(ui_theme::cyan(), 220));
  rankingsButton->setStyledBorderWidth(1);
  retryRow->addView(rankingsButton);
  refreshRankingsButton();

  exportPhotoButton = new Button();
  exportPhotoButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  exportPhotoButtonText->setText("Export Photo");
  exportPhotoButtonText->setAlign(TextView::CENTER);
  exportPhotoButtonText->setVAlign(TextView::MIDDLE);
  exportPhotoButtonText->setColor(
      ui_theme::sdl(ui_theme::textOn(ui_theme::violetAction())));
  exportPhotoButton->setContentView(exportPhotoButtonText);
  exportPhotoButton->setOnClickListener([this]() { exportPhoto(); });
  exportPhotoButton->setSize(232, 64);
  exportPhotoButton->setCornerRadius(ui_theme::controlRadius());
  exportPhotoButton->setBackgroundColors(ui_theme::violetAction(),
                                         ui_theme::violetActionHover(),
                                         ui_theme::violetActionPressed());
  exportPhotoButton->setBorderColors(
      ui_theme::withAlpha(ui_theme::violetActionHover(), 150),
      ui_theme::withAlpha(ui_theme::violetActionHover(), 190),
      ui_theme::withAlpha(ui_theme::violetActionHover(), 220));
  exportPhotoButton->setStyledBorderWidth(1);
  if (autoPlayResult) {
    exportPhotoButtonText->setText("AUTO PLAY");
    exportPhotoButton->setOnClickListener([]() {});
    exportPhotoButton->setBackgroundColors(ui_theme::control(),
                                           ui_theme::control(),
                                           ui_theme::control());
    exportPhotoButton->setBorderColors(ui_theme::hairlineSubtle(),
                                       ui_theme::hairlineSubtle(),
                                       ui_theme::hairlineSubtle());
  }
  retryRow->addView(exportPhotoButton);

  practiceSectionButton = new Button();
  practiceSectionButtonText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  practiceSectionButtonText->setText("Select Section");
  practiceSectionButtonText->setAlign(TextView::CENTER);
  practiceSectionButtonText->setVAlign(TextView::MIDDLE);
  practiceSectionButtonText->setColor(
      ui_theme::sdl(ui_theme::textOn(ui_theme::successAction())));
  practiceSectionButton->setContentView(practiceSectionButtonText);
  practiceSectionButton->setOnClickListener(
      [this]() { practiceThisSection(); });
  practiceSectionButton->setSize(280, 64);
  practiceSectionButton->setCornerRadius(ui_theme::controlRadius());
  practiceSectionButton->setBackgroundColors(
      ui_theme::successAction(), ui_theme::successActionHover(),
      ui_theme::successActionPressed());
  practiceSectionButton->setBorderColors(
      ui_theme::withAlpha(ui_theme::lime(), 150),
      ui_theme::withAlpha(ui_theme::lime(), 190),
      ui_theme::withAlpha(ui_theme::lime(), 220));
  practiceSectionButton->setStyledBorderWidth(1);
  retryRow->addView(practiceSectionButton);
  updatePracticeSectionAction();
  actionHost->addView(retryRow);
}

bool ResultScene::rankingsAvailable() const {
  if (context.irRankingService == nullptr || isCourseStageResult() ||
      isCourseFinalResult()) {
    return false;
  }
  const auto driver = context.irDrivers.find(ir::kTachiProviderId);
  const auto settings = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  return driver != nullptr && driver->capabilities().chartRankings &&
         settings != context.settings.irProviders.end() &&
         settings->second.enabled &&
         ir::makeBokutachiRankingQuery(meta).value.has_value();
}

void ResultScene::refreshRankingsButton() {
  if (rankingsButton != nullptr) {
    rankingsButton->setEnabled(rankingsAvailable());
  }
}

void ResultScene::openRankings() {
  if (!rankingsAvailable() || rankingOverlayPortal == nullptr ||
      context.irRankingService == nullptr) {
    refreshRankingsButton();
    return;
  }
  const auto query = ir::makeBokutachiRankingQuery(meta);
  const auto settings = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  if (!query.value || settings == context.settings.irProviders.end()) {
    refreshRankingsButton();
    return;
  }
  if (!rankingsModal) {
    rankingsModal = std::make_unique<ir::IrRankingModal>(
        *rankingOverlayPortal, *context.irRankingService);
  }
  rankingsModal->open(
      {.profileId = context.profileManager.activeProfile().id,
       .providerId = std::string(ir::kTachiProviderId),
       .serverOrigin = settings->second.serverOrigin,
       .chart = *query.value,
       .localComparison = ir::IrLocalComparison{
           .label = "This Play",
           .score = resultState.getScore(),
           .maxScore = std::max(0, meta.TotalNotes) * 2,
           .clearType = resultState.getClearTypeRank(),
           .badPoints = resultState.comboBreak,
           .maxCombo = resultState.maxCombo,
       }},
      meta.Title.empty() ? "Completed chart" : meta.Title);
}

void ResultScene::addCourseButtons() {
  if (rootLayout == nullptr) {
    return;
  }

  View *actionHost = rootLayout->findViewByName("resultActions");
  if (actionHost == nullptr) {
    actionHost = rootLayout;
  }

  auto makeButton = [](const std::string &label, Color normal, Color hover,
                       Color pressed, Color border,
                       std::function<void()> onClick) {
    auto *button = new Button();
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
    text->setText(label);
    text->setAlign(TextView::CENTER);
    text->setVAlign(TextView::MIDDLE);
    text->setColor(ui_theme::sdl(ui_theme::textOn(normal)));
    button->setContentView(text);
    button->setOnClickListener(std::move(onClick));
    button->setSize(232, 64);
    button->setCornerRadius(ui_theme::controlRadius());
    button->setBackgroundColors(normal, hover, pressed);
    button->setBorderColors(ui_theme::withAlpha(border, 150),
                            ui_theme::withAlpha(border, 190),
                            ui_theme::withAlpha(border, 220));
    button->setStyledBorderWidth(1);
    return std::pair<Button *, TextView *>(button, text);
  };

  if (isCourseStageResult()) {
    auto [nextButton, ignoredText] = makeButton(
        "Next", ui_theme::successAction(), ui_theme::successActionHover(),
        ui_theme::successActionPressed(), ui_theme::lime(),
        [this]() { continueCourse(); });
    (void)ignoredText;
    actionHost->addView(nextButton);
  }

  if (isCourseFinalResult() && courseOptions.session != nullptr &&
      courseOptions.session->courseReplayData != nullptr &&
      !courseOptions.session->courseReplayData->stages.empty()) {
    auto [replayButton, ignoredText] = makeButton(
        "Replay", ui_theme::infoAction(), ui_theme::infoActionHover(),
        ui_theme::infoActionPressed(), ui_theme::cyan(),
        [this]() { startCourseReplay(); });
    (void)ignoredText;
    actionHost->addView(replayButton);
  }

  auto [photoButton, photoText] = makeButton(
      "Export Photo", ui_theme::violetAction(),
      ui_theme::violetActionHover(), ui_theme::violetActionPressed(),
      ui_theme::violetActionHover(), [this]() { exportPhoto(); });
  exportPhotoButton = photoButton;
  exportPhotoButtonText = photoText;
  actionHost->addView(exportPhotoButton);

  if (auto *backButton =
          dynamic_cast<Button *>(rootLayout->findViewByName("backButton"));
      backButton != nullptr) {
    backButton->setOnClickListener([this]() {
      if (isCourseStageResult()) {
        showCourseExitConfirmation();
      } else {
        exitResult();
      }
    });
  }
}

void ResultScene::buildCourseExitConfirmation() {
  if (!isCourseStageResult() || rootLayout == nullptr) {
    return;
  }

  auto *overlay = new Button();
  overlay->setName("courseExitConfirmation");
  overlay->setPositionType(YGPositionTypeAbsolute);
  overlay->setPosition(Edge::Left, 0);
  overlay->setPosition(Edge::Top, 0);
  overlay->setWidth(static_cast<float>(rendering::window_width));
  overlay->setHeight(static_cast<float>(rendering::window_height));
  overlay->setFlexDirection(FlexDirection::Column);
  overlay->setAlignItems(YGAlignCenter);
  overlay->setJustifyContent(YGJustifyCenter);
  overlay->setBackgroundColors(Color(0, 0, 0, 174), Color(0, 0, 0, 174),
                               Color(0, 0, 0, 174));
  overlay->setOnClickListener([]() {});
  overlay->setZIndex(100);

  auto *panel = new View();
  panel->setWidth(560);
  panel->setPadding(Edge::All, 26);
  panel->setFlexDirection(FlexDirection::Column);
  panel->setAlignItems(YGAlignStretch);
  panel->setGap(18);
  panel->setBackgroundColor(ui_theme::resultPanelStrong());
  panel->setCornerRadius(ui_theme::panelRadius());
  panel->setBorderColor(ui_theme::withAlpha(ui_theme::coral(), 180));
  panel->setBorderWidth(1);
  panel->setShadow(ui_theme::cardShadow(), ui_theme::kCardShadow);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title->setText("Leave Course?");
  title->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  title->setAlign(TextView::CENTER);
  title->setHeight(42);
  panel->addView(title);

  auto *message = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  message->setText("Course progress will be lost.");
  message->setColor(ui_theme::sdl(ui_theme::textSecondary()));
  message->setAlign(TextView::CENTER);
  message->setHeight(32);
  panel->addView(message);

  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setAlignItems(YGAlignCenter);
  row->setJustifyContent(YGJustifyCenter);
  row->setGap(14);

  auto makeModalButton = [](const std::string &label, Color normal,
                            Color hover, Color pressed, Color border,
                            std::function<void()> onClick) {
    auto *button = new Button();
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
    text->setText(label);
    text->setAlign(TextView::CENTER);
    text->setVAlign(TextView::MIDDLE);
    text->setColor(ui_theme::sdl(ui_theme::textOn(normal)));
    button->setContentView(text);
    button->setOnClickListener(std::move(onClick));
    button->setSize(208, 58);
    button->setCornerRadius(ui_theme::controlRadius());
    button->setBackgroundColors(normal, hover, pressed);
    button->setBorderColors(ui_theme::withAlpha(border, 150),
                            ui_theme::withAlpha(border, 190),
                            ui_theme::withAlpha(border, 220));
    button->setStyledBorderWidth(1);
    return button;
  };

  row->addView(makeModalButton(
      "Cancel", ui_theme::control(), ui_theme::controlHover(),
      ui_theme::controlPressed(), ui_theme::hairlineSubtle(),
      [this]() { hideCourseExitConfirmation(); }));
  row->addView(makeModalButton(
      "Back to Menu", ui_theme::warningAction(),
      ui_theme::warningActionHover(), ui_theme::warningActionPressed(),
      ui_theme::coral(), [this]() { exitResult(); }));
  panel->addView(row);
  overlay->addView(panel);
  overlay->setVisible(false);
  courseExitConfirmation = overlay;
  rootLayout->addView(overlay);
}

void ResultScene::showCourseExitConfirmation() {
  if (courseExitConfirmation != nullptr) {
    courseExitConfirmation->setVisible(true);
    rootLayout->applyYogaLayout();
  }
}

void ResultScene::hideCourseExitConfirmation() {
  if (courseExitConfirmation != nullptr) {
    courseExitConfirmation->setVisible(false);
    rootLayout->applyYogaLayout();
  }
}

void ResultScene::recordCourseStageRestTime() {
  if (!isCourseStageResult() || courseStageRestRecorded ||
      courseOptions.session == nullptr ||
      courseOptions.session->courseReplayPlayback ||
      courseStageResultShownMicros <= 0) {
    return;
  }

  courseStageRestRecorded = true;
  courseOptions.session->recordRestMicrosAfterCurrentStage(
      nowMicros() - courseStageResultShownMicros);
}

void ResultScene::exportPhoto() {
  if (autoPlayResult || resultPhotoExportInProgress ||
      exportPhotoButtonText == nullptr) {
    return;
  }

  resultPhotoExportInProgress = true;
  exportPhotoButtonText->setText("Saving...");
  const std::optional<ResultPacemakerData> pacemaker =
      pacemakerDataForCurrentResult();
  const auto analyticsModel = makeTimingAnalyticsModel();
  const auto result = ResultImageExporter::Export(
      context, meta, resultState, playModeLabel, laneOrderLabel,
      difficultyLabel, previousBest, currentClearLabelOverride,
      currentClearRankOverride, headerDifficultyLabelOverride, pacemaker,
      analyticsModel);
  resultPhotoExportInProgress = false;

  if (result.success) {
    exportPhotoButtonText->setText(result.message == "Saved to Photos"
                                       ? "Saved"
                                       : "Exported");
    SDL_Log("Result image exported: %s (%s)",
            fspath_to_utf8(result.outputPath).c_str(), result.message.c_str());
  } else {
    exportPhotoButtonText->setText("Export Failed");
    SDL_Log("Result image export failed: %s (%s)", result.message.c_str(),
            fspath_to_utf8(result.outputPath).c_str());
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }

  defer(
      [this]() {
        if (!resultPhotoExportInProgress && exportPhotoButtonText != nullptr) {
          exportPhotoButtonText->setText("Export Photo");
          if (rootLayout != nullptr) {
            rootLayout->applyYogaLayout();
          }
        }
        return true;
      },
      result.success ? 1800 : 1400, true);
}

void ResultScene::continueCourse() {
  auto session = courseOptions.session;
  if (!isCourseStageResult() || session == nullptr ||
      courseTransitionStarted) {
    return;
  }
  courseTransitionStarted = true;
  recordCourseStageRestTime();

  if (session->courseReplayPlayback) {
    if (!session->hasNextCourseReplayStage()) {
      showCourseResult();
      return;
    }

    session->currentIndex++;
    startCourseReplayStage(session);
    return;
  }

  const float finalGauge = session->carriedGauge.has_value()
                               ? session->carriedGauge->currentGauge
                               : resultState.currentGauge;
  if (!session->hasNextChart() || finalGauge <= 0.0f) {
    showCourseResult();
    return;
  }

  session->currentIndex++;
  const bms_parser::ChartMeta *nextMeta = session->currentMeta();
  if (nextMeta == nullptr || nextMeta->BmsPath.empty()) {
    showCourseResult();
    return;
  }

  std::atomic_bool parseCancelled = false;
  std::unique_ptr<bms_parser::Chart> nextChart;
  try {
    nextChart =
        play_options::parseChart(nextMeta->BmsPath, parseCancelled, "course");
  } catch (const std::exception &e) {
    SDL_Log("Course parse failed %s: %s",
            fspath_to_utf8(nextMeta->BmsPath).c_str(), e.what());
    archive_file::appendDebugLogLine(
        "Course parse exception: " + fspath_to_utf8(nextMeta->BmsPath) +
        ": " + e.what());
    showCourseResult();
    return;
  }
  if (nextChart == nullptr || parseCancelled) {
    showCourseResult();
    return;
  }
  applyCourseConstraintsToChart(*nextChart, session->constraints);

  play_options::PlayOptionReplayInfo playInfo =
      play_options::applySelectedPlayOptions(*nextChart,
                                             session->requestedPlayOption);
  applyEffectiveLongNoteModeToChart(*nextChart, session->longNoteMode);
  session->playOption = playInfo.option;
  session->playOptionSeed = playInfo.seed;
  session->playOption2 = playInfo.option2;
  session->playOption2Seed = playInfo.seed2;

  context.jukebox.stop();
  context.jukebox.loadChart(*nextChart, true, parseCancelled);
  if (parseCancelled) {
    showCourseResult();
    return;
  }

  StartOptions nextOptions;
  nextOptions.startPosition = 0;
  nextOptions.autoKeySound = session->autoKeySound;
  nextOptions.autoPlay = false;
  nextOptions.gaugeType = session->gaugeType;
  nextOptions.gaugeProfile = session->gaugeProfile;
  nextOptions.gaugeAutoShift = session->gaugeAutoShift;
  nextOptions.gaugeAutoShiftLowerBound = session->gaugeAutoShiftLowerBound;
  nextOptions.playOption = playInfo.option;
  nextOptions.playOptionSeed = playInfo.seed;
  nextOptions.playOption2 = playInfo.option2;
  nextOptions.playOption2Seed = playInfo.seed2;
  nextOptions.longNoteMode = session->longNoteMode;
  nextOptions.assistOption = session->assistOption;
  nextOptions.courseSession = session;
  nextOptions.courseConstraints = session->constraints;
  nextOptions.ruleset = session->ruleset;
  nextOptions.requiredRulesetDescriptor = session->rulesetDescriptor;
  nextOptions.ownsChart = true;

  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(nextChart),
                                      std::move(nextOptions)),
      false);
}

void ResultScene::showCourseResult() {
  auto session = courseOptions.session;
  if (session == nullptr) {
    exitResult();
    return;
  }

  bms_parser::ChartMeta courseMeta = courseResultMetaForSession(*session);
  RhythmState courseState = courseResultStateForSession(*session);
  context.sceneManager->changeScene(
      std::make_unique<ResultScene>(
          context, courseMeta, courseState, session->aggregateProvenance(),
          nullptr, ResultPersistenceOptions{}, nullptr, ResultPracticeOptions{},
          false,
          ResultCourseOptions{.mode = ResultCourseMode::CourseResult,
                              .session = session}),
      false);
}

void ResultScene::startRetry(bool samePattern) {
  const std::optional<practice::Configuration> practiceConfiguration =
      practiceOptions.session != nullptr
          ? std::optional<practice::Configuration>(
                practiceOptions.session->configuration())
          : std::nullopt;
  const audio::PlaybackRate retryPlayback =
      resultRetryPlayback(attemptProvenance, practiceConfiguration);
  const auto retryPracticeSession =
      freshPracticeSessionForRetry(practiceOptions.session);
  ReplayData retrySource;
  if (retryData.has_value()) {
    retrySource = *retryData;
  } else {
    retrySource.chartMeta = meta;
    retrySource.randomSeed = meta.RandomSeed;
    retrySource.randomPrng = meta.RandomPrng;
    retrySource.randomValues = meta.RandomValues;
  }
  if (practiceOptions.enabled) {
    retrySource.playOption = practiceOptions.playOption;
    retrySource.playOptionSeed = practiceOptions.playOptionSeed;
    retrySource.playOption2 = practiceOptions.playOption2;
    retrySource.playOption2Seed = practiceOptions.playOption2Seed;
    retrySource.assistOption = practiceOptions.assistOption;
    retrySource.initialGaugeType =
        practiceConfiguration.has_value()
            ? practiceConfiguration->gaugeType
            : practiceOptions.gaugeType;
    retrySource.gaugeAutoShift = practiceOptions.gaugeAutoShift;
    retrySource.gaugeAutoShiftLowerBound =
        practiceOptions.gaugeAutoShiftLowerBound;
  }

  context.jukebox.stop();
  defer(
      [this, retrySource, samePattern, practiceConfiguration, retryPlayback,
       retryPracticeSession]() {
        std::atomic_bool parseCancelled = false;
        const bool reuseCurrentPattern =
            samePattern && reusableRetryChart != nullptr;
        std::unique_ptr<bms_parser::Chart> ownedRetryChart;
        bms_parser::Chart *retryChart = nullptr;
        if (reuseCurrentPattern) {
          if (ownedReusableRetryChart != nullptr) {
            ownedRetryChart = std::move(ownedReusableRetryChart);
            retryChart = ownedRetryChart.get();
          } else {
            retryChart = reusableRetryChart;
          }
        } else {
          ownedRetryChart = play_options::parseChartForRetry(
              retrySource, meta, parseCancelled, samePattern);
          retryChart = ownedRetryChart.get();
        }
        if (retryChart == nullptr || parseCancelled) {
          return true;
        }

        StartOptions options;
        options.startPosition = practiceOptions.enabled
                                    ? (practiceConfiguration.has_value()
                                           ? static_cast<unsigned long long>(
                                                 std::max(
                                                     0LL,
                                                     practiceConfiguration
                                                         ->startMicros))
                                           : practiceOptions.startPosition)
                                    : 0;
        options.autoKeySound =
            practiceOptions.enabled
                ? (practiceOptions.autoPlay || practiceOptions.autoKeySound)
                                    : !context.settings.inputKeysoundEnabled;
        options.autoPlay =
            practiceOptions.enabled ? practiceOptions.autoPlay : false;
        options.gaugeType = practiceConfiguration.has_value()
                                ? practiceConfiguration->gaugeType
                                : retrySource.initialGaugeType;
        options.gaugeAutoShift =
            practiceConfiguration.has_value()
                ? practiceConfiguration->gaugeAutoShift
                : retrySource.gaugeAutoShift;
        options.gaugeAutoShiftLowerBound =
            practiceConfiguration.has_value()
                ? practiceConfiguration->gaugeAutoShiftLowerBound
                : retrySource.gaugeAutoShiftLowerBound;
        options.longNoteMode = normalizeChartLongNoteModeValue(
            retrySource.chartMeta.LnMode);
        options.assistOption = retrySource.assistOption;
        options.clubMode = attemptProvenance.clubMode;
        options.pacemakerTarget =
            practiceOptions.enabled
                ? pacemaker::kTargetOff
                : pacemaker::normalizeTargetId(
                      context.settings.selectedPacemakerTarget);
        options.playback = retryPlayback;
        options.ownsChart = true;
        options.requiredRulesetDescriptor = attemptProvenance.ruleset;
        if (const auto completedRuleset =
                gameplayRulesetFromId(attemptProvenance.ruleset.id)) {
          options.ruleset = *completedRuleset;
        }
        if (practiceOptions.enabled) {
          options.practiceSession = retryPracticeSession;
          options.practiceMode = retryPracticeSession == nullptr;
          options.practiceLeadInMicros =
              retryPracticeSession == nullptr ? practiceOptions.leadInMicros
                                              : 0;
          if (practiceConfiguration.has_value()) {
            options.judgeWindowScalePercent =
                practiceConfiguration->judge.scalePercent;
            options.startingGaugePercent =
                practiceConfiguration->startingGaugePercent;
          }
          options.longNoteMode = practiceOptions.longNoteMode;
          options.returnScene = practiceOptions.returnScene;
          if (!options.autoPlay) {
            options.practiceGhostCallback =
                practiceOptions.practiceGhostCallback;
          }
          if (options.autoPlay) {
            options.touchVisualizationEnabled = false;
            options.replayGhostRenderingEnabled = false;
          }
        }

        if (reuseCurrentPattern) {
          options.playOption = retrySource.playOption;
          options.playOptionSeed = retrySource.playOptionSeed;
          if (retryChart->Meta.IsDP) {
            options.playOption2 = retrySource.playOption2;
            options.playOption2Seed = retrySource.playOption2Seed;
          }
        } else if (retrySource.playOption.has_value()) {
          if (samePattern &&
              play_options::usesRandomizer(*retrySource.playOption) &&
              !retrySource.playOptionSeed.has_value()) {
            SDL_Log("Cannot retry same pattern: missing play option seed");
            return true;
          }
          const std::optional<long long> optionSeed =
              samePattern ? retrySource.playOptionSeed
                          : std::optional<long long>();
          if (!play_options::applyPlayOptionModifier(
                  *retryChart, *retrySource.playOption, optionSeed, 0,
                  options.playOption, options.playOptionSeed, "retry")) {
            return true;
          }
        }

        if (!reuseCurrentPattern && retryChart->Meta.IsDP &&
            retrySource.playOption2.has_value()) {
          if (samePattern &&
              play_options::usesRandomizer(*retrySource.playOption2) &&
              !retrySource.playOption2Seed.has_value()) {
            SDL_Log("Cannot retry same pattern: missing P2 play option seed");
            return true;
          }
          const std::optional<long long> optionSeed =
              samePattern ? retrySource.playOption2Seed
                          : std::optional<long long>();
          if (!play_options::applyPlayOptionModifier(
                  *retryChart, *retrySource.playOption2, optionSeed, 1,
                  options.playOption2, options.playOption2Seed, "retry")) {
            return true;
          }
        }

        context.jukebox.stop();
        if (!reuseCurrentPattern) {
          context.jukebox.reloadChartResources(*retryChart, true,
                                               parseCancelled);
          if (parseCancelled) {
            return true;
          }
        }

        if (ownedRetryChart != nullptr) {
          context.sceneManager->changeScene(
              std::make_unique<GamePlayScene>(context, std::move(ownedRetryChart),
                                              options),
              false);
        } else {
          options.ownsChart = false;
          context.sceneManager->changeScene(
              std::make_unique<GamePlayScene>(context, retryChart, options),
              false);
        }
        return false;
      },
      0, true);
}

void ResultScene::exitResult() {
  if (persistenceDecisionRequired()) {
    return;
  }
  context.jukebox.stop();
  if (practiceOptions.enabled && practiceOptions.returnScene != nullptr) {
    context.sceneManager->changeScene(practiceOptions.returnScene, false);
    return;
  }
  context.sceneManager->changeScene("MainMenu");
}

void ResultScene::startReplay() {
  if (!retryData.has_value()) {
    return;
  }

  const ReplayData replaySource = *retryData;
  context.jukebox.stop();
  defer(
      [this, replaySource]() {
        std::atomic_bool parseCancelled = false;
        auto replayChart = play_options::prepareReplayChart(
            meta.BmsPath, replaySource, parseCancelled);
        if (replayChart == nullptr || parseCancelled) {
          return true;
        }

        context.jukebox.stop();
        context.jukebox.loadChart(*replayChart, true, parseCancelled);
        if (parseCancelled) {
          return true;
        }

        auto replayData = std::make_shared<ReplayData>(replaySource);
        StartOptions replayOptions{
            .startPosition = 0,
            .autoKeySound = false,
            .autoPlay = false,
            .gaugeType = replayData->initialGaugeType,
            .gaugeAutoShift = replayData->gaugeAutoShift,
            .replayData = replayData,
            .ownsChart = true,
        };
        applyReplayProvenanceToStartOptions(replayOptions, *replayData);
        context.sceneManager->changeScene(
            std::make_unique<GamePlayScene>(
                context, std::move(replayChart), std::move(replayOptions)),
            false);
        return false;
      },
      0, true);
}

practice::LaunchRequest ResultScene::makePracticeLaunchRequest(
    long long startMicros, long long endMicros) const {
  const practice::LaunchSource source =
      practiceOptions.enabled
          ? practice::LaunchSource::PracticeResult
          : (replayResult ? practice::LaunchSource::ReplayResult
                          : practice::LaunchSource::NormalResult);
  bms_parser::ChartMeta chartMeta = meta;
  if (source == practice::LaunchSource::ReplayResult &&
      retryData.has_value()) {
    chartMeta = practice::mergeReplayLaunchChartMeta(meta, *retryData);
  }
  practice::LaunchRequest request{
      .chartMeta = chartMeta,
      .startMicros = startMicros,
      .endMicros = endMicros,
      .source = source,
  };
  const ScoreProvenance &rulesetSource =
      source == practice::LaunchSource::ReplayResult && retryData.has_value()
          ? retryData->provenance
          : attemptProvenance;
  request.requiredRulesetDescriptor = rulesetSource.ruleset;
  if (const auto selectedRuleset =
          gameplayRulesetFromId(rulesetSource.ruleset.id)) {
    request.ruleset = *selectedRuleset;
  }
  if (source == practice::LaunchSource::ReplayResult) {
    if (const auto *stage =
            score_provenance::uniqueStageForChart(rulesetSource, chartMeta)) {
      request.replayRulesetSnapshot = *stage;
    }
  }
  if (source == practice::LaunchSource::ReplayResult &&
      retryData.has_value()) {
    request.replayId = retryData->id;
    request.replayPlayOptions =
        practice::launchPlayOptionsFromReplay(*retryData);
  }
  return request;
}

std::optional<practice::LaunchRequest>
ResultScene::selectedPracticeLaunchRequest() const {
  if (timingAnalyticsView == nullptr) {
    return std::nullopt;
  }
  const auto selected = timingAnalyticsView->selectedSection();
  if (!selected.has_value()) {
    return std::nullopt;
  }
  return makePracticeLaunchRequest(selected->startMicros,
                                   selected->endMicros);
}

void ResultScene::updatePracticeSectionAction() {
  if (practiceSectionButton == nullptr || practiceSectionButtonText == nullptr) {
    return;
  }

  const auto availabilityRequest = makePracticeLaunchRequest(0, 1);
  if (const auto issue = practice::validateLaunchRequest(availabilityRequest);
      issue.has_value()) {
    practiceSectionButton->setEnabled(false);
    practiceSectionButtonText->setText(*issue);
    return;
  }

  const auto selectedRequest = selectedPracticeLaunchRequest();
  if (!selectedRequest.has_value()) {
    practiceSectionButton->setEnabled(false);
    practiceSectionButtonText->setText("Select Section");
    return;
  }
  if (const auto issue = practice::validateLaunchRequest(*selectedRequest);
      issue.has_value()) {
    practiceSectionButton->setEnabled(false);
    practiceSectionButtonText->setText(*issue);
    return;
  }

  practiceSectionButton->setEnabled(true);
  practiceSectionButtonText->setText("Practice Section");
}

void ResultScene::practiceThisSection() {
  const auto selectedRequest = selectedPracticeLaunchRequest();
  if (!selectedRequest.has_value() ||
      practice::validateLaunchRequest(*selectedRequest).has_value()) {
    updatePracticeSectionAction();
    return;
  }

  practice::LaunchRequest request = *selectedRequest;
  context.jukebox.stop();

  if (request.source == practice::LaunchSource::PracticeResult &&
      practiceOptions.returnScene != nullptr) {
    if (auto *viewer =
            dynamic_cast<ChartViewerScene *>(practiceOptions.returnScene);
        viewer != nullptr) {
      viewer->setPracticeLaunchRequest(std::move(request));
      context.sceneManager->changeScene(viewer, false);
      return;
    }
  }

  ChartMetaRecord record{
      .meta = request.chartMeta,
      .difficultyTableLabels = difficultyLabel,
  };
  const auto randomSeed = request.chartMeta.RandomSeed;
  const auto randomPrng = request.chartMeta.RandomPrng;
  const auto randomValues = request.chartMeta.RandomValues;
  context.sceneManager->changeScene(
      std::make_unique<ChartViewerScene>(
          context, std::move(record), randomSeed, randomPrng, randomValues,
          std::move(request)),
      false);
}

void ResultScene::startCourseReplay() {
  if (!isCourseFinalResult() || courseOptions.session == nullptr ||
      courseOptions.session->courseReplayData == nullptr ||
      courseOptions.session->courseReplayData->stages.empty() ||
      courseTransitionStarted) {
    return;
  }

  courseTransitionStarted = true;
  auto source = courseOptions.session->courseReplayData;
  auto replayData = std::make_shared<CourseReplayData>(*source);
  auto replaySession = std::make_shared<CoursePlaySession>();
  replaySession->courseId = replayData->courseId;
  replaySession->courseKey = replayData->courseKey;
  replaySession->courseName = replayData->courseName;
  replaySession->courseGroupName = replayData->courseGroupName;
  replaySession->constraintJson = replayData->constraintJson;
  replaySession->entries.reserve(replayData->stages.size());
  for (const auto &stage : replayData->stages) {
    replaySession->entries.push_back(CoursePlayEntry{.meta = stage.replay.chartMeta});
  }
  replaySession->snapshotRulesetFromReplay(replayData->stages.front().replay);
  const CourseConstraintSettings constraintSettings =
      courseConstraintSettingsFromJson(replayData->constraintJson);
  replaySession->currentIndex = 0;
  replaySession->gaugeType = replayData->initialGaugeType;
  replaySession->gaugeProfile = replayData->gaugeProfile;
  replaySession->gaugeAutoShift = replayData->gaugeAutoShift;
  replaySession->gaugeAutoShiftLowerBound =
      replayData->gaugeAutoShiftLowerBound;
  replaySession->longNoteMode = replayData->longNoteMode;
  replaySession->constraints = constraintSettings.rules;
  replaySession->requestedPlayOption = replayData->requestedPlayOption;
  replaySession->assistOption = replayData->assistOption;
  replaySession->autoKeySound = false;
  replaySession->courseReplayPlayback = true;
  replaySession->courseReplayData = std::move(replayData);
  replaySession->replayTouchVisualizationEnabled =
      courseOptions.session->replayTouchVisualizationEnabled;
  replaySession->replayGhostRenderingEnabled =
      courseOptions.session->replayGhostRenderingEnabled;
  startCourseReplayStage(std::move(replaySession));
}

void ResultScene::startCourseReplayStage(
    std::shared_ptr<CoursePlaySession> session) {
  if (session == nullptr ||
      !session->hasCourseReplayStage(session->currentIndex)) {
    return;
  }

  auto stageReplay = session->currentCourseReplayStageReplay();
  if (stageReplay == nullptr) {
    return;
  }
  session->applyReplayStagePlayOptions(*stageReplay);
  context.jukebox.stop();
  std::atomic_bool parseCancelled = false;
  auto replayChart = play_options::prepareReplayChart(
      stageReplay->chartMeta.BmsPath, *stageReplay, parseCancelled);
  if (replayChart == nullptr || parseCancelled) {
    context.sceneManager->changeScene("MainMenu");
    return;
  }

  context.jukebox.stop();
  context.jukebox.loadChart(*replayChart, true, parseCancelled);
  if (parseCancelled) {
    context.sceneManager->changeScene("MainMenu");
    return;
  }

  StartOptions options = makeCourseReplayStageStartOptions(session, stageReplay);

  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(replayChart),
                                      std::move(options)),
      false);
}

void ResultScene::init() {
  if (isCourseStageResult()) {
    courseStageResultShownMicros = nowMicros();
  }

  loadDifficultyLabel();
  loadPreviousBest();
  saveCourseScore();
  saveCourseReplay();

  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  addView(rootLayout);

  auto analyticsModel = makeTimingAnalyticsModel();
  ResultSkinData data = makeResultSkinData();
  data.showTimingAnalytics = analyticsModel.has_value();
  skin->buildLayout("Result", rootLayout, &data);
  addTimingAnalytics(std::move(analyticsModel));
  addResultPersistenceStatus();
  addIrResultStatus();
  if (isCourseStageResult() || isCourseFinalResult()) {
    addCourseButtons();
    buildCourseExitConfirmation();
  } else {
    addRetryButtons();
  }

  if (!isCourseStageResult() && !isCourseFinalResult()) {
    if (auto *backButton =
            dynamic_cast<Button *>(rootLayout->findViewByName("backButton"));
        backButton != nullptr) {
      backButton->setOnClickListener([this]() { exitResult(); });
    }
  }

  rankingOverlayPortal = new OverlayPortal(
      0, 0, rendering::window_width, rendering::window_height);
  rankingOverlayPortal->setPositionType(YGPositionTypeAbsolute);
  rankingOverlayPortal->setPosition(Edge::Left, 0);
  rankingOverlayPortal->setPosition(Edge::Top, 0);
  rankingOverlayPortal->setZIndex(2000);
  rootLayout->addView(rankingOverlayPortal);

  updateResultPersistencePresentation();
  updateIrResultPresentation(true);

  graphPlaceHolder = rootLayout->findViewByName("graph");

  rootLayout->applyYogaLayout();

  if (isCourseStageResult() && courseOptions.session != nullptr &&
      courseOptions.session->courseReplayPlayback) {
    const long long restMicros =
        courseOptions.session->restMicrosAfterCurrentStage();
    const Uint64 delayMs =
        static_cast<Uint64>((std::max(0LL, restMicros) + 999LL) / 1000LL);
    defer(
        [this]() {
          continueCourse();
          return false;
        },
        delayMs, true);
  }
}

void ResultScene::update(float dt) {
  (void)dt;
  updatePracticeSectionAction();
  if (persistenceOptions.irSubmission && context.irSubmissionService) {
    const auto snapshot = context.irSubmissionService->status(
        ir::kTachiProviderId, persistenceOptions.irSubmission->attemptId);
    if (!irObservedSnapshotInitialized ||
        snapshot.revision != irObservedSnapshotRevision) {
      irActionDiagnostic.clear();
      updateIrResultPresentation();
    }
  }
  if (rankingsModal) {
    rankingsModal->update();
  }
}

void ResultScene::renderScene() {
  if (rankingOverlayPortal != nullptr) {
    rankingOverlayPortal->setSize(rendering::window_width,
                                  rendering::window_height);
  }
  if (graphPlaceHolder && !resultState.gaugeHistory.empty()) {
    float x = graphPlaceHolder->getX();
    float y = graphPlaceHolder->getY();
    float w = graphPlaceHolder->getWidth();
    float h = graphPlaceHolder->getHeight();

    rendering::SimpleBatchRenderer graphBatch;
    graphBatch.setSubmitView(rendering::ui_view);
    graphBatch.setSubmitDepth(0);
    graphBatch.begin();
    drawResultGaugeLineGraph(graphBatch, resultState, x, y, w, h);
    graphBatch.end();
  }
}

void ResultScene::cleanupScene() {
  rankingsModal.reset();
  rootLayout = nullptr;
  graphPlaceHolder = nullptr;
  normalResultActions = nullptr;
  resultPersistenceStatus = nullptr;
  persistenceStatusMessage = nullptr;
  persistenceRetryButton = nullptr;
  irResultStatus = nullptr;
  irResultStatusText = nullptr;
  irResultDetailText = nullptr;
  irResultSubmitButton = nullptr;
  irResultRetryButton = nullptr;
  rankingOverlayPortal = nullptr;
  rankingsButton = nullptr;
  irObservedSnapshotInitialized = false;
  irObservedSnapshotRevision = 0;
  irActionDiagnostic.clear();
  timingAnalyticsView = nullptr;
  courseExitConfirmation = nullptr;
  exportPhotoButton = nullptr;
  exportPhotoButtonText = nullptr;
  practiceSectionButton = nullptr;
  practiceSectionButtonText = nullptr;
  resultPhotoExportInProgress = false;
}
