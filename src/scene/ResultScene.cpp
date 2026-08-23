#include "ResultScene.h"
#include "../CourseConstraintUtils.h"
#include "../CourseIdentity.h"
#include "../CoursePlaySession.h"
#include "../PlayOptionUtils.h"
#include "../repositories/ReplayRepository.h"
#include "../replay/CourseReplayConsumer.h"
#include "../ResultImageExporter.h"
#include "../ResultContracts.h"
#include "../ResultPresentationUtils.h"
#include "../repositories/ScoreRepository.h"
#include "../path.h"
#include "../practice/PracticeLaunchRequest.h"
#include "../practice/PracticeResultModel.h"
#include "../view/BlockingOverlayView.h"
#include "../view/Button.h"
#include "../view/ClearLampColors.h"
#include "../view/IconText.h"
#include "../view/OverlayPortal.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"
#include "play/GamePlayScene.h"
#include "play/Pacemaker.h"
#include "ChartViewerScene.h"
#include "PracticeAnalyticsPresentation.h"
#include "PracticeAnalyticsView.h"
#include "RemoteResultRecallController.h"
#include "ResultGaugeHistory.h"
#include "ResultSkinApplicationOverlays.h"
#include "ResultSkinLayering.h"
#include "ResultPhotoExportPresentation.h"
#include "ResultSkinFailurePresentation.h"
#include "ResultTouchControls.h"

#include "../rendering/Color.h"
#include "../rendering/SimpleBatchRenderer.h"
#include "../rendering/common.h"
#include "bgfx/bgfx.h"
#include "../skin/DefaultSkin.h"
#include "../skin/SkinTypes.h"
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include "../skin/GameplaySkinLifecycle.h"
#include "../skin/beatoraja/BgfxSkinTextureDevice.h"
#include "../skin/beatoraja/LuaSkinApplicationAudioBackend.h"
#include "../skin/beatoraja/ResultSkinSession.h"
#endif

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

std::int64_t nowUnixMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void applyModernCoursePersistencePresentation(
    CoursePlaySession &session, ResultPersistenceOptions &presentation) {
  const auto &outcome = *session.modernCoursePersistenceOutcome;
  result_persistence::SaveState presentationState =
      result_persistence::SaveState::Unstaged;
  switch (outcome.state) {
  case replay::CourseResultPersistenceState::SavedWithReplay:
  case replay::CourseResultPersistenceState::SavedWithoutReplay:
    presentationState = result_persistence::SaveState::Saved;
    session.courseScoreSaved = true;
    break;
  case replay::CourseResultPersistenceState::PendingScore:
    presentationState = result_persistence::SaveState::PendingScore;
    break;
  case replay::CourseResultPersistenceState::Retryable:
    presentationState = result_persistence::SaveState::Unstaged;
    break;
  case replay::CourseResultPersistenceState::InvalidAttempt:
    presentationState = result_persistence::SaveState::InvalidAttempt;
    break;
  case replay::CourseResultPersistenceState::IntegrityConflict:
    presentationState = result_persistence::SaveState::UnstagedConflict;
    break;
  }
  presentation.outcome = {
      .state = presentationState,
      .userMessage = outcome.saved()
                         ? std::string{}
                         : std::string(result_persistence::saveStateUserMessage(
                               presentationState)),
      .diagnostic = outcome.diagnostic.empty() ? session.modernCourseDiagnostic
                                               : outcome.diagnostic,
  };
}

std::optional<int> rankingBadPoints(const RhythmState &state) {
  const auto count = [&](Judgement judgement) {
    const auto it = state.judgeCount.find(judgement);
    return it == state.judgeCount.end() ? 0 : it->second;
  };
  return ir::calculateIrBadPoints(count(Bad), count(Poor), count(Kpoor));
}

void drawResultGaugeGraphPrimitive(
    rendering::SimpleBatchRenderer &batch,
    const result_gauge_history::ResultGaugeGraph &graph, float x, float y,
    float w, float h) {
  batch.addRect(x, y, w, h, ui_theme::resultPanelSubtle().toABGR());

  const float padding = 8.0f;
  const float graphX = x + padding;
  const float graphY = y + padding;
  const float graphW = std::max(1.0f, w - padding * 2.0f);
  const float graphH = std::max(1.0f, h - padding * 2.0f);
  const auto pointX = [graphX, graphW](const auto &point) {
    return graphX + point.normalizedX * graphW;
  };
  const auto pointY = [graphY, graphH](const auto &point) {
    return graphY + point.normalizedY * graphH;
  };

  const uint32_t guideColor = ui_theme::hairlineSubtle().toABGR();
  const float guide80Y = graphY + graph.geometry.guide80Y * graphH;
  const float guide30Y = graphY + graph.geometry.guide30Y * graphH;
  batch.addLine(graphX, guide80Y, graphX + graphW, guide80Y, 1.0F, guideColor);
  batch.addLine(graphX, guide30Y, graphX + graphW, guide30Y, 1.0F, guideColor);

  for (const auto &segment : graph.geometry.segments) {
    batch.addLine(pointX(segment.from), pointY(segment.from),
                  pointX(segment.to), pointY(segment.to), 3.0F,
                  segment.to.color.toABGR());
  }

  const float markerRadius = graph.geometry.segments.empty() ? 3.5F : 2.5F;
  for (const auto &marker : graph.geometry.markers) {
    batch.addCircle(pointX(marker), pointY(marker), markerRadius,
                    marker.color.toABGR());
  }
}

class ResultGaugeGraphView final : public View {
public:
  explicit ResultGaugeGraphView(std::vector<ResultGaugeSeries> series)
      : series(std::move(series)) {
    batch.setSubmitView(rendering::ui_view);
    updateGraph();
  }

  void selectNext() {
    selectedIndex =
        result_gauge_history::nextSeriesIndex(series, selectedIndex);
    updateGraph();
  }

  [[nodiscard]] std::size_t seriesCount() const { return series.size(); }

  [[nodiscard]] const std::optional<result_gauge_history::ResultGaugeGraph> &
  graph() const {
    return selectedGraph;
  }

protected:
  [[nodiscard]] bool requiresUiBatchBoundary() const noexcept override {
    return true;
  }

  void renderImpl(RenderContext &context) override {
    const auto &selectedGraph = graph();
    if (!selectedGraph.has_value() || getWidth() <= 0 || getHeight() <= 0) {
      return;
    }
    rendering::setScissorUI(context.scissor.x, context.scissor.y,
                            context.scissor.width, context.scissor.height);
    batch.begin(context.getTransformMatrix());
    drawResultGaugeGraphPrimitive(
        batch, *selectedGraph, static_cast<float>(getX()),
        static_cast<float>(getY()), static_cast<float>(getWidth()),
        static_cast<float>(getHeight()));
    batch.end();
  }

private:
  void updateGraph() {
    selectedGraph = result_gauge_history::graphFor(series, selectedIndex);
  }

  std::vector<ResultGaugeSeries> series;
  std::size_t selectedIndex = 0;
  std::optional<result_gauge_history::ResultGaugeGraph> selectedGraph;
  rendering::SimpleBatchRenderer batch;
};

play_options::PlayModeDisplayLabel
resultPlayModeDisplayLabel(const bms_parser::ChartMeta &meta,
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

bms_parser::ChartMeta
courseResultMetaForSession(const CoursePlaySession &session) {
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
  if (session.courseCarriedGauge() != nullptr) {
    aggregate.restoreGaugeState(*session.courseCarriedGauge());
  }

  aggregate.resetJudgeCounts();
  aggregate.comboBreak = 0;
  aggregate.maxCombo = session.courseMaximumCombo();
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
    aggregate.combo = session.courseCarriedCombo();
  }
  if (session.completedResults.size() < session.entries.size()) {
    aggregate.currentGauge = 0.0f;
    aggregate.gaugeValues[gaugeTypeIndex(aggregate.gaugeType)] = 0.0f;
    aggregate.gaugeSurvivalFailed[gaugeTypeIndex(aggregate.gaugeType)] = true;
  }
  return aggregate;
}

int courseResultClearTypeForSession(const CoursePlaySession &session,
                                    const RhythmState &resultState,
                                    const ScoreProvenance &provenance) {
  const bms_parser::ChartMeta courseMeta = courseResultMetaForSession(session);
  const bool fullCombo = result_presentation::isFullComboCourseResult(
      static_cast<int>(session.completedResults.size()),
      static_cast<int>(session.entries.size()), session.entries.size(),
      resultState, courseMeta);
  return clear_policy::fullComboRankForPlayback(resultState.getClearTypeRank(),
                                                fullCombo, provenance.playback);
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
  replay.maxCombo = session.courseMaximumCombo();
  replay.finalGauge = resultState.currentGauge;
  replay.clearType = resultState.getClearTypeRank();
  replay.completedCharts = static_cast<int>(completedCharts);
  replay.totalCharts = static_cast<int>(totalCharts);
  replay.provenance = provenance;
  replay.clearType =
      courseResultClearTypeForSession(session, resultState, provenance);

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
    const ReplayData *analyticsSource,
    std::optional<std::string> modernReplayAttemptId, bool retrySameAllowed,
    ResultTableContext tableContext)
    : Scene(context),
      source(LocalResultSource{
          .meta = meta,
          .resultState = state,
          .attemptProvenance = attemptProvenance,
          .presentationReplay = replay != nullptr
                                    ? std::optional<ReplayData>(*replay)
                                : std::nullopt,
          .retryData =
              retrySource != nullptr
                  ? std::optional<ReplayData>(*retrySource)
                  : (replay != nullptr ? std::optional<ReplayData>(*replay)
                                       : std::nullopt),
          .analyticsData = analyticsSource != nullptr
                  ? std::optional<ReplayData>(*analyticsSource)
                  : std::nullopt,
          .persistenceOptions = std::move(persistenceOptions),
          .practiceOptions = std::move(practiceOptions),
          .courseOptions = std::move(courseOptions),
          .tableContext = std::move(tableContext),
          .ownedReusableRetryChart = std::move(ownedReusableRetryChart),
          .pacemakerTarget = pacemaker::normalizeTargetId(
              pacemakerTarget.empty() ? context.settings.selectedPacemakerTarget
                  : pacemakerTarget),
          .pacemakerOverride = std::move(pacemakerOverride),
          .modernReplayAttemptId = modernReplayAttemptId,
          .replayResult = replay == nullptr && retrySource != nullptr &&
                          !modernReplayAttemptId.has_value(),
          .retrySameAllowed = retrySameAllowed,
          .autoPlayResult = autoPlayResult ||
              (retrySource != nullptr && retrySource->autoPlay),
      }) {
  auto &local = *localSource();
  local.replayResult = local.replayResult && !local.practiceOptions.enabled &&
                       local.courseOptions.mode == ResultCourseMode::None;
  local.reusableRetryChart = local.ownedReusableRetryChart != nullptr
                                 ? local.ownedReusableRetryChart.get()
                                 : reusableRetryChart;
  const play_options::PlayModeDisplayLabel display =
      resultPlayModeDisplayLabel(local.meta, local.presentationReplay,
                                 local.retryData, local.practiceOptions);
  local.playModeLabel = display.mode;
  local.laneOrderLabel = display.laneOrder;
  if (local.courseOptions.session != nullptr) {
    const auto courseDisplay = play_options::formatPlayModeDisplayLabel(
        local.meta, local.courseOptions.session->playOption,
        local.courseOptions.session->playOptionSeed,
        local.courseOptions.session->playOption2,
        local.courseOptions.session->playOption2Seed);
    local.playModeLabel =
        courseDisplay.mode.empty() ? "COURSE" : courseDisplay.mode;
    local.laneOrderLabel = courseDisplay.laneOrder;
  }
  if (isCourseStageResult()) {
    local.currentClearLabelOverride = "NO PLAY";
    local.currentClearRankOverride = kNoClearTypeRank;
  } else if (isCourseFinalResult()) {
    local.headerDifficultyLabelOverride = "COURSE";
    const auto &session = *local.courseOptions.session;
    const bms_parser::ChartMeta courseMeta =
        courseResultMetaForSession(session);
    const bool fullCombo = result_presentation::isFullComboCourseResult(
        static_cast<int>(session.completedResults.size()),
        static_cast<int>(session.entries.size()), session.entries.size(),
        local.resultState, courseMeta);
    if (fullCombo) {
      const int clearRank = clear_policy::fullComboRankForPlayback(
          local.resultState.getClearTypeRank(), true,
          local.attemptProvenance.playback);
      local.currentClearLabelOverride = clearTypeRankToLabel(clearRank);
      local.currentClearRankOverride = clearRank;
    }
  }
  rebuildLocalPresentation();
  skin = std::make_unique<DefaultSkin>();
}

ResultScene::ResultScene(ApplicationContext &context,
                         ResultRemoteOptions remote)
    : Scene(context), source(makeResultRemoteSource(std::move(remote))) {
  skin = std::make_unique<DefaultSkin>();
}

ResultScene::~ResultScene() = default;

bool ResultScene::startSelectedResultSkin() {
#if !ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  return false;
#else
  const auto appendDiagnostic =
      [this](const skin::SkinEntryId &entry, std::string revisionDigest,
             std::string configurationDigest,
             skin::SkinDiagnostic diagnostic) noexcept {
        if (!context.skinDiagnosticHistory) {
          return;
        }
        try {
          const auto luaLine = diagnostic.source && diagnostic.source->line != 0
                                   ? std::optional<std::uint32_t>(
                                         diagnostic.source->line)
                                   : std::nullopt;
          context.skinDiagnosticHistory->append({
              .entry = entry,
              .revisionDigest = std::move(revisionDigest),
              .configurationDigest = std::move(configurationDigest),
              .phase = skin::SkinDiagnosticPhase::Session,
              .diagnostic = std::move(diagnostic),
              .luaLine = luaLine,
              .frameSerial = std::nullopt,
          });
        } catch (...) {
        }
      };
  if (!context.gameplaySkinLifecycle || !context.skinStorageRoots ||
      !context.skinResourcePreparationService ||
      !context.skinLiveResourceCounters) {
    return false;
  }
  const auto profileId =
      skin::makeSkinProfileId(context.profileManager.activeProfile().id);
  if (!profileId) return false;
  const int skinType = isCourseFinalResult() ? 15 : 7;
  auto acquisition =
      context.gameplaySkinLifecycle->acquireForSkinType(skinType);
  if (acquisition.disposition !=
          skin::GameplaySkinAcquisitionDisposition::Ready ||
      !acquisition.request) {
    if (acquisition.disposition ==
            skin::GameplaySkinAcquisitionDisposition::Failed &&
        acquisition.failure) {
      const auto &failure = *acquisition.failure;
      appendDiagnostic(failure.entry.value_or(skin::SkinEntryId{}),
                       failure.revisionDigest, failure.configurationDigest,
                       failure.diagnostic);
    }
    return false;
  }
  const auto entry = acquisition.request->activation.entry;
  const auto revisionDigest =
      acquisition.request->activation.revision.revision().lowercaseSha256;
  const auto configurationDigest =
      acquisition.request->activation.configurationDigest;
  auto created = skin::ResultSkinSession::create(
      std::move(acquisition.request->activation),
      {.profileId = *profileId,
       .expectedSkinType = skinType,
       .storageRoots = *context.skinStorageRoots,
       .resourcePreparation = *context.skinResourcePreparationService,
       .initialData = makeResultSkinData(),
       .textureDevice = std::make_shared<skin::BgfxSkinTextureDevice>(),
       .audioBackend = skin::createLuaSkinApplicationAudioBackend(
           context.jukebox.audioRuntime(), [&context] {
             return context.settings.audioVideo.audio.masterVolume;
           }, {}, context.skinLiveResourceCounters),
       .liveResourceCounters = context.skinLiveResourceCounters,
       .safetyPolicy = skin::SkinSafetyPolicy(acquisition.request->safetyLevel)});
  if (!created.session) {
    for (auto &diagnostic : created.diagnostics) {
      appendDiagnostic(entry, revisionDigest, configurationDigest,
                       std::move(diagnostic));
    }
    return false;
  }
  resultSkinSession = std::move(created.session);
  resultSkinStartedMicros = nowMicros();
  return true;
#endif
}

LocalResultSource *ResultScene::localSource() noexcept {
  return std::get_if<LocalResultSource>(&source);
}

const LocalResultSource *ResultScene::localSource() const noexcept {
  return std::get_if<LocalResultSource>(&source);
}

RemoteResultSource *ResultScene::remoteSource() noexcept {
  return std::get_if<RemoteResultSource>(&source);
}

const RemoteResultSource *ResultScene::remoteSource() const noexcept {
  return std::get_if<RemoteResultSource>(&source);
}

bool ResultScene::isCourseStageResult() const {
  const auto *local = localSource();
  return local != nullptr &&
         local->courseOptions.mode == ResultCourseMode::Stage &&
         local->courseOptions.session != nullptr;
}

bool ResultScene::isCourseFinalResult() const {
  const auto *local = localSource();
  return local != nullptr &&
         local->courseOptions.mode == ResultCourseMode::CourseResult &&
         local->courseOptions.session != nullptr;
}

std::optional<ResultPacemakerData>
ResultScene::pacemakerDataForCurrentResult() const {
  const auto *local = localSource();
  if (local == nullptr) {
    return std::nullopt;
  }
  if (isCourseStageResult() || isCourseFinalResult()) {
    return std::nullopt;
  }
  if (local->pacemakerOverride.has_value()) {
    return local->pacemakerOverride;
  }
  return result_presentation::pacemakerDataForResult(
      local->meta, local->resultState, local->pacemakerTarget,
      local->previousBest);
}

ResultSkinData ResultScene::makeResultSkinData() const {
  const auto *local = localSource();
  const auto *remote = remoteSource();
  ResultSkinData data = {
      local == nullptr ? nullptr : &local->resultState,
      local == nullptr ? nullptr : &local->meta,
      &context,
  };
  data.playerName = context.profileManager.activeProfile().displayName;
  data.irOnline = std::ranges::any_of(
      context.settings.irProviders,
      [](const auto &provider) { return provider.second.enabled; });
  if (remote != nullptr) {
    data.presentation = &remote->presentation;
    return data;
  }
  if (local == nullptr) {
    return data;
  }
  data.tableName = local->tableContext.tableName;
  data.tableLevel = local->tableContext.tableLevel;
  data.playModeLabel = local->playModeLabel;
  data.laneOrderLabel = local->laneOrderLabel;
  data.difficultyLabel = local->difficultyLabel;
  data.headerDifficultyLabelOverride = local->headerDifficultyLabelOverride;
  if (local->autoPlayResult) {
    data.currentClearLabelOverride = "AUTO PLAY";
  }
  if (local->currentClearLabelOverride.has_value()) {
    data.currentClearLabelOverride = local->currentClearLabelOverride;
  }
  data.currentClearRankOverride = local->currentClearRankOverride;
  if (local->courseOptions.session != nullptr) {
    data.courseTitles = local->courseOptions.session->beatorajaSkinStageTitles();
  }
  data.previousBest = local->previousBest;
  data.previousLampBest = local->previousLampBest;
  data.pacemaker = pacemakerDataForCurrentResult();
  data.presentation = &local->presentation;
  return data;
}

void ResultScene::rebuildLocalPresentation(
    std::optional<practice::ResultModel> analyticsModel) {
  auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  local->presentation = makeLocalResultPresentation(
      local->meta, local->resultState,
      {.playModeLabel = local->playModeLabel,
       .laneOrderLabel = local->laneOrderLabel,
       .difficultyLabel = local->difficultyLabel,
       .headerDifficultyLabelOverride = local->headerDifficultyLabelOverride,
       .currentClearLabelOverride =
           local->autoPlayResult ? std::optional<std::string>("AUTO PLAY")
                                        : local->currentClearLabelOverride,
       .currentClearRankOverride = local->currentClearRankOverride,
       .previousBest = local->previousBest,
       .previousLampBest = local->previousLampBest,
       .pacemaker = pacemakerDataForCurrentResult(),
       .timingAnalytics = std::move(analyticsModel)});
}

bool ResultScene::persistModernCourseResult() {
  auto *local = localSource();
  if (local == nullptr || !isCourseFinalResult() ||
      local->courseOptions.session == nullptr ||
      local->courseOptions.savedResultBrowsing ||
      local->courseOptions.session->courseReplayPlayback) {
    return false;
  }
  auto &session = *local->courseOptions.session;
  if (session.modernCourseAttemptId.empty()) {
    session.modernCourseDiagnostic =
        "Modern course completion has no durable attempt identity.";
    session.modernCoursePersistenceOutcome =
        replay::CourseResultPersistenceOutcome{
            .state = replay::CourseResultPersistenceState::InvalidAttempt,
            .diagnostic = session.modernCourseDiagnostic};
    applyModernCoursePersistencePresentation(session,
                                             local->persistenceOptions);
    return true;
  }

  // Once a live course has entered the modern capture path, every terminal
  // outcome is owned here. In particular, incomplete capture must not fall
  // through to the legacy course score/replay writers below.
  if (session.modernCourseStageResults.empty()) {
    session.modernCourseDiagnostic =
        "Modern course completion capture contains no stages.";
  }

  if (!session.modernCourseAttempt.has_value()) {
    if (session.modernCourseStageResults.size() !=
            session.completedResults.size() ||
        session.modernCourseReplayStages.size() !=
            session.modernCourseStageResults.size()) {
      session.modernCourseDiagnostic =
          "Modern course completion capture is not contiguous.";
    } else {
      if (!course_identity::isCanonicalKey(session.courseKey)) {
        session.courseKey = course_identity::makeCourseKey(session);
      }
      if (session.modernCoursePlayedAtUnixMillis <= 0) {
        session.modernCoursePlayedAtUnixMillis = nowUnixMillis();
      }
      std::vector<result_persistence::ModernCourseEntryFacts> entryFacts;
      entryFacts.reserve(session.entries.size());
      for (const auto &entry : session.entries) {
        entryFacts.push_back({
            .totalNotes = entry.meta.TotalNotes,
            .playLengthMicros =
                std::max<std::int64_t>(0, entry.meta.PlayLength),
        });
      }
      const int courseLongNoteMode =
          normalizeChartLongNoteModeValue(session.longNoteMode);
      result_persistence::ModernCourseResultCapture resultCapture{
          .attemptId = session.modernCourseAttemptId,
          .courseKey = session.courseKey,
          .legacyCourseId = session.courseId,
          .courseName = session.courseName,
          .courseGroupName = session.courseGroupName,
          .constraintJson = session.constraintJson,
          .requestedPlayOption = session.requestedPlayOption,
          .assistOption = assist_options::normalize(session.assistOption),
          .initialGaugeType = session.gaugeType,
          .gaugeProfile = session.gaugeProfile,
          .gaugeAutoShift = session.gaugeAutoShift,
          .gaugeAutoShiftLowerBound = session.gaugeAutoShiftLowerBound,
          .longNoteMode = courseLongNoteMode,
          .clearType = courseResultClearTypeForSession(
              session, local->resultState, local->attemptProvenance),
          .stages = session.modernCourseStageResults,
          .entryFacts = std::move(entryFacts),
          .playedAtUnixMillis = session.modernCoursePlayedAtUnixMillis,
      };
      std::string diagnostic;
      auto result = result_persistence::captureModernCourseResult(resultCapture,
                                                                  diagnostic);
      if (result) {
        replay::CourseReplayCapture replayCapture{
            .result = std::move(*result),
            .stages = session.modernCourseReplayStages,
            .constraints =
                {
                    .beatorajaConstraintIds =
                        beatorajaCourseConstraintIdsFromJson(
                            session.constraintJson),
                    .longNoteMode = courseLongNoteMode,
                },
        };
        session.modernCourseAttempt =
            replay::captureCourseReplayAttempt(replayCapture, diagnostic);
      }
      if (!session.modernCourseAttempt.has_value()) {
        session.modernCourseDiagnostic =
            diagnostic.empty() ? "Modern course result capture failed."
                               : std::move(diagnostic);
      } else if (!diagnostic.empty()) {
        if (!session.modernCourseDiagnostic.empty()) {
          session.modernCourseDiagnostic += "; ";
        }
        session.modernCourseDiagnostic += diagnostic;
      }
    }
  }

  if (!session.modernCourseAttempt.has_value()) {
    session.modernCoursePersistenceOutcome =
        replay::CourseResultPersistenceOutcome{
            .state = replay::CourseResultPersistenceState::InvalidAttempt,
            .diagnostic = session.modernCourseDiagnostic};
  } else {
    session.modernCoursePersistenceOutcome =
        context.persistModernCourse(*session.modernCourseAttempt);
  }
  const auto &outcome = *session.modernCoursePersistenceOutcome;
  applyModernCoursePersistencePresentation(session, local->persistenceOptions);
  SDL_Log("Modern course persistence state=%d diagnostic=%s",
          static_cast<int>(outcome.state),
          local->persistenceOptions.outcome.diagnostic.c_str());
  return true;
}

void ResultScene::loadPreviousBest() {
  auto *local = localSource();
  if (local == nullptr || local->previousBestLoaded) {
    return;
  }
  const auto &persistenceOptions = local->persistenceOptions;
  local->previousBestLoaded = true;
  local->previousBest.reset();
  local->previousLampBest.reset();

  std::optional<std::string> beforeCreatedAt;
  std::optional<std::string> excludeAttemptId;
  if (persistenceOptions.chartAttempt != nullptr &&
      persistenceOptions.chartOutcome.has_value() &&
      persistenceOptions.chartOutcome->durable()) {
    excludeAttemptId = persistenceOptions.chartAttempt->result.attemptId;
  } else if (local->replayResult && local->retryData.has_value() &&
             !local->retryData->autoPlay &&
             !local->retryData->createdAt.empty()) {
    beforeCreatedAt = local->retryData->createdAt;
  }

  const auto best = isCourseFinalResult()
                        ? context.scoreRepository.LoadBestCourseScore(
                              *local->courseOptions.session)
                        : context.scoreRepository.LoadBestScore(
                              local->meta, beforeCreatedAt, excludeAttemptId);
  if (best.has_value()) {
    local->previousBest =
        result_presentation::previousBestDataFromSnapshot(*best);
  }
  if (!isCourseFinalResult()) {
    const auto bestLamp = context.scoreRepository.LoadBestClearScore(
        local->meta, beforeCreatedAt, excludeAttemptId);
    if (bestLamp.has_value()) {
      local->previousLampBest =
          result_presentation::previousBestDataFromSnapshot(*bestLamp);
    }
  }
}

void ResultScene::loadDifficultyLabel() {
  auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  if (isCourseFinalResult()) {
    local->difficultyLabel = "Course";
    return;
  }
  local->difficultyLabel = result_presentation::difficultyLabelForChart(
      context.chartRepository, local->meta);
}

bool ResultScene::persistenceDecisionRequired() const {
  const auto *local = localSource();
  return local != nullptr &&
         local->persistenceOptions.outcome.requiresUserDecision(
             local->persistenceOptions.chartAttempt != nullptr,
             local->persistenceContinueChosen);
}

std::optional<practice::ResultModel>
ResultScene::makeTimingAnalyticsModel() const {
  const auto *local = localSource();
  if (local == nullptr || local->reusableRetryChart == nullptr ||
      isCourseStageResult() || isCourseFinalResult()) {
    return std::nullopt;
  }
  std::span<const ReplayData> completedAttempts;
  std::size_t abandonedAttempts = 0;
  std::array<ReplayData, 1> singleAttempt;
  if (local->practiceOptions.session != nullptr) {
    completedAttempts = local->practiceOptions.session->completedAttempts();
    abandonedAttempts = local->practiceOptions.session->abandonedAttemptCount();
  } else if (local->analyticsData.has_value()) {
    singleAttempt.front() = *local->analyticsData;
    completedAttempts = singleAttempt;
  } else if (local->presentationReplay.has_value()) {
    singleAttempt.front() = *local->presentationReplay;
    singleAttempt.front().autoPlay =
        singleAttempt.front().autoPlay || local->autoPlayResult;
    completedAttempts = singleAttempt;
  } else if (local->retryData.has_value()) {
    singleAttempt.front() = *local->retryData;
    singleAttempt.front().autoPlay =
        singleAttempt.front().autoPlay || local->autoPlayResult;
    completedAttempts = singleAttempt;
  }
  if (completedAttempts.empty()) {
    return std::nullopt;
  }

  return practice::ResultModel(*local->reusableRetryChart, completedAttempts,
                               abandonedAttempts);
}

void ResultScene::addTimingAnalytics(
    std::optional<practice::ResultModel> analyticsModel) {
  View *host = rootLayout == nullptr
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
    host->setHeight(practice_analytics_presentation::kPreferredAnalyticsHeight);
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

  timingAnalyticsView = new PracticeAnalyticsView(std::move(*analyticsModel));
  host->addView(timingAnalyticsView);
}

void ResultScene::addResultPersistenceStatus() {
  auto *local = localSource();
  if (local == nullptr || rootLayout == nullptr) {
    return;
  }
  const auto &persistenceOptions = local->persistenceOptions;

  normalResultActions = rootLayout->findViewByName("resultActions");
  const bool hasPersistenceResult =
      persistenceOptions.chartAttempt != nullptr ||
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
  const bool courseRetryable =
      isCourseFinalResult() && local->courseOptions.session != nullptr &&
      local->courseOptions.session->modernCourseAttempt.has_value() &&
      local->courseOptions.session->modernCoursePersistenceOutcome
          .has_value() &&
      local->courseOptions.session->modernCoursePersistenceOutcome->retryable();
  persistenceRetryButton->setEnabled(courseRetryable ||
                                     (persistenceOptions.chartAttempt != nullptr &&
                                      persistenceOptions.outcome.retryable()));
  actions->addView(persistenceRetryButton);
  persistenceDetailsButton = makeButton(
      "Show Details", ui_theme::infoAction(), ui_theme::infoActionHover(),
      ui_theme::infoActionPressed(), ui_theme::accentBorder(), [this]() {
        const auto *current = localSource();
        if (current == nullptr || persistenceDetailsModalRoot == nullptr) {
          return;
        }
        const std::string_view attemptId =
            current->persistenceOptions.chartAttempt == nullptr
                ? std::string_view{}
                : std::string_view(
                      current->persistenceOptions.chartAttempt->result.attemptId);
        const auto details = result_persistence::saveConflictDetails(
            current->persistenceOptions.outcome, attemptId);
        if (!details.has_value()) {
          return;
        }
        if (persistenceDetailsStateText != nullptr) {
          persistenceDetailsStateText->setText("Save state: " + details->state);
        }
        if (persistenceDetailsReasonText != nullptr) {
          persistenceDetailsReasonText->setText("Reason\n" + details->reason);
        }
        if (persistenceDetailsReferenceText != nullptr) {
          std::string references = details->attemptId.empty()
                  ? "Attempt ID: unavailable"
                  : "Attempt ID: " + details->attemptId;
          if (details->replayId.has_value()) {
            references += "\nReplay ID: " + std::to_string(*details->replayId);
          }
          persistenceDetailsReferenceText->setText(references);
        }
        persistenceDetailsModalRoot->setSize(rendering::window_width,
                                             rendering::window_height);
        persistenceDetailsModalRoot->setVisible(true);
        persistenceDetailsModalRoot->applyYogaLayout();
      });
  const std::string_view attemptId =
      persistenceOptions.chartAttempt == nullptr
          ? std::string_view{}
          : std::string_view(persistenceOptions.chartAttempt->result.attemptId);
  const bool hasConflictDetails = result_persistence::saveConflictDetails(
                                      persistenceOptions.outcome, attemptId)
          .has_value();
  persistenceDetailsButton->setVisible(hasConflictDetails);
  persistenceDetailsButton->setDisplay(hasConflictDetails ? YGDisplayFlex
                                                           : YGDisplayNone);
  actions->addView(persistenceDetailsButton);
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

  persistenceDetailsModalRoot = new BlockingOverlayView(
      0, 0, rendering::window_width, rendering::window_height);
  persistenceDetailsModalRoot->setPositionType(YGPositionTypeAbsolute);
  persistenceDetailsModalRoot->setPosition(Edge::Left, 0);
  persistenceDetailsModalRoot->setPosition(Edge::Top, 0);
  persistenceDetailsModalRoot->setZIndex(2200);
  persistenceDetailsModalRoot->setVisible(false);
  persistenceDetailsModalRoot->setFlexDirection(FlexDirection::Column);
  persistenceDetailsModalRoot->setAlignItems(YGAlignCenter);
  persistenceDetailsModalRoot->setJustifyContent(YGJustifyCenter);
  persistenceDetailsModalRoot->setBackgroundColor(ui_theme::scrim());

  auto *modalPanel = new View();
  const float modalWidth = std::max(
      320.0F,
      std::min(760.0F, static_cast<float>(rendering::window_width) - 64.0F));
  const float modalHeight = std::max(
      420.0F,
      std::min(600.0F, static_cast<float>(rendering::window_height) - 64.0F));
  modalPanel->setWidth(modalWidth)
      ->setHeight(modalHeight)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, 24)
      ->setBackgroundColor(ui_theme::panelStrong())
      ->setCornerRadius(ui_theme::panelRadius())
      ->setShadow(ui_theme::shadow(), ui_theme::kModalShadow)
      ->setBorderColor(ui_theme::hairline())
      ->setBorderWidth(1);

  auto *modalTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  modalTitle->setText("Save Conflict Details");
  modalTitle->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  modalTitle->setHeight(42);
  modalPanel->addView(modalTitle);

  auto *modalIntroduction = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  modalIntroduction->setText(
      "This diagnostic identifies the integrity check that raised the "
      "warning.");
  modalIntroduction->setColor(ui_theme::sdl(ui_theme::textSecondary()));
  modalIntroduction->setWrap(true);
  modalIntroduction->setHeight(50);
  modalPanel->addView(modalIntroduction);

  persistenceDetailsStateText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  persistenceDetailsStateText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  persistenceDetailsStateText->setWrap(true);
  persistenceDetailsStateText->setHeight(34);
  modalPanel->addView(persistenceDetailsStateText);

  persistenceDetailsReasonText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  persistenceDetailsReasonText->setColor(
      ui_theme::sdl(ui_theme::textPrimary()));
  persistenceDetailsReasonText->setWrap(true);
  persistenceDetailsReasonText->setMinHeight(130);
  persistenceDetailsReasonText->setFlexGrow(1);
  persistenceDetailsReasonText->setFlexShrink(1);
  modalPanel->addView(persistenceDetailsReasonText);

  persistenceDetailsReferenceText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 17);
  persistenceDetailsReferenceText->setColor(
      ui_theme::sdl(ui_theme::textMuted()));
  persistenceDetailsReferenceText->setWrap(true);
  persistenceDetailsReferenceText->setHeight(66);
  modalPanel->addView(persistenceDetailsReferenceText);

  auto *modalFooter = new View();
  modalFooter->setFlexDirection(FlexDirection::Row);
  modalFooter->setJustifyContent(YGJustifyFlexEnd);
  modalFooter->setHeight(60);
  modalFooter->setFlexShrink(0);
  auto *closeButton = makeButton(
      "Close", ui_theme::infoAction(), ui_theme::infoActionHover(),
      ui_theme::infoActionPressed(), ui_theme::accentBorder(), [this]() {
        if (persistenceDetailsModalRoot != nullptr) {
          persistenceDetailsModalRoot->setVisible(false);
        }
      });
  closeButton->setSize(160, 60);
  modalFooter->addView(closeButton);
  modalPanel->addView(modalFooter);

  persistenceDetailsModalRoot->addView(modalPanel);
  rootLayout->addView(persistenceDetailsModalRoot);
}

ir::IrResultPresentation ResultScene::makeIrResultPresentation() const {
  const auto *local = localSource();
  if (local == nullptr) {
    return {};
  }
  ir::IrDriverCapabilities capabilities;
  if (const auto driver = context.irDrivers.find(ir::kTachiProviderId)) {
    capabilities = driver->capabilities();
  }
  ir::IrProviderSettings settings;
  if (const auto found =
          context.settings.irProviders.find(std::string(ir::kTachiProviderId));
      found != context.settings.irProviders.end()) {
    settings = found->second;
  }
  ir::IrAttemptStatusSnapshot snapshot;
  if (local->persistenceOptions.irSubmission && context.irSubmissionService) {
    snapshot = context.irSubmissionService->status(
        ir::kTachiProviderId,
        local->persistenceOptions.irSubmission->attemptId);
  }
  std::optional<ir::BuildDraftOutcome> draftOutcome;
  if (local->persistenceOptions.irSubmission) {
    draftOutcome = context.irDrivers.buildDraft(
        ir::kTachiProviderId, *local->persistenceOptions.irSubmission);
  }
  return ir::makeIrResultPresentation(
      {.providerId = std::string(ir::kTachiProviderId),
       .providerDisplayName = "Bokutachi",
       .capabilities = capabilities,
       .settings = std::move(settings),
       .saveOutcome = local->persistenceOptions.outcome,
       .submission = local->persistenceOptions.irSubmission,
       .draftOutcome = std::move(draftOutcome),
       .snapshot = std::move(snapshot)});
}

void ResultScene::addIrResultStatus() {
  if (localSource() == nullptr || rootLayout == nullptr ||
      irResultStatus != nullptr) {
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
  irResultStatusText = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  irResultStatusText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  irResultStatusText->setWrap(true);
  irResultStatusText->setWidthPercent(100.0F);
  copy->addView(irResultStatusText);
  irResultDetailText = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
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
    button->setBackgroundColors(ui_theme::withAlpha(accent, 72),
                                ui_theme::withAlpha(accent, 104),
        ui_theme::withAlpha(accent, 136));
    button->setBorderColors(ui_theme::withAlpha(accent, 170),
                            ui_theme::withAlpha(accent, 210), accent);
    button->setStyledBorderWidth(1);
    button->setOnClickListener(std::move(action));
    return button;
  };
  irResultSubmitButton =
      makeAction("Submit", ui_theme::cyan(), [this]() { submitIrResult(); });
  status->addView(irResultSubmitButton);
  irResultRetryButton =
      makeAction("Retry", ui_theme::lime(), [this]() { retryIrResult(); });
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
  auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  const ir::IrResultPresentation presentation = makeIrResultPresentation();
  if (!force && local->irObservedSnapshotInitialized &&
      presentation.snapshotRevision == local->irObservedSnapshotRevision) {
    return;
  }
  local->irObservedSnapshotInitialized = true;
  local->irObservedSnapshotRevision = presentation.snapshotRevision;
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
    irResultDetailText->setText(local->irActionDiagnostic.empty()
                                    ? presentation.detailText
                                    : local->irActionDiagnostic);
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
  auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  const ir::IrResultPresentation presentation = makeIrResultPresentation();
  if (!presentation.canSubmit || !local->persistenceOptions.irSubmission) {
    return;
  }
  if (!context.irSubmissionService) {
    local->irActionDiagnostic = "The IR submission service is unavailable.";
    updateIrResultPresentation(true);
    return;
  }
  const auto draft = context.irDrivers.buildDraft(
      ir::kTachiProviderId, *local->persistenceOptions.irSubmission);
  if (draft.status != ir::BuildDraftStatus::Built || !draft.draft) {
    local->irActionDiagnostic =
        draft.diagnostic.empty() ? "This result could not be prepared for IR."
                             : ir::sanitizeDiagnostic(draft.diagnostic);
    updateIrResultPresentation(true);
    return;
  }
  const auto enqueued =
      context.irSubmissionService->enqueueManual(*draft.draft);
  if (enqueued.status == ir::IrOutboxInsertStatus::Inserted ||
      enqueued.status == ir::IrOutboxInsertStatus::AlreadyExists ||
      enqueued.status == ir::IrOutboxInsertStatus::AlreadySubmitted) {
    local->irActionDiagnostic.clear();
  } else {
    local->irActionDiagnostic =
        enqueued.diagnostic.empty()
            ? "This result could not be added to the submission queue."
            : ir::sanitizeDiagnostic(enqueued.diagnostic);
  }
  updateIrResultPresentation(true);
}

void ResultScene::retryIrResult() {
  auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  const ir::IrResultPresentation presentation = makeIrResultPresentation();
  if (!presentation.canRetry || presentation.rowId <= 0) {
    return;
  }
  if (!context.irSubmissionService) {
    local->irActionDiagnostic = "The IR submission service is unavailable.";
    updateIrResultPresentation(true);
    return;
  }
  const auto retried = context.irSubmissionService->retry(presentation.rowId);
  if (retried.status == ir::IrOutboxMutationStatus::Updated) {
    local->irActionDiagnostic.clear();
  } else {
    local->irActionDiagnostic =
        retried.diagnostic.empty()
            ? "This submission could not be scheduled for retry."
            : ir::sanitizeDiagnostic(retried.diagnostic);
  }
  updateIrResultPresentation(true);
}

void ResultScene::retryResultPersistence() {
  auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  if (isCourseFinalResult() && local->courseOptions.session != nullptr &&
      local->courseOptions.session->modernCourseAttempt.has_value() &&
      local->courseOptions.session->modernCoursePersistenceOutcome
          .has_value() &&
      local->courseOptions.session->modernCoursePersistenceOutcome
          ->retryable()) {
    (void)persistModernCourseResult();
    loadPreviousBest();
    rebuildLocalPresentation(makeTimingAnalyticsModel());
    defer(
        [this]() {
          refreshResultSummary();
          updateResultPersistencePresentation();
          return true;
        },
        0, true);
    return;
  }
  if (local->persistenceOptions.chartAttempt == nullptr ||
      !local->persistenceOptions.outcome.retryable()) {
    return;
  }
  auto &persistenceOptions = local->persistenceOptions;

  local->persistenceContinueChosen = false;
  std::vector<ir::IrOutboxDraft> automaticDrafts;
  if (persistenceOptions.irSubmission) {
    automaticDrafts = context.irDrivers.buildAutomaticDrafts(
        context.settings.irProviders, *persistenceOptions.irSubmission);
  }
  persistenceOptions.chartOutcome = context.persistModernChart(
      *persistenceOptions.chartAttempt, automaticDrafts);
  persistenceOptions.outcome =
      chartResultPersistencePresentation(*persistenceOptions.chartOutcome);
  if (persistenceOptions.chartOutcome->durable() &&
      !automaticDrafts.empty() &&
      context.irSubmissionService) {
    context.irSubmissionService->notifyOutboxChanged();
  }
  SDL_Log("Result persistence retry state=%d diagnostic=%s",
          static_cast<int>(persistenceOptions.outcome.state),
          persistenceOptions.outcome.diagnostic.c_str());
  local->previousBestLoaded = false;
  loadPreviousBest();
  rebuildLocalPresentation(makeTimingAnalyticsModel());
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
  auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  local->persistenceContinueChosen = true;
  updateResultPersistencePresentation();
}

void ResultScene::updateResultPersistencePresentation() {
  auto *local = localSource();
  if (local == nullptr) {
    return;
  }
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
    persistenceStatusMessage->setText(
        local->persistenceOptions.outcome.userMessage);
  }
  if (persistenceRetryButton != nullptr) {
    const bool courseRetryable =
        isCourseFinalResult() && local->courseOptions.session != nullptr &&
        local->courseOptions.session->modernCourseAttempt.has_value() &&
        local->courseOptions.session->modernCoursePersistenceOutcome
            .has_value() &&
        local->courseOptions.session->modernCoursePersistenceOutcome
            ->retryable();
    persistenceRetryButton->setEnabled(
        courseRetryable || (local->persistenceOptions.chartAttempt != nullptr &&
                            local->persistenceOptions.outcome.retryable()));
  }
  const std::string_view attemptId =
      local->persistenceOptions.chartAttempt == nullptr
          ? std::string_view{}
          : std::string_view(
                local->persistenceOptions.chartAttempt->result.attemptId);
  const bool hasConflictDetails =
      result_persistence::saveConflictDetails(local->persistenceOptions.outcome,
                                              attemptId)
          .has_value();
  if (persistenceDetailsButton != nullptr) {
    const bool showDetails = decisionRequired && hasConflictDetails;
    persistenceDetailsButton->setVisible(showDetails);
    persistenceDetailsButton->setDisplay(showDetails ? YGDisplayFlex
                                                     : YGDisplayNone);
  }
  if ((!decisionRequired || !hasConflictDetails) &&
      persistenceDetailsModalRoot != nullptr) {
    persistenceDetailsModalRoot->setVisible(false);
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
  auto *local = localSource();
  if (local == nullptr || rootLayout == nullptr) {
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

  if (local->replayResult) {
    retryRow->addView(makeButton("Replay", true, true, ui_theme::infoAction(),
                                 ui_theme::infoActionHover(),
                                 ui_theme::infoActionPressed(),
                                 ui_theme::cyan()));
  } else if (local->practiceOptions.enabled) {
    retryRow->addView(
        makeButton("Retry", true, false, ui_theme::primaryAction(),
                                 ui_theme::primaryActionHover(),
                   ui_theme::primaryActionPressed(), ui_theme::cyan()));
  } else {
    const bool canRetrySame =
        local->retrySameAllowed && local->retryData.has_value()
            ? play_options::hasSamePatternRandomization(*local->retryData)
            : (local->retrySameAllowed &&
               play_options::hasSamePatternRandomization(local->meta));
    retryRow->addView(
        makeButton("Retry", !canRetrySame, false, ui_theme::primaryAction(),
                                 ui_theme::primaryActionHover(),
                   ui_theme::primaryActionPressed(), ui_theme::cyan()));
    if (canRetrySame) {
      retryRow->addView(
          makeButton("Retry Same", true, false, ui_theme::successAction(),
                                   ui_theme::successActionHover(),
                     ui_theme::successActionPressed(), ui_theme::lime()));
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
  rankingsButton->setBorderColors(ui_theme::withAlpha(ui_theme::cyan(), 150),
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
  if (local->autoPlayResult) {
    exportPhotoButtonText->setText("AUTO PLAY");
    exportPhotoButton->setOnClickListener([]() {});
    exportPhotoButton->setBackgroundColors(
        ui_theme::control(), ui_theme::control(), ui_theme::control());
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
  practiceSectionButton->setBackgroundColors(ui_theme::successAction(),
                                             ui_theme::successActionHover(),
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

void ResultScene::buildResultTouchControls() {
  if (rootLayout == nullptr || resultTouchControlsOverlay != nullptr) {
    return;
  }
  ResultTouchControlAvailability availability{.back = true};
  const auto *local = localSource();
  const auto *remote = remoteSource();
  if (local != nullptr) {
    if (isCourseStageResult()) {
      availability.next = true;
    } else if (isCourseFinalResult()) {
      const auto &course = local->courseOptions;
      availability.replay = course.session != nullptr &&
                             course.session->courseReplayData != nullptr &&
                             !course.session->courseReplayData->stages.empty();
      availability.retrySame = course.session != nullptr &&
                              course.session->modernCourseResultBrowsing &&
                              course.session->modernCourseRetrySameAllowed;
      availability.exportPhoto = true;
    } else {
      availability.replay = local->replayResult;
      availability.retry = !local->replayResult;
      availability.retrySame = !local->replayResult &&
                              local->retrySameAllowed &&
                              (local->retryData.has_value()
                                   ? play_options::hasSamePatternRandomization(
                                         *local->retryData)
                                   : play_options::hasSamePatternRandomization(
                                         local->meta));
      availability.rankings = true;
      availability.exportPhoto = true;
      availability.selectSection = true;
    }
  } else if (remote != nullptr) {
    availability.rankings = remote->rankingQuery.has_value();
    availability.exportPhoto = true;
  }

  const auto presentation = makeResultTouchControlPresentation(
      {.skinSelected = true,
       .hidden = resultTouchControlsHidden},
      availability);
  if (!presentation.showsControls && !presentation.capturesRestoreTouch) {
    return;
  }

  auto *overlay = new View();
  overlay->setName("resultTouchControlsOverlay");
  overlay->setPositionType(YGPositionTypeAbsolute);
  overlay->setPosition(Edge::Left, 0);
  overlay->setPosition(Edge::Top, 0);
  overlay->setWidth(static_cast<float>(rendering::window_width));
  overlay->setHeight(static_cast<float>(rendering::window_height));
  overlay->setZIndex(1900);

  auto *panel = new View();
  panel->setName("resultTouchControlsPanel");
  panel->setPositionType(YGPositionTypeAbsolute);
  panel->setPosition(Edge::Left, 12.0F);
  panel->setPosition(Edge::Right, 12.0F);
  panel->setPosition(Edge::Bottom, 12.0F);
  panel->setFlexDirection(FlexDirection::Row);
  panel->setAlignItems(YGAlignCenter);
  panel->setJustifyContent(YGJustifyCenter);
  panel->setFlexWrap(YGWrapWrap);
  panel->setGap(8.0F);
  panel->setPadding(Edge::All, 8.0F);
  panel->setCornerRadius(ui_theme::controlRadius());
  panel->setBackgroundColor(Color(8, 16, 24, 96));

  const auto addAction = [this, panel, local](
                             ResultTouchControlAction action) {
    std::string label;
    Color accent = ui_theme::cyan();
    std::function<void()> callback;
    bool enabled = true;
    switch (action) {
    case ResultTouchControlAction::Back:
      label = "Back";
      callback = [this]() {
        const auto *current = localSource();
        if (current != nullptr && isCourseStageResult() &&
            !current->courseOptions.savedResultBrowsing) {
          showCourseExitConfirmation();
        } else {
          exitResult();
        }
      };
      break;
    case ResultTouchControlAction::Retry:
      label = "Retry";
      accent = ui_theme::primaryAction();
      callback = [this, local]() {
        if (local == nullptr) return;
        const bool canRetrySame = local->retrySameAllowed &&
                                  (local->retryData.has_value()
                                       ? play_options::hasSamePatternRandomization(
                                             *local->retryData)
                                       : play_options::hasSamePatternRandomization(
                                             local->meta));
        startRetry(!canRetrySame);
      };
      break;
    case ResultTouchControlAction::RetrySame:
      label = "Retry Same";
      accent = ui_theme::successAction();
      callback = [this]() {
        if (isCourseFinalResult()) {
          startModernCourseRetrySame();
        } else {
          startRetry(true);
        }
      };
      break;
    case ResultTouchControlAction::Replay:
      label = "Replay";
      accent = ui_theme::infoAction();
      callback = [this]() {
        if (isCourseFinalResult()) {
          startCourseReplay();
        } else {
          startReplay();
        }
      };
      break;
    case ResultTouchControlAction::Rankings:
      label = "Rankings";
      accent = ui_theme::infoAction();
      callback = [this]() { openRankings(); };
      enabled = rankingsAvailable();
      break;
    case ResultTouchControlAction::ExportPhoto:
      label = "Export";
      accent = ui_theme::violetAction();
      callback = [this]() { exportPhoto(); };
      break;
    case ResultTouchControlAction::SelectSection:
      label = "Section";
      accent = ui_theme::successAction();
      callback = [this]() { practiceThisSection(); };
      break;
    case ResultTouchControlAction::Next:
      label = "Next";
      accent = ui_theme::successAction();
      callback = [this]() { continueCourse(); };
      break;
    case ResultTouchControlAction::Hide:
      label = "Hide";
      accent = ui_theme::textSecondary();
      callback = [this]() { setResultTouchControlsHidden(true); };
      break;
    }
    auto *button = new Button();
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
    text->setText(label);
    text->setAlign(TextView::CENTER);
    text->setVAlign(TextView::MIDDLE);
    text->setColor(ui_theme::sdl(ui_theme::textPrimary()));
    if (action == ResultTouchControlAction::ExportPhoto) {
      resultTouchExportPhotoText = text;
    }
    button->setContentView(text);
    button->setSize(142, 48);
    button->setCornerRadius(ui_theme::controlRadius());
    button->setBackgroundColors(ui_theme::withAlpha(accent, 76),
                                ui_theme::withAlpha(accent, 112),
                                ui_theme::withAlpha(accent, 152));
    button->setBorderColors(ui_theme::withAlpha(accent, 140),
                            ui_theme::withAlpha(accent, 190), accent);
    button->setStyledBorderWidth(1);
    button->setEnabled(enabled);
    button->setOnClickListener(std::move(callback));
    panel->addView(button);
  };
  for (const auto action : presentation.actions) {
    addAction(action);
  }
  overlay->addView(panel);

  auto *restore = new Button();
  restore->setName("resultTouchControlsRestore");
  restore->setPositionType(YGPositionTypeAbsolute);
  restore->setPosition(Edge::Left, 0);
  restore->setPosition(Edge::Top, 0);
  restore->setWidth(static_cast<float>(rendering::window_width));
  restore->setHeight(static_cast<float>(rendering::window_height));
  restore->setBackgroundColors(Color(0, 0, 0, 0), Color(0, 0, 0, 0),
                               Color(0, 0, 0, 0));
  restore->setOnClickListener(
      [this]() { setResultTouchControlsHidden(false); });
  restore->setVisible(presentation.capturesRestoreTouch);
  restore->setDisplay(presentation.capturesRestoreTouch ? YGDisplayFlex
                                                         : YGDisplayNone);
  overlay->addView(restore);

  resultTouchControlsOverlay = overlay;
  resultTouchControlsPanel = panel;
  resultTouchControlsRestore = restore;
  rootLayout->addView(overlay);
}

void ResultScene::setResultTouchControlsHidden(bool hidden) {
  resultTouchControlsHidden = hidden;
  if (resultTouchControlsPanel == nullptr || resultTouchControlsRestore == nullptr) {
    return;
  }
  resultTouchControlsPanel->setVisible(!hidden);
  resultTouchControlsPanel->setDisplay(hidden ? YGDisplayNone : YGDisplayFlex);
  resultTouchControlsRestore->setVisible(hidden);
  resultTouchControlsRestore->setDisplay(hidden ? YGDisplayFlex
                                                : YGDisplayNone);
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void ResultScene::handleResultSkinRenderFailure() {
#if !ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  return;
#else
  if (resultSkinSession == nullptr || rootLayout == nullptr) {
    return;
  }
  SDL_Log("Result skin rendering failed; restoring application controls.");
  resultSkinSession.reset();
  const auto presentation = makeResultSkinFailurePresentation(true);
  if (presentation.restoreTouchControls) {
    setResultTouchControlsHidden(false);
  }
  if (!presentation.showNotice || resultSkinFailureNotice != nullptr) {
    return;
  }

  auto *notice = new View();
  notice->setName("resultSkinFailureNotice");
  notice->setPositionType(YGPositionTypeAbsolute);
  notice->setPosition(Edge::Left, 24.0F);
  notice->setPosition(Edge::Right, 24.0F);
  notice->setPosition(Edge::Top, 24.0F);
  notice->setFlexDirection(FlexDirection::Row);
  notice->setAlignItems(YGAlignCenter);
  notice->setJustifyContent(YGJustifySpaceBetween);
  notice->setPadding(Edge::All, 16.0F);
  notice->setGap(16.0F);
  notice->setCornerRadius(ui_theme::panelRadius());
  notice->setBackgroundColor(ui_theme::resultPanelStrong());
  notice->setBorderColor(ui_theme::withAlpha(ui_theme::coral(), 210));
  notice->setBorderWidth(1);
  notice->setZIndex(2300);

  auto *message = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  message->setText("Result skin stopped rendering. Application controls are restored.");
  message->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  message->setWrap(true);
  message->setFlexGrow(1.0F);
  notice->addView(message);

  auto *back = new Button();
  auto *backText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  backText->setText("Back");
  backText->setAlign(TextView::CENTER);
  backText->setVAlign(TextView::MIDDLE);
  backText->setColor(ui_theme::sdl(ui_theme::textOn(ui_theme::primaryAction())));
  back->setContentView(backText);
  back->setSize(128, 48);
  back->setCornerRadius(ui_theme::controlRadius());
  back->setBackgroundColors(ui_theme::primaryAction(),
                            ui_theme::primaryActionHover(),
                            ui_theme::primaryActionPressed());
  back->setOnClickListener([this]() { exitResult(); });
  notice->addView(back);

  resultSkinFailureNotice = notice;
  rootLayout->addView(notice);
  rootLayout->applyYogaLayout();
#endif
}

void ResultScene::addRemoteButtons() {
  const auto *remote = remoteSource();
  if (remote == nullptr || rootLayout == nullptr) {
    return;
  }
  const auto actions =
      remoteResultSceneActions(remote->rankingQuery.has_value());
  View *actionHost = rootLayout->findViewByName("resultActions");
  if (actionHost == nullptr) {
    actionHost = rootLayout;
  }
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setAlignItems(YGAlignCenter);
  row->setJustifyContent(YGJustifyCenter);
  row->setFlexWrap(YGWrapWrap);
  row->setGap(14);

  const auto makeButton = [](const std::string &label, const Color &normal,
                             const Color &hover, const Color &pressed,
                             const Color &border,
                             std::function<void()> action) {
    auto *button = new Button();
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
    text->setText(label);
    text->setAlign(TextView::CENTER);
    text->setVAlign(TextView::MIDDLE);
    text->setColor(ui_theme::sdl(ui_theme::textOn(normal)));
    button->setContentView(text);
    button->setOnClickListener(std::move(action));
    button->setSize(232, 64);
    button->setCornerRadius(ui_theme::controlRadius());
    button->setBackgroundColors(normal, hover, pressed);
    button->setBorderColors(ui_theme::withAlpha(border, 150),
                            ui_theme::withAlpha(border, 190),
                            ui_theme::withAlpha(border, 220));
    button->setStyledBorderWidth(1);
    return button;
  };

  if (actions.rankings) {
    rankingsButton =
        makeButton("Rankings", ui_theme::infoAction(),
                   ui_theme::infoActionHover(), ui_theme::infoActionPressed(),
                   ui_theme::cyan(), [this]() { openRankings(); });
    rankingsButton->setEnabled(rankingsAvailable());
    row->addView(rankingsButton);
  }

  if (actions.exportPhoto) {
    exportPhotoButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
    exportPhotoButtonText->setText("Export Photo");
    exportPhotoButtonText->setAlign(TextView::CENTER);
    exportPhotoButtonText->setVAlign(TextView::MIDDLE);
    exportPhotoButtonText->setColor(
        ui_theme::sdl(ui_theme::textOn(ui_theme::violetAction())));
    exportPhotoButton = makeButton(
        "Export Photo", ui_theme::violetAction(), ui_theme::violetActionHover(),
        ui_theme::violetActionPressed(), ui_theme::violetActionHover(),
        [this]() { exportPhoto(); });
    exportPhotoButton->setContentView(exportPhotoButtonText);
    row->addView(exportPhotoButton);
  }
  actionHost->addView(row);
}

void ResultScene::addRemoteIrStatus() {
  const auto *remote = remoteSource();
  if (remote == nullptr || rootLayout == nullptr || irResultStatus != nullptr) {
    return;
  }
  const auto actions =
      remoteResultSceneActions(remote->rankingQuery.has_value());
  if (!actions.readOnlyIrUploaded) {
    return;
  }
  normalResultActions = rootLayout->findViewByName("resultActions");
  auto *status = new View();
  status->setName("irResultStatus");
  status->setWidthPercent(100.0F);
  status->setHeight(72.0F);
  status->setMinHeight(72.0F);
  status->setFlexShrink(0.0F);
  status->setPadding(Edge::All, 12.0F);
  status->setFlexDirection(FlexDirection::Row);
  status->setAlignItems(YGAlignCenter);
  status->setGap(12.0F);
  status->setBackgroundColor(ui_theme::resultPanelStrong());
  status->setCornerRadius(ui_theme::panelRadius());
  status->setBorderColor(ui_theme::withAlpha(ui_theme::lime(), 170));
  status->setBorderWidth(1);

  auto *irLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  irLabel->setName("remoteIrLabel");
  irLabel->setText("IR");
  irLabel->setColor(ui_theme::sdl(ui_theme::lime()));
  irLabel->setWidth(28.0F);
  irLabel->setHeight(32.0F);
  status->addView(irLabel);

  auto *check = new TextView(ui_icons::kFontAwesomeSolidPath, 18);
  check->setName("remoteIrUploadedCheck");
  check->setText(ui_icons::textForCodepoint(0xf00c));
  check->setColor(ui_theme::sdl(ui_theme::lime()));
  check->setWidth(24.0F);
  check->setHeight(32.0F);
  status->addView(check);

  irResultStatusText = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  irResultStatusText->setText("Bokutachi · Uploaded");
  irResultStatusText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  irResultStatusText->setFlex(1.0F);
  irResultStatusText->setHeight(32.0F);
  status->addView(irResultStatusText);

  irResultDetailText = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  irResultDetailText->setName("remoteIrReadOnlyStatus");
  irResultDetailText->setText("Read-only synchronized result");
  irResultDetailText->setColor(ui_theme::sdl(ui_theme::textSecondary()));
  irResultDetailText->setWidth(240.0F);
  irResultDetailText->setHeight(28.0F);
  status->addView(irResultDetailText);

  if (normalResultActions != nullptr) {
    rootLayout->insertViewBefore(status, normalResultActions);
  } else {
    rootLayout->addView(status);
  }
  irResultStatus = status;
}

bool ResultScene::rankingsAvailable() const {
  if (context.irRankingService == nullptr) {
    return false;
  }
  const auto *remote = remoteSource();
  const auto *local = localSource();
  if (local != nullptr && (isCourseStageResult() || isCourseFinalResult())) {
    return false;
  }
  const std::string providerId = remote == nullptr
                                     ? std::string(ir::kTachiProviderId)
                        : remote->providerId;
  const auto driver = context.irDrivers.find(providerId);
  const auto settings = context.settings.irProviders.find(providerId);
  if (driver == nullptr || !driver->capabilities().chartRankings ||
      settings == context.settings.irProviders.end() ||
      !settings->second.enabled) {
    return false;
  }
  if (remote != nullptr) {
    const auto configuredOrigin =
        ir::normalizeServerOrigin(settings->second.serverOrigin);
    return remote->rankingQuery.has_value() && configuredOrigin.has_value() &&
           *configuredOrigin == remote->serverOrigin;
  }
  return local != nullptr &&
         ir::makeBokutachiRankingQuery(local->meta).value.has_value();
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
  const auto *remote = remoteSource();
  const auto *local = localSource();
  const std::string providerId = remote == nullptr
                                     ? std::string(ir::kTachiProviderId)
                        : remote->providerId;
  std::optional<ir::IrChartQuery> chartQuery;
  std::string serverOrigin;
  std::string title;
  std::optional<ir::IrLocalComparison> comparison;
  if (remote != nullptr) {
    chartQuery = remote->rankingQuery;
    serverOrigin = remote->serverOrigin;
    title = remote->score.title.empty() ? "Synchronized chart"
                                        : remote->score.title;
    comparison = ir::IrLocalComparison{
        .label = "This Play",
        .score = remote->score.score,
        .maxScore = result_contract::maximumScoreForNotes(
                        remote->score.noteCount)
                        .value_or(0),
        .clearType = remote->score.lampRank,
        .badPoints = remote->score.badPoints,
        .maxCombo = remote->score.maxCombo,
    };
  } else if (local != nullptr) {
    const auto query = ir::makeBokutachiRankingQuery(local->meta);
    const auto settings = context.settings.irProviders.find(providerId);
    if (query.value && settings != context.settings.irProviders.end()) {
      chartQuery = *query.value;
      serverOrigin = settings->second.serverOrigin;
      title = local->meta.Title.empty() ? "Completed chart" : local->meta.Title;
      comparison = ir::IrLocalComparison{
          .label = "This Play",
          .score = local->resultState.getScore(),
          .maxScore = result_contract::maximumScoreForNotes(
                          local->meta.TotalNotes)
                          .value_or(0),
          .clearType = local->resultState.getClearTypeRank(),
          .badPoints = rankingBadPoints(local->resultState),
          .maxCombo = local->resultState.maxCombo,
      };
    }
  }
  if (!chartQuery.has_value() || serverOrigin.empty()) {
    refreshRankingsButton();
    return;
  }
  if (!rankingsModal) {
    rankingsModal = std::make_unique<ir::IrRankingModal>(
        *rankingOverlayPortal, *context.irRankingService);
  }
  rankingsModal->open({.profileId = context.profileManager.activeProfile().id,
       .providerId = providerId,
       .serverOrigin = serverOrigin,
       .chart = *chartQuery,
       .localComparison = std::move(comparison)},
      std::move(title));
}

void ResultScene::addCourseButtons() {
  auto *local = localSource();
  if (local == nullptr || rootLayout == nullptr) {
    return;
  }
  const auto &courseOptions = local->courseOptions;

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
    auto [replayButton, ignoredText] =
        makeButton("Replay", ui_theme::infoAction(),
                   ui_theme::infoActionHover(), ui_theme::infoActionPressed(),
                   ui_theme::cyan(), [this]() { startCourseReplay(); });
    (void)ignoredText;
    actionHost->addView(replayButton);
  }

  if (isCourseFinalResult() && courseOptions.session != nullptr &&
      courseOptions.session->modernCourseResultBrowsing &&
      courseOptions.session->modernCourseRetrySameAllowed) {
    auto [retrySameButton, ignoredText] =
        makeButton("Retry Same", ui_theme::successAction(),
                   ui_theme::successActionHover(),
                   ui_theme::successActionPressed(), ui_theme::lime(),
                   [this]() { startModernCourseRetrySame(); });
    (void)ignoredText;
    actionHost->addView(retrySameButton);
  }

  auto [photoButton, photoText] =
      makeButton("Export Photo", ui_theme::violetAction(),
      ui_theme::violetActionHover(), ui_theme::violetActionPressed(),
      ui_theme::violetActionHover(), [this]() { exportPhoto(); });
  exportPhotoButton = photoButton;
  exportPhotoButtonText = photoText;
  actionHost->addView(exportPhotoButton);

  if (auto *backButton =
          dynamic_cast<Button *>(rootLayout->findViewByName("backButton"));
      backButton != nullptr) {
    backButton->setOnClickListener([this]() {
      const auto *local = localSource();
      if (local == nullptr) {
        return;
      }
      if (isCourseStageResult()) {
        if (local->courseOptions.savedResultBrowsing) {
          exitResult();
        } else {
          showCourseExitConfirmation();
        }
      } else {
        exitResult();
      }
    });
  }
}

void ResultScene::buildCourseExitConfirmation() {
  const auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  const auto &courseOptions = local->courseOptions;
  if (!isCourseStageResult() || courseOptions.savedResultBrowsing ||
      rootLayout == nullptr) {
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

  auto makeModalButton = [](const std::string &label, Color normal, Color hover,
                            Color pressed, Color border,
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

  row->addView(
      makeModalButton("Cancel", ui_theme::control(), ui_theme::controlHover(),
      ui_theme::controlPressed(), ui_theme::hairlineSubtle(),
      [this]() { hideCourseExitConfirmation(); }));
  row->addView(makeModalButton("Back to Menu", ui_theme::warningAction(),
                               ui_theme::warningActionHover(),
                               ui_theme::warningActionPressed(),
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

bool ResultScene::recordCourseStageRestTime() {
  auto *local = localSource();
  if (local == nullptr || !isCourseStageResult() ||
      local->courseStageRestRecorded ||
      local->courseOptions.session == nullptr ||
      local->courseOptions.savedResultBrowsing ||
      local->courseOptions.session->courseReplayPlayback ||
      local->courseStageResultShownMicros <= 0) {
    return true;
  }

  local->courseStageRestRecorded = true;
  return local->courseOptions.session->recordRestMicrosAfterCurrentStage(
      nowMicros() - local->courseStageResultShownMicros);
}

void ResultScene::exportPhoto() {
  const auto *local = localSource();
  const auto *remote = remoteSource();
  if ((local != nullptr && local->autoPlayResult) || resultPhotoExportInProgress) {
    return;
  }

  if (local == nullptr && remote == nullptr) {
    return;
  }

  resultPhotoExportInProgress = true;
  setResultPhotoExportPresentation(ResultPhotoExportPresentation::Saving);
  ResultImageExportResult result;
  if (remote != nullptr) {
    result = ResultImageExporter::Export(context, remote->presentation);
  } else {
    const std::optional<ResultPacemakerData> pacemaker =
        pacemakerDataForCurrentResult();
    const auto analyticsModel = makeTimingAnalyticsModel();
    result = ResultImageExporter::Export(
        context, local->meta, local->resultState, local->playModeLabel,
        local->laneOrderLabel, local->difficultyLabel, local->previousBest,
        local->previousLampBest,
        local->currentClearLabelOverride, local->currentClearRankOverride,
        local->headerDifficultyLabelOverride, pacemaker, analyticsModel);
  }
  resultPhotoExportInProgress = false;

  if (result.success) {
    setResultPhotoExportPresentation(ResultPhotoExportPresentation::Saved);
    SDL_Log("Result image exported: %s (%s)",
            fspath_to_utf8(result.outputPath).c_str(), result.message.c_str());
  } else {
    setResultPhotoExportPresentation(ResultPhotoExportPresentation::Failed);
    SDL_Log("Result image export failed: %s (%s)", result.message.c_str(),
            fspath_to_utf8(result.outputPath).c_str());
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }

  defer(
      [this]() {
        if (!resultPhotoExportInProgress) {
          setResultPhotoExportPresentation(ResultPhotoExportPresentation::Ready);
          if (rootLayout != nullptr) {
            rootLayout->applyYogaLayout();
          }
        }
        return true;
      },
      result.success ? 1800 : 1400, true);
}

void ResultScene::setResultPhotoExportPresentation(
    ResultPhotoExportPresentation presentation) {
  const std::string label(resultPhotoExportLabel(presentation));
  if (exportPhotoButtonText != nullptr) {
    exportPhotoButtonText->setText(label);
  }
  if (resultTouchExportPhotoText != nullptr) {
    resultTouchExportPhotoText->setText(label);
  }
}

void ResultScene::continueCourse() {
  auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  auto session = local->courseOptions.session;
  if (!isCourseStageResult() || session == nullptr ||
      local->courseTransitionStarted) {
    return;
  }
  local->courseTransitionStarted = true;
  (void)recordCourseStageRestTime();

  if (local->courseOptions.savedResultBrowsing) {
    if (session->currentIndex + 1 >= session->completedResults.size()) {
      showCourseResult();
      return;
    }
    ++session->currentIndex;
    showSavedCourseStage();
    return;
  }

  if (session->courseReplayPlayback) {
    if (!session->hasNextCourseReplayStage()) {
      showCourseResult();
      return;
    }

    session->currentIndex++;
    startCourseReplayStage(session);
    return;
  }

  const float finalGauge = session->courseCarriedGauge() != nullptr
                               ? session->courseCarriedGauge()->currentGauge
                               : local->resultState.currentGauge;
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
  const ReplayData *retrySetup =
      session->courseRetrySameStageSetup(session->currentIndex);
  std::unique_ptr<bms_parser::Chart> nextChart =
      session->takePreparedCourseChart(session->currentIndex);
  if (nextChart == nullptr) {
    try {
      nextChart = retrySetup != nullptr
                      ? play_options::prepareReplayChart(
                            nextMeta->BmsPath, *retrySetup, parseCancelled)
                      : play_options::parseChart(nextMeta->BmsPath,
                                                 parseCancelled, "course");
    } catch (const std::exception &e) {
      SDL_Log("Course parse failed %s: %s",
              fspath_to_utf8(nextMeta->BmsPath).c_str(), e.what());
      archive_file::appendDebugLogLine(
          "Course parse exception: " + fspath_to_utf8(nextMeta->BmsPath) +
          ": " + e.what());
      showCourseResult();
      return;
    }
  }
  if (nextChart == nullptr || parseCancelled) {
    showCourseResult();
    return;
  }
  play_options::PlayOptionReplayInfo playInfo;
  if (retrySetup != nullptr) {
    session->applyReplayStagePlayOptions(*retrySetup);
    playInfo = {.option = retrySetup->playOption,
                .seed = retrySetup->playOptionSeed,
                .option2 = retrySetup->playOption2,
                .seed2 = retrySetup->playOption2Seed};
  } else {
    applyCourseConstraintsToChart(*nextChart, session->constraints);
    playInfo = play_options::applySelectedPlayOptions(
        *nextChart, session->requestedPlayOption);
    applyEffectiveLongNoteModeToChart(*nextChart, session->longNoteMode);
    session->playOption = playInfo.option;
    session->playOptionSeed = playInfo.seed;
    session->playOption2 = playInfo.option2;
    session->playOption2Seed = playInfo.seed2;
  }

  context.jukebox.stop();
  context.jukebox.loadChart(*nextChart, true, parseCancelled);
  if (parseCancelled) {
    showCourseResult();
    return;
  }

  StartOptions nextOptions =
      retrySetup != nullptr
          ? makeCourseRetrySameStageStartOptions(session, *retrySetup)
          : StartOptions{};
  if (retrySetup == nullptr) {
    nextOptions.startPosition = 0;
    nextOptions.autoKeySound = session->autoKeySound;
    nextOptions.autoPlay = false;
    nextOptions.gaugeType = session->gaugeType;
    nextOptions.gaugeProfile = session->gaugeProfile;
    nextOptions.gaugeAutoShift = session->gaugeAutoShift;
    nextOptions.gaugeAutoShiftLowerBound =
        session->gaugeAutoShiftLowerBound;
    nextOptions.playOption = playInfo.option;
    nextOptions.playOptionSeed = playInfo.seed;
    nextOptions.playOption2 = playInfo.option2;
    nextOptions.playOption2Seed = playInfo.seed2;
    nextOptions.longNoteMode = session->longNoteMode;
    nextOptions.assistOption = session->assistOption;
    nextOptions.playback = course_rules::kRequiredPlaybackRate;
    nextOptions.courseSession = session;
    nextOptions.courseConstraints = session->constraints;
    nextOptions.ruleset = session->ruleset;
    nextOptions.requiredRulesetDescriptor = session->rulesetDescriptor;
    nextOptions.ownsChart = true;
  }

  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(nextChart),
                                      std::move(nextOptions)),
      false);
}

void ResultScene::showSavedCourseStage() {
  auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  auto session = local->courseOptions.session;
  if (session == nullptr ||
      session->currentIndex >= session->completedResults.size()) {
    exitResult();
    return;
  }
  const auto &result = session->completedResults[session->currentIndex];
  ScoreProvenance provenance = ScoreProvenance::Legacy();
  if (session->modernCourseResultBrowsing) {
    if (session->currentIndex >= session->stageProvenance.size() ||
        !session->stageProvenance[session->currentIndex].has_value()) {
      exitResult();
      return;
    }
    provenance = *session->stageProvenance[session->currentIndex];
  } else {
    if (session->courseReplayData == nullptr ||
        session->currentIndex >= session->courseReplayData->stages.size()) {
      exitResult();
      return;
    }
    const auto &replay =
        session->courseReplayData->stages[session->currentIndex].replay;
    session->applyReplayStagePlayOptions(replay);
    provenance = replay.provenance;
  }
  context.sceneManager->changeScene(
      std::make_unique<ResultScene>(
          context, result.meta, result.state, provenance, nullptr,
          ResultPersistenceOptions{}, nullptr, ResultPracticeOptions{}, false,
          ResultCourseOptions{.mode = ResultCourseMode::Stage,
                              .session = session,
                              .savedResultBrowsing = true}),
      false);
}

void ResultScene::showCourseResult() {
  auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  auto session = local->courseOptions.session;
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
                              .session = session,
                              .savedResultBrowsing =
                                  local->courseOptions.savedResultBrowsing}),
      false);
}

void ResultScene::startRetry(bool samePattern) {
  auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  const std::optional<practice::Configuration> practiceConfiguration =
      local->practiceOptions.session != nullptr
          ? std::optional<practice::Configuration>(
                local->practiceOptions.session->configuration())
          : std::nullopt;
  const audio::PlaybackRate retryPlayback =
      resultRetryPlayback(local->attemptProvenance, practiceConfiguration);
  const auto retryPracticeSession =
      freshPracticeSessionForRetry(local->practiceOptions.session);
  ReplayData retrySource;
  if (local->retryData.has_value()) {
    retrySource = *local->retryData;
  } else {
    retrySource.chartMeta = local->meta;
    retrySource.randomSeed = local->meta.RandomSeed;
    retrySource.randomPrng = local->meta.RandomPrng;
    retrySource.randomValues = local->meta.RandomValues;
  }
  if (local->practiceOptions.enabled) {
    retrySource.playOption = local->practiceOptions.playOption;
    retrySource.playOptionSeed = local->practiceOptions.playOptionSeed;
    retrySource.playOption2 = local->practiceOptions.playOption2;
    retrySource.playOption2Seed = local->practiceOptions.playOption2Seed;
    retrySource.assistOption = local->practiceOptions.assistOption;
    retrySource.initialGaugeType = practiceConfiguration.has_value()
            ? practiceConfiguration->gaugeType
            : local->practiceOptions.gaugeType;
    retrySource.gaugeAutoShift = local->practiceOptions.gaugeAutoShift;
    retrySource.gaugeAutoShiftLowerBound =
        local->practiceOptions.gaugeAutoShiftLowerBound;
  }

  context.jukebox.stop();
  defer(
      [this, retrySource, samePattern, practiceConfiguration, retryPlayback,
       retryPracticeSession]() {
        auto *local = localSource();
        if (local == nullptr) {
          return true;
        }
        std::atomic_bool parseCancelled = false;
        // A session-backed retry opens the skin practice menu again. That
        // menu owns applying its filters, flip, and player modifiers, so it
        // must always receive a pristine chart rather than the completed,
        // already-mutated attempt.
        const bool sessionBackedPracticeRetry = retryPracticeSession != nullptr;
        const bool reuseCurrentPattern =
            samePattern && !sessionBackedPracticeRetry &&
            local->reusableRetryChart != nullptr;
        std::unique_ptr<bms_parser::Chart> ownedRetryChart;
        bms_parser::Chart *retryChart = nullptr;
        if (reuseCurrentPattern) {
          if (local->ownedReusableRetryChart != nullptr) {
            ownedRetryChart = std::move(local->ownedReusableRetryChart);
            retryChart = ownedRetryChart.get();
          } else {
            retryChart = local->reusableRetryChart;
          }
        } else {
          ownedRetryChart = play_options::parseChartForRetry(
              retrySource, local->meta, parseCancelled, samePattern);
          retryChart = ownedRetryChart.get();
        }
        if (retryChart == nullptr || parseCancelled) {
          return true;
        }

        StartOptions options;
        options.startPosition =
            local->practiceOptions.enabled
                                    ? (practiceConfiguration.has_value()
                                           ? static_cast<unsigned long long>(
                             std::max(0LL, practiceConfiguration->startMicros))
                                           : local->practiceOptions.startPosition)
                                    : 0;
        options.autoKeySound = local->practiceOptions.enabled
                ? (local->practiceOptions.autoPlay ||
                   local->practiceOptions.autoKeySound)
                                    : !context.settings.inputKeysoundEnabled;
        options.autoPlay = local->practiceOptions.enabled
                               ? local->practiceOptions.autoPlay
                                           : false;
        options.gaugeType = practiceConfiguration.has_value()
                                ? practiceConfiguration->gaugeType
                                : retrySource.initialGaugeType;
        options.gaugeAutoShift = practiceConfiguration.has_value()
                ? practiceConfiguration->gaugeAutoShift
                : retrySource.gaugeAutoShift;
        options.gaugeAutoShiftLowerBound =
            practiceConfiguration.has_value()
                ? practiceConfiguration->gaugeAutoShiftLowerBound
                : retrySource.gaugeAutoShiftLowerBound;
        options.longNoteMode = resultRetryLongNoteMode(
            retrySource.chartMeta, local->attemptProvenance);
        options.assistOption = retrySource.assistOption;
        options.clubMode = local->attemptProvenance.clubMode;
        options.pacemakerTarget =
            local->practiceOptions.enabled
                ? pacemaker::kTargetOff
                : pacemaker::normalizeTargetId(
                      context.settings.selectedPacemakerTarget);
        applyResultTableContext(options, local->tableContext);
        options.playback = retryPlayback;
        options.ownsChart = true;
        options.requiredRulesetDescriptor = local->attemptProvenance.ruleset;
        if (const auto completedRuleset =
                gameplayRulesetFromId(local->attemptProvenance.ruleset.id)) {
          options.ruleset = *completedRuleset;
        }
        if (local->practiceOptions.enabled) {
          options.practiceSession = retryPracticeSession;
          options.practiceMenuPreservePlayOptionSeeds = samePattern;
          options.practiceMode = retryPracticeSession == nullptr;
          options.practiceLeadInMicros =
              retryPracticeSession == nullptr
                  ? local->practiceOptions.leadInMicros
                  : 0;
          if (practiceConfiguration.has_value()) {
            options.judgeWindowScalePercent =
                practiceConfiguration->judge.scalePercent;
            options.startingGaugePercent =
                practiceConfiguration->startingGaugePercent;
          }
          options.longNoteMode = local->practiceOptions.longNoteMode;
          options.returnScene = local->practiceOptions.returnScene;
          if (!options.autoPlay) {
            options.practiceGhostCallback =
                local->practiceOptions.practiceGhostCallback;
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
        } else if (sessionBackedPracticeRetry) {
          options.playOption = retrySource.playOption;
          options.playOptionSeed = retrySource.playOptionSeed;
          options.playOption2 = retrySource.playOption2;
          options.playOption2Seed = retrySource.playOption2Seed;
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

        if (!reuseCurrentPattern && !sessionBackedPracticeRetry &&
            retryChart->Meta.IsDP &&
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
              std::make_unique<GamePlayScene>(
                  context, std::move(ownedRetryChart), options),
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
  if (remoteSource() != nullptr) {
    (void)executeRemoteResultBack(
        [this]() { context.sceneManager->changeScene("MainMenu"); });
    return;
  }
  const auto *local = localSource();
  if (local != nullptr && local->practiceOptions.enabled &&
      local->practiceOptions.returnScene != nullptr) {
    context.sceneManager->changeScene(local->practiceOptions.returnScene,
                                      false);
    return;
  }
  context.sceneManager->changeScene("MainMenu");
}

void ResultScene::startReplay() {
  auto *local = localSource();
  if (local == nullptr || !local->retryData.has_value()) {
    return;
  }

  const ReplayData replaySource = *local->retryData;
  const std::filesystem::path chartPath = local->meta.BmsPath;
  context.jukebox.stop();
  defer(
      [this, replaySource, chartPath]() {
        auto *local = localSource();
        if (local == nullptr) {
          return true;
        }
        std::atomic_bool parseCancelled = false;
        std::unique_ptr<bms_parser::Chart> replayChart;
        if (local->ownedReusableRetryChart != nullptr) {
          replayChart = std::move(local->ownedReusableRetryChart);
          local->reusableRetryChart = nullptr;
        } else {
          replayChart = play_options::prepareReplayChart(
              chartPath, replaySource, parseCancelled);
        }
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
        applyResultTableContext(replayOptions, local->tableContext);
        applyReplayProvenanceToStartOptions(replayOptions, *replayData);
        context.sceneManager->changeScene(
            std::make_unique<GamePlayScene>(context, std::move(replayChart),
                                            std::move(replayOptions)),
            false);
        return false;
      },
      0, true);
}

practice::LaunchRequest
ResultScene::makePracticeLaunchRequest(long long startMicros,
                                       long long endMicros) const {
  const auto *local = localSource();
  if (local == nullptr) {
    throw std::logic_error("practice launch is local-result only");
  }
  const practice::LaunchSource source =
      local->practiceOptions.enabled
          ? practice::LaunchSource::PracticeResult
          : ((local->replayResult ||
              (local->modernReplayAttemptId.has_value() &&
               local->retryData.has_value()))
                 ? practice::LaunchSource::ReplayResult
                 : practice::LaunchSource::NormalResult);
  bms_parser::ChartMeta chartMeta = local->meta;
  if (source == practice::LaunchSource::ReplayResult &&
      local->retryData.has_value()) {
    chartMeta =
        practice::mergeReplayLaunchChartMeta(local->meta, *local->retryData);
  }
  practice::LaunchRequest request{
      .chartMeta = chartMeta,
      .startMicros = startMicros,
      .endMicros = endMicros,
      .source = source,
  };
  const ScoreProvenance &rulesetSource =
      source == practice::LaunchSource::ReplayResult &&
              local->retryData.has_value()
          ? local->retryData->provenance
          : local->attemptProvenance;
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
      local->retryData.has_value()) {
    if (local->modernReplayAttemptId.has_value()) {
      request.modernReplayAttemptId = local->modernReplayAttemptId;
    } else {
      request.replayId = local->retryData->id;
    }
    request.replayPlayOptions =
        practice::launchPlayOptionsFromReplay(*local->retryData);
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
  return makePracticeLaunchRequest(selected->startMicros, selected->endMicros);
}

void ResultScene::updatePracticeSectionAction() {
  if (practiceSectionButton == nullptr ||
      practiceSectionButtonText == nullptr) {
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
  const auto *local = localSource();
  if (local == nullptr) {
    return;
  }
  const auto selectedRequest = selectedPracticeLaunchRequest();
  if (!selectedRequest.has_value() ||
      practice::validateLaunchRequest(*selectedRequest).has_value()) {
    updatePracticeSectionAction();
    return;
  }

  practice::LaunchRequest request = *selectedRequest;
  context.jukebox.stop();

  if (request.source == practice::LaunchSource::PracticeResult &&
      local->practiceOptions.returnScene != nullptr) {
    if (auto *viewer = dynamic_cast<ChartViewerScene *>(
            local->practiceOptions.returnScene);
        viewer != nullptr) {
      viewer->setPracticeLaunchRequest(std::move(request));
      context.sceneManager->changeScene(viewer, false);
      return;
    }
  }

  ChartMetaRecord record{
      .meta = request.chartMeta,
      .difficultyTableLabels = local->difficultyLabel,
  };
  const auto randomSeed = request.chartMeta.RandomSeed;
  const auto randomPrng = request.chartMeta.RandomPrng;
  const auto randomValues = request.chartMeta.RandomValues;
  context.sceneManager->changeScene(
      std::make_unique<ChartViewerScene>(context, std::move(record), randomSeed,
                                         randomPrng, randomValues,
          std::move(request)),
      false);
}

void ResultScene::startCourseReplay() {
  auto *local = localSource();
  if (local == nullptr || !isCourseFinalResult() ||
      local->courseOptions.session == nullptr ||
      local->courseOptions.session->courseReplayData == nullptr ||
      local->courseOptions.session->courseReplayData->stages.empty() ||
      local->courseTransitionStarted) {
    return;
  }

  local->courseTransitionStarted = true;
  auto source = local->courseOptions.session->courseReplayData;
  auto replayData = std::make_shared<CourseReplayData>(*source);
  auto replaySession = std::make_shared<CoursePlaySession>();
  replaySession->courseId = replayData->courseId;
  replaySession->courseKey = replayData->courseKey;
  replaySession->courseName = replayData->courseName;
  replaySession->courseGroupName = replayData->courseGroupName;
  replaySession->constraintJson = replayData->constraintJson;
  replaySession->entries.reserve(replayData->stages.size());
  for (const auto &stage : replayData->stages) {
    replaySession->entries.push_back(
        CoursePlayEntry{.meta = stage.replay.chartMeta});
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
      local->courseOptions.session->replayTouchVisualizationEnabled;
  replaySession->replayGhostRenderingEnabled =
      local->courseOptions.session->replayGhostRenderingEnabled;
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
  auto replayChart = session->takePreparedCourseChart(session->currentIndex);
  if (replayChart == nullptr) {
    replayChart = play_options::prepareReplayChart(
        stageReplay->chartMeta.BmsPath, *stageReplay, parseCancelled);
  }
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

  StartOptions options =
      makeCourseReplayStageStartOptions(session, stageReplay);

  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(replayChart),
                                      std::move(options)),
      false);
}

void ResultScene::startModernCourseRetrySame() {
  auto *local = localSource();
  if (local == nullptr || !isCourseFinalResult() ||
      local->courseOptions.session == nullptr ||
      !local->courseOptions.session->modernCourseResultBrowsing ||
      !local->courseOptions.session->modernCourseRetrySameAllowed ||
      local->courseOptions.session->modernCourseAttemptId.empty() ||
      local->courseOptions.session->modernCourseChartPaths.empty() ||
      local->courseTransitionStarted) {
    return;
  }

  const auto exact = context.replayRepository.LoadModernCourseResultByAttempt(
      local->courseOptions.session->modernCourseAttemptId);
  if (exact.status != ModernCourseResultReadStatus::Loaded ||
      !exact.record.has_value()) {
    return;
  }
  std::atomic_bool cancelled = false;
  auto consumer = replay::makeRuntimeCourseReplayConsumer(
      context.replayRepository);
  auto loaded = consumer.load(
      *exact.record, local->courseOptions.session->modernCourseChartPaths,
      cancelled);
  auto retrySession = replay::makeCourseReplayLaunchSession(
      std::move(loaded), replay::CourseReplayLaunchMode::RetrySame);
  if (retrySession == nullptr || cancelled) {
    return;
  }
  retrySession->entries = local->courseOptions.session->entries;
  retrySession->preparedCourseCharts.resize(retrySession->entries.size());
  retrySession->autoKeySound = !context.settings.inputKeysoundEnabled;
  local->courseTransitionStarted = true;
  startModernCourseRetrySameStage(std::move(retrySession));
}

void ResultScene::startModernCourseRetrySameStage(
    std::shared_ptr<CoursePlaySession> session) {
  if (session == nullptr || !session->validCurrentIndex()) {
    return;
  }
  const ReplayData *setup =
      session->courseRetrySameStageSetup(session->currentIndex);
  auto chart = session->takePreparedCourseChart(session->currentIndex);
  if (setup == nullptr || chart == nullptr) {
    return;
  }
  session->applyReplayStagePlayOptions(*setup);
  std::atomic_bool cancelled = false;
  context.jukebox.stop();
  context.jukebox.loadChart(*chart, true, cancelled);
  if (cancelled) {
    return;
  }
  StartOptions options =
      makeCourseRetrySameStageStartOptions(session, *setup);
  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(chart),
                                      std::move(options)),
      false);
}

void ResultScene::init() {
  auto *local = localSource();
  const auto *remote = remoteSource();
  if (local != nullptr) {
    if (isCourseStageResult()) {
      local->courseStageResultShownMicros = nowMicros();
    }
    loadDifficultyLabel();
    loadPreviousBest();
    if (isCourseFinalResult()) {
      (void)persistModernCourseResult();
    }
  }

  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  addView(rootLayout);

  std::optional<practice::ResultModel> analyticsModel;
  std::vector<ResultGaugeSeries> series;
  if (local != nullptr) {
    analyticsModel = makeTimingAnalyticsModel();
    rebuildLocalPresentation(analyticsModel);
    const RhythmState &resultState = local->resultState;
    series = result_gauge_history::seriesFor(resultState);
  } else if (remote != nullptr) {
    series = remote->presentation.gaugeSeries;
  }
  ResultSkinData data = makeResultSkinData();
  data.showTimingAnalytics = analyticsModel.has_value();
  data.showResultGraph = !series.empty();
  const bool selectedResultSkin = startSelectedResultSkin();
  if (!selectedResultSkin) {
    skin->buildLayout("Result", rootLayout, &data);
  }
  if (local != nullptr) {
    const bool hasPersistenceResult =
        local->persistenceOptions.chartAttempt != nullptr ||
        !local->persistenceOptions.outcome.userMessage.empty();
    const auto applicationOverlays = makeResultSkinApplicationOverlays(
        {.selectedSkin = selectedResultSkin,
         .hasPersistenceResult = hasPersistenceResult,
         .courseStage = isCourseStageResult(),
         .savedResultBrowsing = local->courseOptions.savedResultBrowsing});
    if (!selectedResultSkin) {
      addTimingAnalytics(std::move(analyticsModel));
    }
    if (applicationOverlays.showsPersistenceRecovery) {
      addResultPersistenceStatus();
    }
    addIrResultStatus();
    if (isCourseStageResult() || isCourseFinalResult()) {
      if (!selectedResultSkin) {
        addCourseButtons();
      }
      if (applicationOverlays.buildsCourseExitConfirmation) {
        buildCourseExitConfirmation();
      }
    } else if (!selectedResultSkin) {
      addRetryButtons();
    }
  } else if (!selectedResultSkin && remote != nullptr) {
    addRemoteIrStatus();
    addRemoteButtons();
  }

  if (!selectedResultSkin && !isCourseStageResult() && !isCourseFinalResult()) {
    if (auto *backButton =
            dynamic_cast<Button *>(rootLayout->findViewByName("backButton"));
        backButton != nullptr) {
      backButton->setOnClickListener([this]() { exitResult(); });
    }
  }

  rankingOverlayPortal = new OverlayPortal(0, 0, rendering::window_width,
                                           rendering::window_height);
  rankingOverlayPortal->setPositionType(YGPositionTypeAbsolute);
  rankingOverlayPortal->setPosition(Edge::Left, 0);
  rankingOverlayPortal->setPosition(Edge::Top, 0);
  rankingOverlayPortal->setZIndex(2000);
  rootLayout->addView(rankingOverlayPortal);

  if (selectedResultSkin) {
    buildResultTouchControls();
  }

  if (local != nullptr) {
    updateResultPersistencePresentation();
    updateIrResultPresentation(true);
  }

  graphPlaceHolder = selectedResultSkin ? nullptr : rootLayout->findViewByName("graph");
  if (graphPlaceHolder != nullptr) {
    auto *graphView = new ResultGaugeGraphView(std::move(series));
    graphView->setWidthPercent(100.0F)->setFlex(1.0F);
    graphPlaceHolder->addView(graphView);

    auto *gaugeLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 15);
    gaugeLabel->setAlign(TextView::CENTER);
    gaugeLabel->setVAlign(TextView::MIDDLE);
    gaugeLabel->setPositionType(YGPositionTypeAbsolute);
    gaugeLabel->setPosition(Edge::Left, 12);
    gaugeLabel->setPosition(Edge::Top, 12);
    gaugeLabel->setWidth(142);
    gaugeLabel->setHeight(30);
    gaugeLabel->setCornerRadius(6);
    gaugeLabel->setZIndex(2);
    graphPlaceHolder->addView(gaugeLabel);

    const auto updateGaugeLabel = [graphView, gaugeLabel]() {
      const auto &graph = graphView->graph();
      if (!graph.has_value() || !graph->label.has_value()) {
        gaugeLabel->setVisible(false);
        return;
      }
      gaugeLabel->setVisible(true);
      gaugeLabel->setText(graph->label->text);
      gaugeLabel->setBackgroundColor(graph->label->background);
      gaugeLabel->setColor(
          ui_theme::sdl(ui_theme::textOn(graph->label->background)));
    };
    updateGaugeLabel();

    if (graphView->seriesCount() > 1) {
      if (auto *graphButton = dynamic_cast<Button *>(graphPlaceHolder)) {
        graphButton->setOnClickListener([graphView, updateGaugeLabel]() {
              graphView->selectNext();
              updateGaugeLabel();
            });
      }
    }
  }

  rootLayout->applyYogaLayout();

  if (local != nullptr && isCourseStageResult() &&
      local->courseOptions.session != nullptr &&
      local->courseOptions.session->courseReplayPlayback) {
    const long long restMicros =
        local->courseOptions.session->restMicrosAfterCurrentStage();
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
  auto *local = localSource();
  if (local != nullptr) {
    updatePracticeSectionAction();
  }
  if (local != nullptr && local->persistenceOptions.irSubmission &&
      context.irSubmissionService) {
    const auto snapshot = context.irSubmissionService->status(
        ir::kTachiProviderId,
        local->persistenceOptions.irSubmission->attemptId);
    if (!local->irObservedSnapshotInitialized ||
        snapshot.revision != local->irObservedSnapshotRevision) {
      local->irActionDiagnostic.clear();
      updateIrResultPresentation();
    }
  }
  if (rankingsModal) {
    rankingsModal->update();
  }
}

bool ResultScene::renderViewBeforeScene(const View *view) const {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  return view != rootLayout ||
         !shouldRenderResultRootAfterSkin(resultSkinSession != nullptr);
#else
  (void)view;
  return true;
#endif
}

void ResultScene::renderScene() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (resultSkinSession) {
    RenderContext renderContext(context.uiBatchRenderer);
    RenderContext::UiBatchScope uiBatchScope(renderContext);
    const long long elapsedMillis =
        std::max(0LL, (nowMicros() - resultSkinStartedMicros) / 1000LL);
    if (!resultSkinSession->render(
        renderContext, makeResultSkinData(),
        std::max<std::uint64_t>(1, context.currentFrame), elapsedMillis)) {
      handleResultSkinRenderFailure();
    }
  }
#endif
  if (persistenceDetailsModalRoot != nullptr) {
    persistenceDetailsModalRoot->setSize(rendering::window_width,
                                         rendering::window_height);
  }
  if (rankingOverlayPortal != nullptr) {
    rankingOverlayPortal->setSize(rendering::window_width,
                                  rendering::window_height);
  }
}

void ResultScene::cleanupScene() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  resultSkinSession.reset();
#endif
  rankingsModal.reset();
  rootLayout = nullptr;
  graphPlaceHolder = nullptr;
  resultTouchControlsOverlay = nullptr;
  resultTouchControlsPanel = nullptr;
  resultTouchControlsRestore = nullptr;
  resultSkinFailureNotice = nullptr;
  resultTouchControlsHidden = false;
  normalResultActions = nullptr;
  resultPersistenceStatus = nullptr;
  persistenceStatusMessage = nullptr;
  persistenceRetryButton = nullptr;
  persistenceDetailsButton = nullptr;
  persistenceDetailsModalRoot = nullptr;
  persistenceDetailsStateText = nullptr;
  persistenceDetailsReasonText = nullptr;
  persistenceDetailsReferenceText = nullptr;
  irResultStatus = nullptr;
  irResultStatusText = nullptr;
  irResultDetailText = nullptr;
  irResultSubmitButton = nullptr;
  irResultRetryButton = nullptr;
  rankingOverlayPortal = nullptr;
  rankingsButton = nullptr;
  if (auto *local = localSource()) {
    local->irObservedSnapshotInitialized = false;
    local->irObservedSnapshotRevision = 0;
    local->irActionDiagnostic.clear();
  }
  timingAnalyticsView = nullptr;
  courseExitConfirmation = nullptr;
  exportPhotoButton = nullptr;
  exportPhotoButtonText = nullptr;
  practiceSectionButton = nullptr;
  practiceSectionButtonText = nullptr;
  resultPhotoExportInProgress = false;
}
