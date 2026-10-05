#include "../i18n/Localization.h"
#include "SettingsSceneShared.h"
#include "SettingsScenePreviewAuthority.h"
#include "../library/ChartLibraryPlatform.h"
#include "../input/InputCaptureController.h"
#include "../input/RhythmInputHandler.h"
#include "../rendering/common.h"
#include "../GameplayKeyMode.h"
#include "play/BMSRenderer.h"
#include "play/PlayfieldProjection.h"
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include "../ArchiveFile.h"
#include "play/GameplaySkinSessionFactory.h"
#include "play/PlayfieldPresentationCoordinator.h"
#include "../skin/beatoraja/LuaSkinApplicationAudioBackend.h"
#include "../skin/beatoraja/LuaSkinCurlHttpTransport.h"
#endif
#include "play/BeatorajaHiSpeedChart.h"
#include "play/PlayfieldChartVisualModel.h"
#include "play/PlayfieldVisualState.h"
#include "play/RhythmLaneInputController.h"

using namespace settings_scene;

namespace {
constexpr int kPreviewSampleCombo = 24;
constexpr int kPreviewSampleScore = 123456;

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
// The synthetic chart has no BGA; never borrow the song selector's media state.
class PreviewBga final : public IGameplayBgaSubmitter {
public:
  PreparedGameplayBgaFrame prepareVisualFrameAt(std::uint64_t serial, std::int64_t,
                                                const GameplayBgaMissState &) override {
    return {.sequence = serial};
  }
  BgaPreflightResult preflight(const PreparedGameplayBgaFrame &,
                               std::span<const BgaDrawTarget>) override { return {.ready = true}; }
  void commitPrepared(const PreparedGameplayBgaFrame &) noexcept override {}
  void submitPrepared(const PreparedGameplayBgaFrame &, const BgaDrawTarget &) noexcept override {}
  void finalizePrepared(const PreparedGameplayBgaFrame &) noexcept override {}
  void submitFullscreen(const PreparedGameplayBgaFrame &) noexcept override {}
};
PreviewBga previewBga;
#endif

PlayfieldPresentationConfig
previewPresentationConfiguration(const AppSettings &settings,
                                 const bms_parser::Chart &chart) {
  const gameplay_hispeed::State hispeed(
      {.mode = gameplay_hispeed::fixModeFromEncoded(
           static_cast<int>(settings.hispeedFixMode)),
       .durationMilliseconds = settings.visibleTimeDurationMilliseconds,
       .hispeed = settings.gameplayHispeed,
       .margin = settings.hispeedMargin,
       .laneCoverPercent = settings.presentation().noteStartPositionPercent,
       .laneCoverEnabled = settings.presentation().laneCoverEnabled},
      gameplay_hispeed::summarizeChartBpm(chart));
  PlayfieldPresentationConfig configuration{
      .visibleTimeDurationMilliseconds =
          settings.visibleTimeDurationMilliseconds,
      .configuredHispeed = hispeed.hispeed(),
      .visibleTimeUseMilliseconds = settings.visibleTimeUseMilliseconds,
      .hispeedFixMode = settings.hispeedFixMode,
      .playAreaWidth = settings.playAreaWidthForKeyMode(gameplay::presentationKeyMode(chart)),
      .orientation = settings.activePresentationOrientation(),
      .laneLength = settings.presentation().laneLength,
      .laneAngleDegrees = settings.presentation().laneAngleDegrees,
      .scratchLaneOnRight = settings.presentation().scratchLaneOnRight,
      .hideEmptyScratchLane = chart.Meta.KeyMode == 5
          ? settings.presentation().hideEmptyScratchLane5K
          : settings.presentation().hideEmptyScratchLane7K,
      .laneBeamsEnabled = true,
      .laneCoverHispeedFactor = 1.0F,
      .laneCoverEnabled = settings.presentation().laneCoverEnabled,
      .laneBeamLengthPercent = settings.presentation().laneBeamLengthPercent,
      .noteStartPositionPercent = settings.presentation().noteStartPositionPercent,
      .laneBeamClockUsesRenderTime = true,
      .showInvisibleNotes = settings.showInvisibleNotes,
      .markProcessedNotes = settings.markProcessedNotes,
      .judgementIndicatorEnabled = settings.presentation().judgementIndicatorEnabled,
      .judgementIndicatorY = settings.presentation().judgementIndicatorY,
      .judgementIndicatorWidthScale =
          settings.presentation().judgementIndicatorWidthScale,
      .judgementIndicatorHudMode =
          settings.presentation().judgementIndicatorRenderMode ==
          AppSettings::JudgementIndicatorRenderMode::Hud2D,
      .judgementIndicatorRangeMilliseconds =
          settings.presentation().judgementIndicatorRangeMilliseconds,
      .judgementTextVisibility = settings.presentation().judgementTextVisibility,
      .judgementTextY = settings.presentation().judgementTextY,
      .judgementTimingY = settings.presentation().judgementTimingY,
      .judgementTextSizePercent = settings.presentation().judgementTextSizePercent,
      .judgementTextBold = settings.presentation().judgementTextBold,
      .judgementComboSeparated = settings.presentation().judgementComboSeparated,
      .comboTextY = settings.presentation().comboTextY,
      .comboTextSizePercent = settings.presentation().comboTextSizePercent,
      .comboTextBold = settings.presentation().comboTextBold,
      .judgementTimingSizePercent = settings.presentation().judgementTimingSizePercent,
      .judgementTimingBold = settings.presentation().judgementTimingBold,
      .pacemakerDiffY = settings.presentation().pacemakerDiffY,
      .pacemakerDiffSizePercent = settings.presentation().pacemakerDiffSizePercent,
      .pacemakerDiffBold = settings.presentation().pacemakerDiffBold,
      .judgementCounterEnabled = settings.presentation().judgementCounterEnabled,
      .judgementCounterPosition = settings.presentation().judgementCounterPosition,
      .fastSlowCriteria = settings.presentation().judgementTimingFastSlowCriteria,
      .millisecondsCriteria =
          settings.presentation().judgementTimingMillisecondsCriteria,
      .gaugeBarPosition = settings.presentation().gaugeBarPosition,
      .touchVisualizationEnabled = settings.touchVisualizationEnabled,
      .replayGhostRenderingEnabled = false,
  };
  applyPreviewPlayerConfiguration(configuration, settings);
  return configuration;
}

std::vector<const bms_parser::Note *>
previewNoteSources(const bms_parser::Chart &chart) {
  std::vector<const bms_parser::Note *> result;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (const auto *note : timeline->Notes) {
        if (note != nullptr) {
          result.push_back(note);
        }
      }
      for (const auto *note : timeline->InvisibleNotes) {
        if (note != nullptr) {
          result.push_back(note);
        }
      }
      for (const auto *note : timeline->LandmineNotes) {
        if (note != nullptr) {
          result.push_back(note);
        }
      }
    }
  }
  return result;
}
} // namespace

SettingsScene::~SettingsScene() {
  previewSkinStop.request_stop();
  context.profileSwitchBlockers.scene = nullptr;
  libraryTask.stopAndWait();
  archiveCacheMaintenance.stopAndWait();
  stopProfileArchiveWork();
  inputProfileReplacementRegistration.reset();
}

void SettingsScene::startLanePreview() {
  activeTab = SettingsTab::Lane;
  previewActive = true;
  previewPanelPage = 0;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  updateGameplaySkinSettingsController();
#endif
  resetPreviewSimulation();
  ensurePreviewRenderer();
  resetPreviewHudSample();
  lastLayoutWidth = -1;
}

void SettingsScene::stopLanePreview() {
  previewActive = false;
  destroyPreviewInputHandler();
  destroyPreviewRenderer();
  lastLayoutWidth = -1;
}

void SettingsScene::ensurePreviewRenderer() {
  if (previewRendererDirty) {
    destroyPreviewRenderer();
    previewRendererDirty = false;
  }
  if (previewChart == nullptr) {
    previewChart = makePreviewChart(previewKeyMode);
  }
  if (previewRenderer == nullptr && previewChart != nullptr) {
    previewChartVisualModel = std::make_unique<PlayfieldChartVisualModel>(
        buildPlayfieldChartVisualModel(*previewChart, 0));
    previewVisualStateStore = std::make_unique<PlayfieldVisualStateStore>(
        *previewChartVisualModel);
    previewVisualStateStore->setSceneStartMicros(0);
    previewVisualStateStore->setPlayStartMicros(0);
    previewVisualNoteSources = previewNoteSources(*previewChart);
    if (previewVisualNoteSources.size() !=
        previewChartVisualModel->notes.size()) {
      previewVisualNoteSources.clear();
    }
    Judge previewJudge(previewChart->Meta.Rank);
    auto builtIn = std::make_unique<BMSRenderer>(
        previewChart.get(), previewJudge.timingWindows,
        context.settings.visibleTimeDurationMilliseconds, true);
    previewRenderer = builtIn.get();
    previewProjection = std::make_unique<PlayfieldProjection>();
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
    previewPresentation = std::make_unique<PlayfieldPresentationCoordinator>(
        PlayfieldPresentationCoordinatorDependencies{
            .builtIn = std::move(builtIn), .skin = {}, .bga = previewBga});
#else
    previewPresentation = std::move(builtIn);
#endif
    syncPreviewPresentationConfiguration();
    previewPresentationEvents =
        std::make_unique<PlayfieldPresentationEventFanout>(
            *previewVisualStateStore, *previewPresentation);
    resetPreviewHudSample();
    capturePreviewVisualState();
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
    previewSkinStop = std::stop_source{};
    const auto safe = getSafeAreaInsetsUi();
    const skin::UiLogicalRect bounds{
        .x = static_cast<float>(safe.left), .y = static_cast<float>(safe.top),
        .width = static_cast<float>(rendering::window_width - safe.left - safe.right),
        .height = static_cast<float>(rendering::window_height - safe.top - safe.bottom)};
    previewSkinBounds = {bounds.x, bounds.y, bounds.width, bounds.height};
    const auto projection = projectPreviewFrame();
    auto session = createGameplaySkinSession({
        .acquire = context.acquireGameplaySkinForNextChart,
        .storageRoots = context.skinStorageRoots ? &*context.skinStorageRoots : nullptr,
        .resourcePreparation = context.skinResourcePreparationService.get(),
        .builtinImageReader = archive_file::readFileBounded,
        .liveResourceCounters = context.skinLiveResourceCounters,
        .createHttpTransport = [](std::stop_token stop) {
          return skin::createLuaSkinProductionHttpTransport(stop);
        },
        .audioBackend = skin::createLuaSkinApplicationAudioBackend(
            context.jukebox.audioRuntime(), [this] {
              return context.settings.audioVideo.audio.masterVolume;
            }, {}, context.skinLiveResourceCounters),
        .captureLegacyInputGeneration = [this] {
          return context.inputDeviceRegistry.legacyInputGeneration(
              rendering::render_width, rendering::render_height);
        },
        .configurationWrites = context.skinConfigurationWriteQueue.get(),
        .diagnosticHistory = context.skinDiagnosticHistory.get(),
        .stop = previewSkinStop.get_token()}, {
        .keyMode = previewKeyMode,
        .chartModel = previewChartVisualModel.get(),
        .initialState = previewCapturedVisualState.get(),
        .initialProjection = &projection,
        .safeUiBounds = bounds});
    if (session.disposition == GameplaySkinSessionDisposition::Ready) {
      static_cast<PlayfieldPresentationCoordinator *>(previewPresentation.get())
          ->installSkinSession(std::move(session.session));
    } else if (session.disposition == GameplaySkinSessionDisposition::Failed) {
      previewSkinStop.request_stop();
      previewError = session.failure->diagnostic.message;
    }
#endif
    lastLayoutWidth = -1;
  }
}

void SettingsScene::syncPreviewPresentationConfiguration() {
  if (previewChart == nullptr || previewVisualStateStore == nullptr ||
      previewRenderer == nullptr) {
    return;
  }
  const auto configuration =
      previewPresentationConfiguration(context.settings, *previewChart);
  previewVisualStateStore->setConfiguration(configuration);
  previewPresentation->configure(configuration);
  syncPreviewInputLayout();
}

PlayfieldProjectionResult SettingsScene::projectPreviewFrame() {
  const auto &state = *previewCapturedVisualState;
  return previewProjection->project(*previewChartVisualModel, state,
      {.includeInvisibleNotes = state.configuration.showInvisibleNotes,
       .latePoorTimingMicros = previewRenderer->projectionLatePoorTimingMicros(),
       .pmsPoorDestination = previewPresentation->pmsPoorDestinationGeometry(),
       .buildBuiltInPlan = previewPresentation->activeMode() == PresentationMode::BuiltIn,
       .builtInTraversal = previewRenderer->projectionTraversal()});
}

void SettingsScene::renderPreview() {
  if (!previewPresentation || !previewError.empty()) return;
  syncPreviewPresentationConfiguration();
  previewPresentation->refreshGeometry();
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  const auto safe = getSafeAreaInsetsUi();
  const std::array<double, 4> bounds{static_cast<float>(safe.left), static_cast<float>(safe.top),
      static_cast<float>(rendering::window_width - safe.left - safe.right),
      static_cast<float>(rendering::window_height - safe.top - safe.bottom)};
  if (previewSkinBounds != bounds) {
    static_cast<PlayfieldPresentationCoordinator *>(previewPresentation.get())
        ->updateSkinViewportGeometry({bounds[0], bounds[1], bounds[2], bounds[3]});
    previewSkinBounds = bounds;
  }
#endif
  capturePreviewVisualState();
  const auto projection = projectPreviewFrame();
  (void)previewPresentation->prepareFrame(*previewCapturedVisualState, projection);
  RenderContext renderContext(context.uiBatchRenderer);
  RenderContext::UiBatchScope uiBatchScope(renderContext);
  const auto result = previewPresentation->render(renderContext);
  if (result.failure) {
    previewError = result.failure->diagnostic.message;
    lastLayoutWidth = -1;
  }
  syncPreviewInputLayout();
  syncPreviewTouchLayout();
}

void SettingsScene::syncPreviewTouchLayout() {
  if (!previewInputHandler || !previewPresentation) return;
  auto layout = previewPresentation->touchLayout();
  if (layout.lanes.empty() && layout.laneRegions.empty()) return;
  const auto revision = previewPresentation->touchLayoutRevision();
  if (previewTouchRouter && previewTouchLayoutRevision == revision) return;
  previewTouchLayoutRevision = revision;
  const auto now = static_cast<std::int64_t>(SDL_GetTicks64()) * 1000;
  if (previewTouchRouter) {
    (void)previewTouchRouter->updateLayout(std::move(layout), now);
    return;
  }
  previewTouchRouter = std::make_unique<gameplay::RealtimeTouchInputRouter>(0, std::move(layout),
      gameplay::RealtimeTouchInputSink{
          .context = this,
          .emit = [](void *opaque, const gameplay::RealtimeGameplayInput &input) {
            auto &scene = *static_cast<SettingsScene *>(opaque);
            const auto scratches = scene.previewChart->Meta.GetScratchLaneIndices();
            const bool scratch = std::ranges::find(scratches, input.lane) != scratches.end();
            (void)scene.previewInputHandler->applyTouchLane(input.lane,
                input.type == gameplay::RealtimeGameplayInputType::Press,
                scratch ? std::optional<int>(input.backSpin ? -1 : 1) : std::nullopt);
            return true;
          },
          .scratchLongNoteHeld = [](void *opaque, int lane) {
            auto &scene = *static_cast<SettingsScene *>(opaque);
            return std::ranges::any_of(scene.previewVisualNoteSources, [lane](const auto *note) {
              const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
              return longNote && longNote->Lane == lane && longNote->IsHolding;
            });
          }});
}

void SettingsScene::syncPreviewAuthority() {
  if (previewVisualStateStore == nullptr || previewChart == nullptr) {
    return;
  }
  if (previewGaugeRules == nullptr) {
    previewGaugeRules = std::make_unique<GameplayGaugeRules>(
        compileGameplayGaugeRules(kDefaultGameplayRuleset,
                                  previewChart->Meta,
                                  GaugeProfile::Standard));
  }
  const auto laneCover = previewLaneCoverAuthority(context.settings);
  const PlayfieldAuthorityUpdate authority{
      .currentBpm = kPreviewBpm,
      .judgementCounters = previewJudgeCount,
      .judgementFastSlowCounters = previewJudgeFastSlowCount,
      .comboBreak = previewComboBreak,
      .maximumCombo = previewMaximumCombo,
      .stageCombo = previewCombo,
      .stagePassedNotes = previewPassedNotes,
      .gaugeType = GaugeType::Normal,
      .gaugeAutoShift = GaugeAutoShiftMode::None,
      .currentGauge = 74.0F,
      .gaugeRules = *previewGaugeRules,
      .pacemakerTarget = {.enabled = true, .label = "AAA", .finalScore = 888,
                          .maxScore = 1000, .totalNotes = 500},
      .pacemakerStatus = {.enabled = true, .label = "AAA", .currentScore = 180,
                          .targetScore = 168, .finalTargetScore = 888,
                          .maxScore = 1000, .delta = 12, .playedNotes = 100,
                          .totalNotes = 500},
      .playerName = context.profileManager.activeProfile().displayName,
      .irProviderName = gameplaySkinFirstIrProviderName(context.settings.irProviders),
      .irAccountName = context.irAccountNameSnapshot(),
      .modeFilterName = context.settings.skinModeFilterName,
      .sortId = context.settings.skinSortId,
      .difficultyFilterName = context.settings.skinDifficultyFilterName,
      .chartReplicationMode = context.settings.skinChartReplicationMode,
      .skinTargetId = context.settings.skinTargetId,
      .skinTargetList = context.settings.skinTargetList,
      .playOptionLabel = i18n::tr("settings.preview.preview.badge"),
      .currentFramesPerSecond =
          context.currentFramesPerSecond.load(std::memory_order_acquire),
      .applicationUptimeMillis =
          context.applicationUptimeMillis.load(std::memory_order_acquire),
      .gameplayMode = PlayfieldGameplayMode::Play,
      .loadingState = PlayfieldLoadingState::Loaded,
      .laneCoverPercent = laneCover.percent,
      .laneCoverEnabled = laneCover.enabled,
  };
  previewVisualStateStore->applyAuthorityUpdate(authority);
  // Keep the built-in HUD sample available before the first prepared frame.
  if (previewRenderer != nullptr) {
    previewRenderer->setPacemakerTarget(authority.pacemakerTarget);
    previewRenderer->setPacemakerStatus(authority.pacemakerStatus);
  }
}

void SettingsScene::capturePreviewVisualState() {
  if (previewVisualStateStore == nullptr ||
      previewChartVisualModel == nullptr) {
    return;
  }
  syncPreviewAuthority();
  const std::size_t noteCount =
      std::min(previewVisualNoteSources.size(),
               previewChartVisualModel->notes.size());
  std::vector<NotePresentationState> noteStates;
  noteStates.reserve(noteCount);
  for (std::size_t index = 0; index < noteCount; ++index) {
    const auto *source = previewVisualNoteSources[index];
    NotePresentationState noteState{
        .id = previewChartVisualModel->notes[index].id,
        .judged = source->IsPlayed,
        .dead = source->IsDead,
        .playedTimeMicros = source->IsPlayed ? source->PlayedTime
                                             : kPlayfieldTimestampOff,
    };
    if (const auto *longNote =
            dynamic_cast<const bms_parser::LongNote *>(source);
        longNote != nullptr) {
      const auto *head = longNote->IsTail() && longNote->Head != nullptr
                             ? longNote->Head
                             : longNote;
      noteState.longActive = head->IsHolding;
    }
    noteStates.push_back(noteState);
  }
  previewVisualStateStore->setNoteStates(std::move(noteStates));
  const auto clock = previewFrameClock(++previewFrameSerial, previewElapsedMicros);
  if (previewCapturedVisualState == nullptr) {
    previewCapturedVisualState = std::make_unique<PlayfieldVisualState>(
        previewVisualStateStore->capture(clock));
  } else {
    *previewCapturedVisualState = previewVisualStateStore->capture(clock);
  }
}

void SettingsScene::destroyPreviewRenderer() {
  destroyPreviewInputHandler();
  previewPresentationEvents.reset();
  previewSkinStop.request_stop();
  previewRenderer = nullptr;
  previewPresentation.reset();
  previewProjection.reset();
  previewError.clear();
  previewCapturedVisualState.reset();
  previewGaugeRules.reset();
  previewVisualStateStore.reset();
  previewChartVisualModel.reset();
  previewVisualNoteSources.clear();
  previewFrameSerial = 0;
  previewChart.reset();
  previewElapsedMicros = 0;
}

void SettingsScene::ensurePreviewInputHandler() {
  if (!previewActive) {
    return;
  }
  ensurePreviewRenderer();
  if (!previewError.empty() || previewChart == nullptr || previewRenderer == nullptr ||
      previewPresentationEvents == nullptr) {
    return;
  }
  if (previewLaneController == nullptr) {
    previewLaneController = std::make_unique<RhythmLaneInputController>(
        previewChart.get(), previewPresentationEvents.get(),
        previewLanePressed,
        Judge(previewChart->Meta.Rank));
  }
  if (previewInputHandler == nullptr) {
    previewInputHandler = std::make_unique<RhythmInputHandler>(
        this, previewChart->Meta, context.inputDeviceRegistry,
        context.inputProfile,
        makeGameplayInputScopes(gameplay::presentationKeyMode(*previewChart)),
        LogicalGameplayInputAdapter::CommandCallback{},
        context.settings.playAreaWidthForKeyMode(gameplay::presentationKeyMode(*previewChart)),
        LogicalGameplayRegistryPolicy{.acceptKeyboardFromRegistry = false});
    previewInputHandler->discardPendingTouchEvents();
    previewInputHandler->setTouchEventCallback(
        [this](SDL_FingerID finger, ReplayTouchAction action, Vector3 position) {
          if (!previewPresentation) return false;
          if (!previewTouchRouter) return true;
          const auto phase = action == ReplayTouchAction::Down ? gameplay::RealtimeTouchPhase::Down
              : (action == ReplayTouchAction::Up || action == ReplayTouchAction::Cancel)
                  ? gameplay::RealtimeTouchPhase::Up
              : gameplay::RealtimeTouchPhase::Move;
          if (rendering::render_width <= 0 || rendering::render_height <= 0) return true;
          (void)previewTouchRouter->consume({.fingerId = finger, .phase = phase,
              .normalizedX = (position.x * rendering::window_width * rendering::ui_scale_x +
                             rendering::ui_offset_x) / rendering::render_width,
              .normalizedY = (position.y * rendering::window_height * rendering::ui_scale_y +
                             rendering::ui_offset_y) / rendering::render_height,
              .steadyTimestampMicros = static_cast<std::int64_t>(SDL_GetTicks64()) * 1000});
          return true;
        });
    previewInputHandler->startListenSDL();
  }
  syncPreviewInputLayout();
}

void SettingsScene::destroyPreviewInputHandler() {
  if (previewTouchRouter) {
    (void)previewTouchRouter->cancelAll(static_cast<std::int64_t>(SDL_GetTicks64()) * 1000);
    previewTouchRouter.reset();
  }
  previewTouchLayoutRevision = 0;
  if (previewInputHandler != nullptr) {
    previewInputHandler->stopListen();
    previewInputHandler.reset();
  }
  previewLaneController.reset();
  previewLanePressed.clear();
  previewCombo = 0;
  previewScore = 0;
  previewComboBreak = 0;
  previewJudgeCount.clear();
}

void SettingsScene::syncPreviewInputLayout() {
  if (previewInputHandler == nullptr || previewChart == nullptr ||
      previewRenderer == nullptr) {
    return;
  }
  previewInputHandler->setPlayAreaWidth(
      context.settings.playAreaWidthForKeyMode(gameplay::presentationKeyMode(*previewChart)));
  previewInputHandler->setTouchLaneOrder(
      (previewPresentation ? previewPresentation.get() : previewRenderer)->touchLayout().lanes);
}

void SettingsScene::forwardPreviewInputEvent(SDL_Event &event) {
  if (previewInputHandler == nullptr) {
    return;
  }
  switch (event.type) {
  case SDL_KEYDOWN:
    previewInputHandler->onKeyDown(event.key.keysym.scancode, ScanCode);
    break;
  case SDL_KEYUP:
    previewInputHandler->onKeyUp(event.key.keysym.scancode, ScanCode);
    break;
  case SDL_FINGERDOWN:
  case SDL_FINGERUP:
  case SDL_FINGERMOTION: {
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::normalizedToUiNormalized(event.tfinger.x, event.tfinger.y,
                                        uiNormX, uiNormY);
    const Vector3 location(uiNormX, uiNormY, 0.0f);
    if (event.type == SDL_FINGERDOWN) {
      previewInputHandler->onFingerDown(event.tfinger.fingerId, location);
    } else if (event.type == SDL_FINGERUP) {
      previewInputHandler->onFingerUp(event.tfinger.fingerId, location);
    } else {
      previewInputHandler->onFingerMove(event.tfinger.fingerId, location);
    }
    break;
  }
  case SDL_MOUSEBUTTONDOWN:
  case SDL_MOUSEBUTTONUP: {
    if (event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID) {
      return;
    }
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::screenToUiNormalized(
        static_cast<float>(event.button.x) * rendering::widthScale,
        static_cast<float>(event.button.y) * rendering::heightScale, uiNormX,
        uiNormY);
    const Vector3 location(uiNormX, uiNormY, 0.0f);
    if (event.type == SDL_MOUSEBUTTONDOWN) {
      previewInputHandler->onFingerDown(0, location);
    } else {
      previewInputHandler->onFingerUp(0, location);
    }
    break;
  }
  case SDL_MOUSEMOTION: {
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::screenToUiNormalized(
        static_cast<float>(event.motion.x) * rendering::widthScale,
        static_cast<float>(event.motion.y) * rendering::heightScale, uiNormX,
        uiNormY);
    previewInputHandler->onFingerMove(0, Vector3(uiNormX, uiNormY, 0.0f));
    break;
  }
  default:
    break;
  }
}

void SettingsScene::resetPreviewHudSample() {
  previewJudgeCount.clear();
  for (int i = 0; i < JudgementCount; ++i) {
    previewJudgeCount[static_cast<Judgement>(i)] = 0;
  }
  previewJudgeCount[PGreat] = 8;
  previewJudgeCount[Great] = 3;
  previewJudgeCount[Good] = 1;
  previewCombo = kPreviewSampleCombo;
  previewScore = kPreviewSampleScore;
  previewComboBreak = 0;
  previewMaximumCombo = previewCombo;
  previewPassedNotes = 12;
  previewJudgeFastSlowCount = {{PGreat, {.fast = 4, .slow = 4}},
                              {Great, {.fast = 1, .slow = 2}},
                              {Good, {.fast = 0, .slow = 1}}};

  if (previewRenderer == nullptr) {
    return;
  }
  previewRenderer->setJudgementCounters(previewJudgeCount, previewComboBreak);
  previewGaugeRules = std::make_unique<GameplayGaugeRules>(
      compileGameplayGaugeRules(kDefaultGameplayRuleset,
                                previewChart->Meta,
                                GaugeProfile::Standard));
  previewRenderer->setGaugeStatus(GaugeType::Normal,
                                  GaugeAutoShiftMode::None, 74.0f,
                                  *previewGaugeRules);
  syncPreviewAuthority();
  if (previewPresentationEvents != nullptr) {
    const PlayfieldJudgeEventClock clock =
        makePlayfieldJudgeEventClock(0, 0, 0);
    previewPresentationEvents->onJudge(JudgeResult(Great, 50000),
                                       previewCombo, previewScore, clock,
                                       false);
  }
}

void SettingsScene::publishPreviewJudgement(
    const JudgeResult &judgeResult, long long sourceSongTimeMicros) {
  if (previewRenderer == nullptr) {
    return;
  }
  if (judgeResult.isComboBreak()) {
    previewCombo = 0;
    previewComboBreak++;
  } else if (judgeResult.judgement != Kpoor) {
    previewCombo++;
  }
  if (!judgeResult.isComboBreak() && judgeResult.judgement != Kpoor) {
    previewScore += 2;
  }
  previewJudgeCount[judgeResult.judgement]++;
  previewMaximumCombo = std::max(previewMaximumCombo, previewCombo);
  if (judgeResult.judgement != Kpoor && judgeResult.judgement != None)
    ++previewPassedNotes;
  auto &timingCount = previewJudgeFastSlowCount[judgeResult.judgement];
  if (judgeResult.Diff <= 0) ++timingCount.fast;
  else ++timingCount.slow;
  syncPreviewAuthority();
  if (previewPresentationEvents != nullptr) {
    const PlayfieldJudgeEventClock clock =
        makePlayfieldJudgeEventClock(sourceSongTimeMicros, 0, 0);
    previewPresentationEvents->onJudge(judgeResult, previewCombo,
                                       previewScore, clock, true);
  }
  previewRenderer->setJudgementCounter(
      judgeResult.judgement, previewJudgeCount[judgeResult.judgement],
      previewComboBreak);
}

bms_parser::Note *SettingsScene::pressLane(int lane, double inputDelay) {
  if (!previewActive || previewLaneController == nullptr) {
    return nullptr;
  }
  return pressLane(lane, lane, inputDelay);
}

bms_parser::Note *SettingsScene::pressLane(int mainLane, int compensateLane,
                                           double inputDelay) {
  if (!previewActive || previewLaneController == nullptr) {
    return nullptr;
  }
  const RhythmLaneInputController::InputContext inputContext{
      .songTimeMicros = previewElapsedMicros,
      .laneBeamTimeMicros = previewElapsedMicros,
      .inputDelay = inputDelay,
      .notePriorityMode = context.settings.notePriorityMode,
  };
  auto result =
      previewLaneController->pressLane(mainLane, compensateLane, inputContext);
  for (const auto &transaction : result.transactions) {
    if (transaction.hasJudge && previewRenderer != nullptr) {
      publishPreviewJudgement(
          transaction.judge,
          transaction.hasReplayEvent ? transaction.replayEvent.songTimeMicros
                                     : previewElapsedMicros);
    }
  }
  return result.note;
}

bms_parser::Note *SettingsScene::releaseLane(int lane, double inputDelay,
                                             bool isBackSpin) {
  if (!previewActive || previewLaneController == nullptr) {
    return nullptr;
  }
  const RhythmLaneInputController::InputContext inputContext{
      .songTimeMicros = previewElapsedMicros,
      .laneBeamTimeMicros = previewElapsedMicros,
      .inputDelay = inputDelay,
      .notePriorityMode = context.settings.notePriorityMode,
  };
  auto result =
      previewLaneController->releaseLane(lane, inputContext, isBackSpin);
  for (const auto &transaction : result.transactions) {
    if (transaction.hasJudge && previewRenderer != nullptr) {
      publishPreviewJudgement(
          transaction.judge,
          transaction.hasReplayEvent ? transaction.replayEvent.songTimeMicros
                                     : previewElapsedMicros);
    }
  }
  return result.note;
}

void SettingsScene::resetPreviewSimulation() {
  if (previewTouchRouter)
    (void)previewTouchRouter->cancelAll(static_cast<std::int64_t>(SDL_GetTicks64()) * 1000);
  previewElapsedMicros = 0;
  previewFrameSerial = 0;
  if (previewLaneController != nullptr) {
    previewLaneController->resetLaneStates();
  }
  if (previewPresentation != nullptr) {
    previewPresentation->reset();
  }
  if (previewVisualStateStore != nullptr &&
      previewChartVisualModel != nullptr) {
    previewVisualStateStore->resetModel(*previewChartVisualModel);
    previewVisualStateStore->setSceneStartMicros(0);
    previewVisualStateStore->setPlayStartMicros(0);
  }
  resetPreviewHudSample();
  if (previewChart == nullptr) {
    return;
  }
  for (const auto *measure : previewChart->Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (auto *note : timeline->Notes) {
        if (note != nullptr) {
          note->Reset();
        }
      }
      for (auto *note : timeline->InvisibleNotes) {
        if (note != nullptr) {
          note->Reset();
        }
      }
      for (auto *note : timeline->LandmineNotes) {
        if (note != nullptr) {
          note->Reset();
        }
      }
    }
  }
}
