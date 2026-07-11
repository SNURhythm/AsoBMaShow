#include "SettingsSceneShared.h"
#include "../input/InputCaptureController.h"
#include "../input/RhythmInputHandler.h"
#include "../rendering/common.h"
#include "play/BMSRenderer.h"
#include "play/RhythmLaneInputController.h"

using namespace settings_scene;

namespace {
constexpr int kPreviewTimelineLanes = 16;
constexpr double kPreviewBpm = 120.0;
constexpr int kPreviewSampleCombo = 24;
constexpr int kPreviewSampleScore = 123456;
std::unique_ptr<bms_parser::TimeLine>
makePreviewTimeline(long long timingMicros, bool firstInMeasure = false) {
  auto timeline =
      std::make_unique<bms_parser::TimeLine>(kPreviewTimelineLanes, false);
  timeline->Timing = timingMicros;
  timeline->BeatPosition = static_cast<double>(timingMicros) / 2000000.0;
  timeline->Bpm = kPreviewBpm;
  timeline->Scroll = 1.0;
  timeline->IsFirstInMeasure = firstInMeasure;
  return timeline;
}

void addPreviewNote(bms_parser::TimeLine *timeline, int lane) {
  auto note = std::make_unique<bms_parser::Note>(bms_parser::Parser::NoWav);
  timeline->SetNote(lane, note.release());
}

void addPreviewLongNote(bms_parser::TimeLine *headTimeline,
                        bms_parser::TimeLine *tailTimeline, int lane) {
  auto head = std::make_unique<bms_parser::LongNote>(bms_parser::Parser::NoWav);
  auto tail = std::make_unique<bms_parser::LongNote>(bms_parser::Parser::NoWav);
  head->Tail = tail.get();
  tail->Head = head.get();
  headTimeline->SetNote(lane, head.release());
  tailTimeline->SetNote(lane, tail.release());
}

std::unique_ptr<bms_parser::Chart> makePreviewChart() {
  auto chart = std::make_unique<bms_parser::Chart>();
  chart->Meta.Title = "Settings Preview";
  chart->Meta.Bpm = kPreviewBpm;
  chart->Meta.MinBpm = kPreviewBpm;
  chart->Meta.MaxBpm = kPreviewBpm;
  chart->Meta.KeyMode = 7;
  chart->Meta.IsDP = false;
  chart->Meta.Rank = 3;
  chart->Meta.PlayLength = kPreviewLoopMicros;
  chart->Meta.TotalLength = kPreviewLoopMicros;

  auto measure = std::make_unique<bms_parser::Measure>();
  measure->Timing = 0;
  measure->Scale = 4.0;
  measure->Pos = 0.0;

  auto appendTimeline = [&measure](long long timingMicros,
                                   bool firstInMeasure = false) {
    auto timeline = makePreviewTimeline(timingMicros, firstInMeasure);
    auto *timelinePtr = timeline.get();
    measure->TimeLines.push_back(timeline.release());
    return timelinePtr;
  };

  addPreviewNote(appendTimeline(500000, true), 0);
  addPreviewNote(appendTimeline(850000), 2);
  addPreviewNote(appendTimeline(1200000), 4);
  addPreviewNote(appendTimeline(1550000), 6);
  addPreviewNote(appendTimeline(1900000), 7);

  auto *longHead = appendTimeline(2400000);
  auto *longTail = appendTimeline(3900000);
  addPreviewLongNote(longHead, longTail, 3);

  addPreviewNote(appendTimeline(4300000), 1);
  addPreviewNote(appendTimeline(4700000), 5);
  addPreviewNote(appendTimeline(5200000), 0);
  addPreviewNote(appendTimeline(5650000), 7);
  addPreviewNote(appendTimeline(6100000), 2);
  addPreviewNote(appendTimeline(6550000), 4);
  addPreviewNote(appendTimeline(7000000), 6);

  chart->Measures.push_back(measure.release());
  return chart;
}
} // namespace

SettingsScene::SettingsScene(ApplicationContext &context) : Scene(context) {}

SettingsScene::~SettingsScene() {
  context.profileSwitchBlockers.scene = nullptr;
  stopProfileArchiveWork();
  inputProfileReplacementRegistration.reset();
}

void SettingsScene::startLanePreview() {
  activeTab = SettingsTab::Lane;
  previewActive = true;
  previewPanelPage = 0;
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
  if (previewChart == nullptr) {
    previewChart = makePreviewChart();
  }
  if (previewRenderer == nullptr && previewChart != nullptr) {
    Judge previewJudge(previewChart->Meta.Rank);
    previewRenderer = std::make_unique<BMSRenderer>(
        previewChart.get(), previewJudge.timingWindows,
        context.settings.visibleTimeGreenNumber, true);
    previewRenderer->setVisibleTimeBpmStrategy(
        context.settings.visibleTimeBpmStrategy);
    previewRenderer->setVisibleTimeUseMilliseconds(
        context.settings.visibleTimeUseMilliseconds);
    previewRenderer->setPlayAreaWidth(
        context.settings.playAreaWidthForKeyMode(previewChart->Meta.KeyMode));
    previewRenderer->setLaneBeamLengthPercent(
        context.settings.laneBeamLengthPercent);
    previewRenderer->setNoteStartPositionPercent(
        context.settings.noteStartPositionPercent);
    previewRenderer->setLaneCoverFloatingEnabled(
        context.settings.floatingLaneCoverEnabled);
    previewRenderer->setLaneBeamClockUsesRenderTime(true);
    previewRenderer->setShowInvisibleNotes(context.settings.showInvisibleNotes);
    previewRenderer->setJudgementCounterEnabled(
        context.settings.judgementCounterEnabled);
    previewRenderer->setJudgementCounterPosition(
        context.settings.judgementCounterPosition);
    previewRenderer->setGaugeBarPosition(context.settings.gaugeBarPosition);
    previewRenderer->setJudgementTextY(context.settings.judgementTextY);
    previewRenderer->setJudgementTimingFastSlowCriteria(
        context.settings.judgementTimingFastSlowCriteria);
    previewRenderer->setJudgementTimingMillisecondsCriteria(
        context.settings.judgementTimingMillisecondsCriteria);
    resetPreviewHudSample();
  }
}

void SettingsScene::destroyPreviewRenderer() {
  destroyPreviewInputHandler();
  previewRenderer.reset();
  previewChart.reset();
  previewElapsedMicros = 0;
}

void SettingsScene::ensurePreviewInputHandler() {
  if (!previewActive) {
    return;
  }
  ensurePreviewRenderer();
  if (previewChart == nullptr || previewRenderer == nullptr) {
    return;
  }
  if (previewLaneController == nullptr) {
    previewLaneController = std::make_unique<RhythmLaneInputController>(
        previewChart.get(), previewRenderer.get(), previewLanePressed,
        Judge(previewChart->Meta.Rank));
  }
  if (previewInputHandler == nullptr) {
    previewInputHandler = std::make_unique<RhythmInputHandler>(
        this, previewChart->Meta, context.inputDeviceRegistry,
        context.inputProfile,
        makeGameplayInputScopes(previewChart->Meta.KeyMode),
        LogicalGameplayInputAdapter::CommandCallback{},
        context.settings.playAreaWidthForKeyMode(previewChart->Meta.KeyMode),
        LogicalGameplayRegistryPolicy{.acceptKeyboardFromRegistry = false});
    previewInputHandler->discardPendingTouchEvents();
    previewInputHandler->startListenSDL();
  }
}

void SettingsScene::destroyPreviewInputHandler() {
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

void SettingsScene::syncPreviewInputPlayAreaWidth() {
  if (previewInputHandler == nullptr || previewChart == nullptr) {
    return;
  }
  previewInputHandler->setPlayAreaWidth(
      context.settings.playAreaWidthForKeyMode(previewChart->Meta.KeyMode));
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

  if (previewRenderer == nullptr) {
    return;
  }
  previewRenderer->setJudgementCounters(previewJudgeCount, previewComboBreak);
  previewRenderer->setGaugeStatus(GaugeType::Normal, false, 74.0f);
  previewRenderer->onJudge(JudgeResult(Great, 50000), previewCombo,
                           previewScore, previewElapsedMicros, false);
}

void SettingsScene::publishPreviewJudgement(const JudgeResult &judgeResult) {
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
  previewRenderer->onJudge(judgeResult, previewCombo, previewScore,
                           previewElapsedMicros, true);
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
  if (result.hasJudge && previewRenderer != nullptr) {
    publishPreviewJudgement(result.judge);
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
  if (result.hasJudge && previewRenderer != nullptr) {
    publishPreviewJudgement(result.judge);
  }
  return result.note;
}

void SettingsScene::resetPreviewSimulation() {
  previewElapsedMicros = 0;
  if (previewLaneController != nullptr) {
    previewLaneController->resetLaneStates();
  }
  if (previewRenderer != nullptr) {
    previewRenderer->reset();
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
