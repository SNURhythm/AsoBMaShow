#include "SettingsSceneShared.h"
#include "play/BMSRenderer.h"

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
  resetPreviewSimulation();
  ensurePreviewRenderer();
  lastLayoutWidth = -1;
}

void SettingsScene::stopLanePreview() {
  previewActive = false;
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
    previewRenderer->setShowInvisibleNotes(context.settings.showInvisibleNotes);
  }
}

void SettingsScene::destroyPreviewRenderer() {
  delete previewRenderer;
  previewRenderer = nullptr;
  delete previewChart;
  previewChart = nullptr;
  previewElapsedMicros = 0;
}

void SettingsScene::resetPreviewSimulation() {
  previewElapsedMicros = 0;
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
