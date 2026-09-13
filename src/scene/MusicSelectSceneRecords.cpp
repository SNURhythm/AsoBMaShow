#include "MusicSelectScene.h"
#include "MusicSelectRecords.h"
#include "MusicSelectGhostBattle.h"
#include "RecordsDiagnostics.h"
#include "ChartRecordActions.h"
#include "ResultRecordsLoader.h"
#include "RecordsIrActions.h"
#include "RemoteResultRecallController.h"
#include "ResultScene.h"
#include "SceneManager.h"
#include "ChartPreloadWorker.h"
#include "play/GamePlayScene.h"
#include "../ArchiveFile.h"
#include "../LongNoteModeUtils.h"
#include "../PlayOptionUtils.h"
#include "../ReplayAutoPlay.h"
#include "../ReplayResultStateBuilder.h"
#include "../ReplayVideoExporter.h"
#include "../audio/Jukebox.h"
#include "../view/BlockingOverlayView.h"
#include "../replay/ChartReplayConsumer.h"
#include "../replay/CourseReplayConsumer.h"

#include <atomic>
#include <memory>
#include <utility>

void MusicSelectScene::openChartRecords() {
  const auto snapshot = bars_.readView();
  if (snapshot.selectedIndex >= snapshot.rowCount()) return;
  const auto &selected = snapshot.rowAt(snapshot.selectedIndex);
  const auto record = musicSelectRecordsTarget(selected);
  if (!record) return;
  if (recordsModal_ == nullptr) {
    recordsModal_ = ReplayRecordsModal::Create(modalLayer_, makeRecordsModalCallbacks());
  }
  if (!recordFileActions_) {
    recordFileActions_ = std::make_unique<RecordFileActions>(context.replayRepository);
  }
  recordsCourse_ = record->courseStart ? std::optional{selected} : std::nullopt;
  recordsIrRevisions_.clear();
  if (playOptionsModal_ != nullptr) playOptionsModal_->hide();
  if (tasksModal_ != nullptr) tasksModal_->setVisible(false);
  recordsModal_->setTouchVisualizationEnabled(context.settings.touchVisualizationEnabled);
  recordsModal_->showChart(*record);
}

std::vector<ResultRecordSummary>
MusicSelectScene::loadRecordsForSelector(const ChartMetaRecord &record) {
  ResultRecordsLoadOptions options;
  if (record.courseStart && recordsCourse_) {
    options.course = CourseReplayLookup{.courseKey = recordsCourse_->courseKey,
                                        .legacyCourseId = recordsCourse_->courseId};
  } else {
    options.autoPlay = musicSelectAutoPlaySummary(
        record, main_menu_profile::Selections::fromSettings(context.settings),
        {.percent = context.settings.selectedPlaybackRatePercent,
         .mode = context.settings.selectedPlaybackMode});
  }
  const auto provider = context.settings.irProviders.find(std::string(ir::kTachiProviderId));
  options.irServerOrigin = provider == context.settings.irProviders.end()
      ? std::optional<std::string>{ir::kDefaultTachiServerOrigin}
      : ir::normalizeServerOrigin(provider->second.serverOrigin);
  options.irEnabled = provider != context.settings.irProviders.end() && provider->second.enabled;
  options.attemptActivity = [this](std::string_view attemptId) {
    const auto status = context.irSubmissionService
        ? context.irSubmissionService->status(ir::kTachiProviderId, attemptId)
        : ir::IrAttemptStatusSnapshot{};
    return replay_records::recordActivity(status.activeRequest);
  };
  auto loaded = loadResultRecords(context.replayRepository, record, options);
  for (const auto &diagnostic : loaded.diagnostics) {
    if (recordsDiagnostic_ != diagnostic.message) {
      recordsDiagnostic_ = diagnostic.message;
      publishRecordsDiagnostic(diagnostic.message);
    }
  }
  if (loaded.complete) recordsDiagnostic_.clear();
  return std::move(loaded.records);
}

bool MusicSelectScene::beginRecordsOperation(bool resultRecall) {
  if (!sceneActive_ || failed_ || launching_ || recordsTask_.active() ||
      recordsExportJob_.inProgress() ||
      (recordFileActions_ && recordFileActions_->active()) ||
      context.appInBackground.load(std::memory_order_acquire)) {
    return false;
  }
  launching_ = true;
  if (recordsModal_) {
    recordsModal_->setLoadInProgress(!resultRecall);
    recordsModal_->setResultRecallInProgress(resultRecall);
  }
  if (preloadWorker_) preloadWorker_->cancel();
  return true;
}

void MusicSelectScene::startRecordsWork(ReplayRecordTask::Work work,
                                        std::string fallback) {
  recordsTask_.start(
      [this, work = std::move(work), fallback = std::move(fallback)](
          std::shared_ptr<std::atomic_bool> cancelled) {
        try {
          stopPreloadWorker();
          if (!cancelled->load()) work(cancelled);
        } catch (const std::exception &error) {
          const auto diagnostic = replay_records::diagnosticOr(error.what(), fallback);
          recordsTask_.publish([this, diagnostic] { finishRecordsFailure(diagnostic); });
        } catch (...) {
          recordsTask_.publish([this, fallback] { finishRecordsFailure(fallback); });
        }
      });
}

void MusicSelectScene::finishRecordsLoading() {
  launching_ = false;
  if (recordsModal_) {
    recordsModal_->setLoadInProgress(false);
    recordsModal_->setResultRecallInProgress(false);
    recordsModal_->setIrUploadInProgress(false);
  }
}

void MusicSelectScene::publishRecordsDiagnostic(const std::string &diagnostic) const {
  const auto message = ir::sanitizeDiagnostic(diagnostic);
  if (!message.empty()) {
    SDL_Log("Records: %s", message.c_str());
    archive_file::appendDebugLogLine("Records: " + message);
  }
}

void MusicSelectScene::finishRecordsFailure(const std::string &diagnostic) {
  finishRecordsLoading();
  const auto message = replay_records::diagnosticOr(diagnostic, "Result unavailable.");
  publishRecordsDiagnostic(message);
  if (recordsModal_) {
    recordsModal_->reloadRecords(true);
    recordsModal_->setStatus(message);
  }
}

std::optional<course_records::CurrentCourseSelection>
MusicSelectScene::currentRecordsCourseSelection(
    const result_persistence::ModernCourseResult &result) const {
  if (!recordsCourse_) return std::nullopt;
  return course_records::currentCourseSelectionFor(
      recordsCourse_->courseKey, recordsCourse_->courseCharts, result);
}

void MusicSelectScene::launchChartReplay(
    const ChartMetaRecord &record, const ModernChartResultRecord &modern,
    bool ghostBattle) {
  if (record.unavailable || record.solidArchive || record.meta.BmsPath.empty() ||
      !beginRecordsOperation(false)) {
    return;
  }
  const auto selections = main_menu_profile::Selections::fromSettings(context.settings);
  const auto table = musicSelectTableContextForLaunch(bars_.readView());
  const bool autoKeySound = !context.settings.inputKeysoundEnabled;
  const bool clubMode = context.settings.gameplayClubModeEnabled;
  const audio::PlaybackRate playback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode};
  const bool renderTouchPoints = recordsModal_ && recordsModal_->renderTouchPoints();
  const bool renderGhosts = !recordsModal_ || recordsModal_->renderReplayGhosts();
  startRecordsWork(
      [this, record, modern, ghostBattle, selections, autoKeySound, playback, table,
       renderTouchPoints, renderGhosts, clubMode](std::shared_ptr<std::atomic_bool> cancelled) {
        auto consumer = replay::makeRuntimeChartReplayConsumer(context.replayRepository);
        auto loaded = consumer.load(modern, record.meta.BmsPath, *cancelled);
        if (cancelled->load()) return;
        if (!loaded.ready() || !loaded.chart) {
          const auto diagnostic = replay_records::diagnosticOr(
              loaded.diagnostic, "Replay playback could not be prepared.");
          recordsTask_.publish([this, diagnostic] { finishRecordsFailure(diagnostic); });
          return;
        }
        publishRecordsDiagnostic(loaded.diagnostic);
        context.jukebox.stop();
        context.jukebox.loadChart(*loaded.chart, true, *cancelled);
        if (cancelled->load()) return;
        StartOptions options{
            .startPosition = 0,
            .autoKeySound = false,
            .autoPlay = false,
            .gaugeType = loaded.replayData->initialGaugeType,
            .gaugeAutoShift = loaded.replayData->gaugeAutoShift,
            .replayData = loaded.replayData,
            .pacemakerTarget = selections.pacemakerTarget,
            .tableName = table.name,
            .tableLevel = table.level,
            .returnScene = this,
            .touchVisualizationEnabled = renderTouchPoints,
            .replayGhostRenderingEnabled = renderGhosts,
        };
        if (ghostBattle) {
          options = musicSelectGhostBattleOptions(
              loaded.replayData, modern.result.score, selections,
              autoKeySound, playback, this);
          options.clubMode = clubMode;
        } else {
          applyReplayProvenanceToStartOptions(options, *loaded.replayData);
        }
        auto prepared = std::make_shared<decltype(loaded)>(std::move(loaded));
        recordsTask_.publish([this, prepared, options = std::move(options)]() mutable {
          finishRecordsLoading();
          if (recordsModal_) recordsModal_->hide();
          context.sceneManager->changeScene(std::make_unique<GamePlayScene>(
              context, std::move(prepared->chart), std::move(options)), true);
        });
      }, "Replay playback could not be prepared.");
}

void MusicSelectScene::launchChartGhostBattle(
    const ChartMetaRecord &record, const ModernChartResultRecord &modern) {
  if (record.courseStart) return;
  launchChartReplay(record, modern, true);
}

void MusicSelectScene::recallChartResult(
    const ChartMetaRecord &record, const ModernChartResultRecord &modern) {
  if (record.courseStart || !beginRecordsOperation(true)) return;
  const auto pacemaker = context.settings.selectedPacemakerTarget;
  startRecordsWork(
      [this, record, modern, pacemaker](std::shared_ptr<std::atomic_bool> cancelled) {
        auto prepared = chart_records::prepareChartResult(
            context.replayRepository, record, modern.result.attemptId, *cancelled);
        if (cancelled->load()) return;
        if (!prepared.completion) {
          recordsTask_.publish([this, diagnostic = std::move(prepared.diagnostic)] {
            finishRecordsFailure(diagnostic);
          });
          return;
        }
        recordsTask_.publish([this, completion = std::move(prepared.completion), pacemaker]() mutable {
          auto &result = completion->view;
          auto &retryData = completion->retryData;
          const auto meta = result.chart->Meta;
          const auto gameplayGraph = retryData
              ? replay_result::BuildSkinGameplayGraphState(
                    *result.chart, *retryData, result.state)
              : replay_result::BuildSkinGameplayChartGraphState(*result.chart, result.state);
          auto scene = std::make_unique<ResultScene>(
              context, meta, result.state, result.result.score.provenance, nullptr,
              ResultPersistenceOptions{}, retryData.get(),
              ResultPracticeOptions{.returnScene = this}, false, ResultCourseOptions{},
              pacemaker, std::move(result.chart), nullptr, std::nullopt, retryData.get(),
              result.result.attemptId, retryData != nullptr, ResultTableContext{},
              gameplayGraph, result.result.playedAtUnixMillis);
          finishRecordsLoading();
          context.jukebox.stop();
          context.sceneManager->changeScene(std::move(scene), true);
        });
      }, "Saved chart result could not be recalled.");
}

void MusicSelectScene::launchCourseReplay(
    const ChartMetaRecord &record, const ModernCourseResultRecord &modern) {
  if (!record.courseStart) return;
  const auto selection = currentRecordsCourseSelection(modern.result);
  if (!selection) {
    finishRecordsFailure("current course charts are unavailable");
    return;
  }
  if (!beginRecordsOperation(false)) return;
  const auto pacemaker = context.settings.selectedPacemakerTarget;
  const auto table = musicSelectTableContextForLaunch(bars_.readView());
  const bool renderTouchPoints = recordsModal_ && recordsModal_->renderTouchPoints();
  const bool renderGhosts = !recordsModal_ || recordsModal_->renderReplayGhosts();
  startRecordsWork(
      [this, modern, paths = selection->completedChartPaths, pacemaker, table,
       renderTouchPoints, renderGhosts](std::shared_ptr<std::atomic_bool> cancelled) {
        auto consumer = replay::makeRuntimeCourseReplayConsumer(context.replayRepository);
        auto loaded = consumer.load(modern, paths, *cancelled);
        if (cancelled->load()) return;
        if (!loaded.ready()) {
          const auto diagnostic = replay_records::diagnosticOr(
              loaded.diagnostic, "Course replay playback could not be prepared.");
          recordsTask_.publish([this, diagnostic] { finishRecordsFailure(diagnostic); });
          return;
        }
        publishRecordsDiagnostic(loaded.diagnostic);
        auto session = replay::makeCourseReplayLaunchSession(
            std::move(loaded), replay::CourseReplayLaunchMode::Watch,
            renderTouchPoints, renderGhosts);
        if (!session || !session->hasCourseReplayStage(session->currentIndex)) {
          recordsTask_.publish([this] {
            finishRecordsFailure("Prepared course replay session is unavailable.");
          });
          return;
        }
        auto stageReplay = session->currentCourseReplayStageReplay();
        auto chart = session->takePreparedCourseChart(session->currentIndex);
        if (!stageReplay || !chart) {
          recordsTask_.publish([this] {
            finishRecordsFailure("Prepared course replay chart is unavailable.");
          });
          return;
        }
        session->applyReplayStagePlayOptions(*stageReplay);
        context.jukebox.stop();
        context.jukebox.loadChart(*chart, true, *cancelled);
        if (cancelled->load()) return;
        StartOptions options = makeCourseReplayStageStartOptions(session, stageReplay);
        options.pacemakerTarget = pacemaker;
        options.tableName = table.name;
        options.tableLevel = table.level;
        options.returnScene = this;
        auto preparedChart = std::make_shared<std::unique_ptr<bms_parser::Chart>>(std::move(chart));
        recordsTask_.publish([this, preparedChart, options = std::move(options)]() mutable {
          finishRecordsLoading();
          if (recordsModal_) recordsModal_->hide();
          context.sceneManager->changeScene(std::make_unique<GamePlayScene>(
              context, std::move(*preparedChart), std::move(options)), true);
        });
      }, "Course replay playback could not be prepared.");
}

void MusicSelectScene::recallCourseResult(
    const ModernCourseResultRecord &modern, bool retrySameAllowed) {
  const auto selection = currentRecordsCourseSelection(modern.result);
  if (!selection) {
    finishRecordsFailure("current course charts are unavailable");
    return;
  }
  if (!beginRecordsOperation(true)) return;
  const auto pacemaker = context.settings.selectedPacemakerTarget;
  startRecordsWork(
      [this, modern, selection, retrySameAllowed, pacemaker](
          std::shared_ptr<std::atomic_bool> cancelled) {
        auto prepared = course_records::prepareCourseResult(
            context.replayRepository, modern.result.attemptId, selection,
            retrySameAllowed, *cancelled);
        if (cancelled->load()) return;
        if (!prepared.session) {
          recordsTask_.publish([this, diagnostic = std::move(prepared.diagnostic)] {
            finishRecordsFailure(diagnostic);
          });
          return;
        }
        recordsTask_.publish([this, session = std::move(prepared.session), pacemaker] {
          const auto &first = session->completedResults.front();
          const auto *replay = session->resultBrowseStageReplay(0);
          auto *chart = session->resultBrowseReplayChart(0);
          if (replay) session->applyReplayStagePlayOptions(*replay);
          auto scene = std::make_unique<ResultScene>(
              context, first.meta, first.state, *session->stageProvenance.front(), replay,
              ResultPersistenceOptions{}, nullptr, ResultPracticeOptions{.returnScene = this},
              false, ResultCourseOptions{.mode = ResultCourseMode::Stage,
                                         .session = session, .savedResultBrowsing = true},
              pacemaker, std::unique_ptr<bms_parser::Chart>{}, chart,
              std::nullopt, replay, std::nullopt, true, ResultTableContext{},
              first.gameplayGraph, session->modernCoursePlayedAtUnixMillis);
          finishRecordsLoading();
          context.jukebox.stop();
          context.sceneManager->changeScene(std::move(scene), true);
        });
      }, "Saved course result could not be recalled.");
}

void MusicSelectScene::launchAutoPlay(const ChartMetaRecord &record) {
  if (record.courseStart || record.unavailable || record.solidArchive ||
      record.meta.BmsPath.empty() || !beginRecordsOperation(false)) return;
  const auto selections = main_menu_profile::Selections::fromSettings(context.settings);
  const audio::PlaybackRate playback{.percent = context.settings.selectedPlaybackRatePercent,
                                     .mode = context.settings.selectedPlaybackMode};
  const bool clubMode = context.settings.gameplayClubModeEnabled;
  auto meta = record.meta;
  meta.RandomSeed.reset();
  meta.RandomPrng.reset();
  meta.RandomValues.clear();
  {
    // Snapshot before startRecordsWork clears the preloaded chart.
    std::lock_guard<std::mutex> lock(preloadMutex_);
    if (preloadedChart_ &&
        fspath_to_path_t(preloadedPath_) == fspath_to_path_t(meta.BmsPath) &&
        fspath_to_path_t(preloadedChart_->Meta.BmsPath) == fspath_to_path_t(meta.BmsPath)) {
      meta.RandomSeed = preloadedChart_->Meta.RandomSeed;
      meta.RandomPrng = preloadedChart_->Meta.RandomPrng;
      meta.RandomValues = preloadedChart_->Meta.RandomValues;
    }
  }
  startRecordsWork([this, meta = std::move(meta), selections, playback, clubMode](std::shared_ptr<std::atomic_bool> cancelled) {
    auto chart = play_options::parseChart(meta, *cancelled, "autoplay");
    if (cancelled->load()) return;
    if (!chart) {
      recordsTask_.publish([this] { finishRecordsFailure("Autoplay chart could not be prepared."); });
      return;
    }
    const auto playInfo = play_options::applySelectedPlayOptions(*chart, selections.playOption);
    applyEffectiveLongNoteModeToChart(*chart, long_note_mode::valueFromId(selections.longNoteMode));
    context.jukebox.stop();
    context.jukebox.loadChart(*chart, true, *cancelled);
    if (cancelled->load()) return;
    StartOptions options{
        .startPosition = 0,
        .autoKeySound = true,
        .autoPlay = true,
        .gaugeType = selections.gaugeType,
        .gaugeAutoShift = selections.gaugeAutoShift,
        .gaugeAutoShiftLowerBound = selections.gaugeAutoShiftLowerBound,
        .playOption = playInfo.option,
        .playOptionSeed = playInfo.seed,
        .playOption2 = playInfo.option2,
        .playOption2Seed = playInfo.seed2,
        .longNoteMode = long_note_mode::valueFromId(selections.longNoteMode),
        .assistOption = selections.assistOption,
        .pacemakerTarget = pacemaker::kTargetOff,
        .playback = playback,
        .clubMode = clubMode,
        .returnScene = this,
        .touchVisualizationEnabled = false,
        .replayGhostRenderingEnabled = false,
        .ruleset = selections.ruleset,
    };
    auto preparedChart = std::make_shared<std::unique_ptr<bms_parser::Chart>>(std::move(chart));
    recordsTask_.publish([this, preparedChart, options = std::move(options)]() mutable {
      auto scene = std::make_unique<GamePlayScene>(context, std::move(*preparedChart), std::move(options));
      finishRecordsLoading();
      if (recordsModal_) recordsModal_->hide();
      context.sceneManager->changeScene(std::move(scene), true);
    });
  }, "Autoplay chart could not be prepared.");
}

bool MusicSelectScene::beginRecordsExport(const std::string &title) {
  if (launching_ || recordsTask_.active() ||
      (recordFileActions_ && recordFileActions_->active()) ||
      !recordsExportJob_.tryBegin()) {
    return false;
  }
  stopPreloadWorker();
  if (recordsModal_ != nullptr) {
    recordsModal_->setExportInProgress(true);
    recordsModal_->showExportProgress(title, "Preparing export");
  }
  return true;
}

void MusicSelectScene::launchChartReplayExport(
    const ChartMetaRecord &record, const ModernChartResultRecord &modern,
    ReplayVideoExportOptions options) {
  if (!beginRecordsExport("Exporting Replay")) {
    return;
  }
  recordsExportJob_.start(std::move(options),
      [this, record, modern](const ReplayVideoExportOptions &options,
                             std::atomic_bool &cancelled) -> ReplayVideoExportResult {
        auto consumer = replay::makeRuntimeChartReplayConsumer(
            context.replayRepository);
        auto loaded = consumer.load(modern, record.meta.BmsPath, cancelled);
        if (cancelled) {
          return {.success = false,
                  .message = "Replay export preparation was cancelled."};
        }
        if (!loaded.ready() || loaded.chart == nullptr) {
          return {.success = false,
                  .message = replay_records::diagnosticOr(
                      loaded.diagnostic,
                      "Replay export playback could not be prepared.")};
        }
        if (!loaded.diagnostic.empty()) {
          publishRecordsDiagnostic(loaded.diagnostic);
        }
        return ReplayVideoExporter::Export(
            context, loaded.chart.get(), *loaded.replayData, options);
      });
}

void MusicSelectScene::launchCourseReplayExport(
    const ModernCourseResultRecord &modern, ReplayVideoExportOptions options) {
  const auto selection = currentRecordsCourseSelection(modern.result);
  if (!selection) {
    finishRecordsFailure("current course charts are unavailable");
    return;
  }
  if (!beginRecordsExport("Exporting Course Replay")) {
    return;
  }
  recordsExportJob_.start(std::move(options),
      [this, modern, paths = selection->completedChartPaths](
          const ReplayVideoExportOptions &options,
          std::atomic_bool &cancelled) -> ReplayVideoExportResult {
        auto consumer = replay::makeRuntimeCourseReplayConsumer(
            context.replayRepository);
        auto loaded = consumer.load(modern, paths, cancelled);
        if (cancelled) {
          return {.success = false,
                  .message = "Course replay export preparation was cancelled."};
        }
        if (!loaded.ready()) {
          return {.success = false,
                  .message = replay_records::diagnosticOr(
                      loaded.diagnostic,
                      "Course replay export playback could not be prepared.")};
        }
        if (!loaded.diagnostic.empty()) {
          publishRecordsDiagnostic(loaded.diagnostic);
        }
        return ReplayVideoExporter::ExportCourseReplay(
            context, std::move(loaded), options);
      });
}

void MusicSelectScene::launchAutoPlayExport(const ChartMetaRecord &record,
                                            ReplayVideoExportOptions options) {
  if (!beginRecordsExport("Exporting Replay")) {
    return;
  }
  const auto selections =
      main_menu_profile::Selections::fromSettings(context.settings);
  if (options.pacemakerTarget.empty()) {
    options.pacemakerTarget = selections.pacemakerTarget;
  }
  const audio::PlaybackRate playback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode};
  const bool clubMode = context.settings.gameplayClubModeEnabled;
  recordsExportJob_.start(std::move(options),
      [this, record, selections, playback, clubMode](
          const ReplayVideoExportOptions &options,
          std::atomic_bool &cancelled) -> ReplayVideoExportResult {
        auto chart = play_options::parseChart(record.meta, cancelled,
                                              "autoplay export");
        if (!chart || cancelled) {
          return {.success = false, .message = "Autoplay export failed."};
        }
        const auto playInfo = play_options::applySelectedPlayOptions(
            *chart, selections.playOption);
        applyEffectiveLongNoteModeToChart(
            *chart, long_note_mode::valueFromId(selections.longNoteMode));
        ReplayData replay = replay_autoplay::BuildReplayData(
            *chart, selections.gaugeType, selections.gaugeAutoShift, playback,
            playInfo.option, playInfo.seed, playInfo.option2, playInfo.seed2,
            selections.assistOption, clubMode,
            selections.gaugeAutoShiftLowerBound, selections.ruleset);
        ReplayVideoExportOptions exportOptions = options;
        exportOptions.renderTouchPoints = false;
        exportOptions.renderReplayGhosts = false;
        return ReplayVideoExporter::Export(
            context, chart.get(), replay, exportOptions);
      });
}

void MusicSelectScene::applyRecordsExportProgress() {
  const auto progress = recordsExportJob_.takeProgress();
  if (progress && recordsModal_ != nullptr) {
    recordsModal_->updateExportProgress(progress->fraction, progress->message);
  }
}

void MusicSelectScene::applyRecordsExportResult() {
  const auto result = recordsExportJob_.takeResult();
  if (!result) {
    return;
  }
  if (recordsModal_ != nullptr) {
    recordsModal_->setExportInProgress(false);
    if (result->success) {
      recordsModal_->returnToList(
          result->message == "Saved to Photos" ? "Saved" : "Exported");
    } else {
      recordsModal_->returnToList(
          replay_records::diagnosticOr(result->message, "Replay export failed."));
    }
  }
}

void MusicSelectScene::recallRemoteResult(IrRemoteRecordId identity,
                                         std::string stableKey) {
  RemoteResultRecallRequest request{.identity = std::move(identity),
                                    .selectedStableKey = std::move(stableKey)};
  if (!recordsModal_ ||
      !remoteResultRecallSelectionMatches(recordsModal_->selection(), request) ||
      !beginRecordsOperation(true)) return;
  startRecordsWork([this, request](std::shared_ptr<std::atomic_bool> cancelled) {
    auto loaded = context.replayRepository.LoadIrRemoteScore(
        request.identity.providerId, request.identity.serverOrigin,
        request.identity.remoteScoreId);
    if (cancelled->load()) return;
    recordsTask_.publish([this, request, loaded = std::move(loaded)]() mutable {
      RemoteResultRecallCallbacks callbacks{
          .selectionStillMatches = [this](const RemoteResultRecallRequest &candidate) {
            return recordsModal_ && remoteResultRecallSelectionMatches(
                recordsModal_->selection(), candidate);
          },
          .loadExact = [&loaded](const IrRemoteRecordId &) { return std::move(loaded); },
          .transition = [this](ResultRemoteOptions remote, bool retain) {
            remote.returnScene = this;
            auto scene = std::make_unique<ResultScene>(context, std::move(remote));
            finishRecordsLoading();
            context.jukebox.stop();
            context.sceneManager->changeScene(std::move(scene), retain);
            return true;
          },
          .failAndReload = [this](std::string diagnostic) { finishRecordsFailure(diagnostic); },
      };
      (void)executeRemoteResultRecall(request, callbacks);
    });
  }, "Synchronized result could not be opened.");
}

void MusicSelectScene::uploadRecord(const ModernChartResultRecord &modern) {
  if (const auto unavailable = replay_records::irUploadUnavailable(context)) {
    if (recordsModal_) recordsModal_->showIrFeedback(*unavailable);
    return;
  }
  if (!beginRecordsOperation(false)) return;
  if (recordsModal_) {
    recordsModal_->setLoadInProgress(false);
    recordsModal_->setIrUploadInProgress(true);
    recordsModal_->showIrFeedback("Preparing IR...");
  }
  // Submission captures UI-owned settings and credentials on the UI thread.
  defer([this, attemptId = modern.result.attemptId] {
    if (!sceneActive_) return true;
    try {
      stopPreloadWorker();
      const auto message = replay_records::uploadSavedResult(context, attemptId);
      finishRecordsLoading();
      if (recordsModal_) {
        recordsModal_->reloadRecords(true);
        recordsModal_->showIrFeedback(replay_records::diagnosticOr(
            message, "IR upload could not be queued."));
      }
    } catch (const std::exception &error) {
      finishRecordsFailure(replay_records::diagnosticOr(
          error.what(), "IR upload could not be prepared."));
    } catch (...) {
      finishRecordsFailure("IR upload could not be prepared.");
    }
    return true;
  }, 1, true);
}

void MusicSelectScene::shareRecord(const replay::ReplayFileActionRequest &request) {
  if (launching_ || recordsTask_.active() || recordsExportJob_.inProgress() ||
      (recordFileActions_ && recordFileActions_->active())) return;
  if (!recordFileActions_) {
    recordFileActions_ = std::make_unique<RecordFileActions>(context.replayRepository);
  }
  const auto feedback = recordFileActions_->share(request);
  if (feedback.failed) publishRecordsDiagnostic(feedback.message);
  if (recordsModal_) {
    recordsModal_->setDocumentHandoffActive(recordFileActions_->active());
    if (feedback.reloadRecords) recordsModal_->reloadRecords(true);
    recordsModal_->setStatus(feedback.message);
  }
}

void MusicSelectScene::removeRecord(const replay::ReplayFileActionRequest &request) {
  if (launching_ || recordsTask_.active() || recordsExportJob_.inProgress() ||
      (recordFileActions_ && recordFileActions_->active())) return;
  if (!recordFileActions_) {
    recordFileActions_ = std::make_unique<RecordFileActions>(context.replayRepository);
  }
  const auto feedback = recordFileActions_->remove(request);
  if (feedback.failed) publishRecordsDiagnostic(feedback.message);
  if (recordsModal_) {
    if (feedback.reloadRecords) recordsModal_->reloadRecords(true);
    recordsModal_->setStatus(feedback.message);
  }
}

void MusicSelectScene::updateRecordServices() {
  if (recordFileActions_) {
    if (const auto feedback = recordFileActions_->poll()) {
      if (feedback->failed) publishRecordsDiagnostic(feedback->message);
      if (recordsModal_) {
        recordsModal_->setDocumentHandoffActive(recordFileActions_->active());
        if (feedback->reloadRecords) recordsModal_->reloadRecords(true);
        recordsModal_->setStatus(feedback->message);
      }
    }
  }
  if (!recordsModal_ || !recordsModal_->isVisible() || !context.irSubmissionService) return;
  const auto accountRevision = context.irAccountEvidenceRevision.load(std::memory_order_acquire);
  bool reload = recordsIrAccountRevision_ != accountRevision;
  recordsIrAccountRevision_ = accountRevision;
  const auto reconciliation = context.irSubmissionService->reconciliationStatus(ir::kTachiProviderId);
  if (reconciliation.phase == ir::IrReconciliationPhase::Succeeded &&
      reconciliation.revision != 0 && recordsIrReconciliationRevision_ != reconciliation.revision) {
    recordsIrReconciliationRevision_ = reconciliation.revision;
    reload = true;
  }
  for (const auto &record : recordsModal_->records()) {
    if (!record.modern) continue;
    const auto &attemptId = record.modern->result.attemptId;
    const auto status = context.irSubmissionService->status(ir::kTachiProviderId, attemptId);
    const auto observed = recordsIrRevisions_.find(attemptId);
    if (observed != recordsIrRevisions_.end() && observed->second == status.revision) continue;
    recordsIrRevisions_[attemptId] = status.revision;
    reload = true;
  }
  if (reload) recordsModal_->reloadRecords(true);
}

ReplayRecordsModalCallbacks MusicSelectScene::makeRecordsModalCallbacks() {
  ReplayRecordsModalCallbacks callbacks;
  callbacks.loadRecords = [this](const ChartMetaRecord &record) {
    return loadRecordsForSelector(record);
  };
  callbacks.watchModernChart =
      [this](const ChartMetaRecord &record,
             const ModernChartResultRecord &modern) {
        launchChartReplay(record, modern);
      };
  callbacks.watchModernCourse =
      [this](const ChartMetaRecord &record,
             const ModernCourseResultRecord &modern) {
        launchCourseReplay(record, modern);
      };
  callbacks.watchAutoPlay = [this](const ChartMetaRecord &record) {
    launchAutoPlay(record);
  };
  callbacks.gbattle = [this](const ChartMetaRecord &record,
                             const ModernChartResultRecord &modern) {
    launchChartGhostBattle(record, modern);
  };
  callbacks.recallModernChart =
      [this](const ChartMetaRecord &record, const ModernChartResultRecord &modern) {
        recallChartResult(record, modern);
      };
  callbacks.recallModernCourse = [this](const ModernCourseResultRecord &modern, bool retrySame) {
    recallCourseResult(modern, retrySame);
  };
  callbacks.recallRemote = [this](const IrRemoteRecordId &identity, const std::string &stableKey) {
    recallRemoteResult(identity, stableKey);
  };
  callbacks.exportModernChart =
      [this](const ChartMetaRecord &record, const ModernChartResultRecord &modern,
             ReplayVideoExportOptions options) {
        launchChartReplayExport(record, modern, options);
      };
  callbacks.exportModernCourse =
      [this](const ModernCourseResultRecord &modern,
             ReplayVideoExportOptions options) {
        launchCourseReplayExport(modern, options);
      };
  callbacks.exportAutoPlay =
      [this](const ChartMetaRecord &record, ReplayVideoExportOptions options) {
        launchAutoPlayExport(record, options);
      };
  callbacks.irUpload = [this](const ModernChartResultRecord &modern) { uploadRecord(modern); };
  callbacks.irStatusFeedback = [this](ir::IrRecordState state) {
    const auto message = replay_records::irStatusFeedback(state);
    if (!message.empty() && recordsModal_) recordsModal_->showIrFeedback(std::string(message));
  };
  callbacks.share = [this](const replay::ReplayFileActionRequest &request) { shareRecord(request); };
  callbacks.remove = [this](const replay::ReplayFileActionRequest &request) { removeRecord(request); };
  return callbacks;
}
