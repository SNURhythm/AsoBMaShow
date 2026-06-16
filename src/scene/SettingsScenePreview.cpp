#include "SettingsSceneShared.h"
#include "../input/RhythmInputHandler.h"
#include "../rendering/common.h"
#include "play/BMSRenderer.h"
#include "play/RhythmLaneInputController.h"

using namespace settings_scene;

namespace {
constexpr int kPreviewTimelineLanes = 16;
constexpr double kPreviewBpm = 120.0;
bms_parser::TimeLine *makePreviewTimeline(long long timingMicros,
                                          bool firstInMeasure = false) {
  auto *timeline = new bms_parser::TimeLine(kPreviewTimelineLanes, false);
  timeline->Timing = timingMicros;
  timeline->BeatPosition = static_cast<double>(timingMicros) / 2000000.0;
  timeline->Bpm = kPreviewBpm;
  timeline->Scroll = 1.0;
  timeline->IsFirstInMeasure = firstInMeasure;
  return timeline;
}

void addPreviewNote(bms_parser::TimeLine *timeline, int lane) {
  timeline->SetNote(lane, new bms_parser::Note(bms_parser::Parser::NoWav));
}

void addPreviewLongNote(bms_parser::TimeLine *headTimeline,
                        bms_parser::TimeLine *tailTimeline, int lane) {
  auto *head = new bms_parser::LongNote(bms_parser::Parser::NoWav);
  auto *tail = new bms_parser::LongNote(bms_parser::Parser::NoWav);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(lane, head);
  tailTimeline->SetNote(lane, tail);
}

bms_parser::Chart *makePreviewChart() {
  auto *chart = new bms_parser::Chart();
  chart->Meta.Title = "Settings Preview";
  chart->Meta.Bpm = kPreviewBpm;
  chart->Meta.MinBpm = kPreviewBpm;
  chart->Meta.MaxBpm = kPreviewBpm;
  chart->Meta.KeyMode = 7;
  chart->Meta.IsDP = false;
  chart->Meta.Rank = 3;
  chart->Meta.PlayLength = kPreviewLoopMicros;
  chart->Meta.TotalLength = kPreviewLoopMicros;

  auto *measure = new bms_parser::Measure();
  measure->Timing = 0;
  measure->Scale = 4.0;
  measure->Pos = 0.0;

  auto appendTimeline = [measure](long long timingMicros,
                                  bool firstInMeasure = false) {
    auto *timeline = makePreviewTimeline(timingMicros, firstInMeasure);
    measure->TimeLines.push_back(timeline);
    return timeline;
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

  chart->Measures.push_back(measure);
  return chart;
}
} // namespace

void SettingsScene::startLanePreview() {
  activeTab = SettingsTab::Lane;
  previewActive = true;
  previewPanelPage = 0;
  resetPreviewSimulation();
  ensurePreviewRenderer();
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
    previewRenderer =
        new BMSRenderer(previewChart, previewJudge.timingWindows,
                        context.settings.visibleTimeGreenNumber, false);
    previewRenderer->setVisibleTimeBpmStrategy(
        context.settings.visibleTimeBpmStrategy);
    previewRenderer->setPlayAreaWidth(
        context.settings.playAreaWidthForKeyMode(previewChart->Meta.KeyMode));
    previewRenderer->setLaneBeamLengthPercent(
        context.settings.laneBeamLengthPercent);
    previewRenderer->setNoteStartPositionPercent(
        context.settings.noteStartPositionPercent);
    previewRenderer->setLaneBeamClockUsesRenderTime(true);
    previewRenderer->setShowInvisibleNotes(context.settings.showInvisibleNotes);
  }
}

void SettingsScene::destroyPreviewRenderer() {
  destroyPreviewInputHandler();
  delete previewRenderer;
  previewRenderer = nullptr;
  delete previewChart;
  previewChart = nullptr;
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
    previewLaneController = new RhythmLaneInputController(
        previewChart, previewRenderer, previewLanePressed);
  }
  if (previewInputHandler == nullptr) {
    previewInputHandler = new RhythmInputHandler(
        this, previewChart->Meta,
        context.settings.playAreaWidthForKeyMode(previewChart->Meta.KeyMode));
    previewInputHandler->discardPendingTouchEvents();
  }
}

void SettingsScene::destroyPreviewInputHandler() {
  if (previewInputHandler != nullptr) {
    previewInputHandler->stopListen();
    delete previewInputHandler;
    previewInputHandler = nullptr;
  }
  delete previewLaneController;
  previewLaneController = nullptr;
  previewLanePressed.clear();
  previewCombo = 0;
  previewScore = 0;
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
    if (result.judge.isComboBreak()) {
      previewCombo = 0;
    } else if (result.judge.judgement != Kpoor) {
      previewCombo++;
    }
    if (!result.judge.isComboBreak() && result.judge.judgement != Kpoor) {
      previewScore += 2;
    }
    previewRenderer->onJudge(result.judge, previewCombo, previewScore,
                             previewElapsedMicros, true);
  }
  return result.note;
}

bms_parser::Note *SettingsScene::releaseLane(int lane, double inputDelay) {
  if (!previewActive || previewLaneController == nullptr) {
    return nullptr;
  }
  const RhythmLaneInputController::InputContext inputContext{
      .songTimeMicros = previewElapsedMicros,
      .laneBeamTimeMicros = previewElapsedMicros,
      .inputDelay = inputDelay,
      .notePriorityMode = context.settings.notePriorityMode,
  };
  auto result = previewLaneController->releaseLane(lane, inputContext);
  if (result.hasJudge && previewRenderer != nullptr) {
    if (result.judge.isComboBreak()) {
      previewCombo = 0;
    } else if (result.judge.judgement != Kpoor) {
      previewCombo++;
    }
    if (!result.judge.isComboBreak() && result.judge.judgement != Kpoor) {
      previewScore += 2;
    }
    previewRenderer->onJudge(result.judge, previewCombo, previewScore,
                             previewElapsedMicros, true);
  }
  return result.note;
}

void SettingsScene::resetPreviewSimulation() {
  previewElapsedMicros = 0;
  previewCombo = 0;
  previewScore = 0;
  if (previewLaneController != nullptr) {
    previewLaneController->resetLaneStates();
  }
  if (previewRenderer != nullptr) {
    previewRenderer->reset();
  }
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
