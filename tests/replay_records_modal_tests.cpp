#include "rendering/UniformCache.h"
#include "rendering/common.h"
#include "scene/ReplayRecordsModal.h"
#include "view/View.h"
#include "view/Button.h"
#include "view/TextView.h"
#include "ReplayVideoExporter.h"
#include "scene/MusicSelectRecords.h"
#include "scene/MusicSelectGhostBattle.h"

#include <SDL2/SDL.h>
#include <bgfx/bgfx.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

namespace {
int failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

ResultRecordSummary modernChartRecord() {
  ModernChartResultRecord modern{
      .result = {.resultId = 19, .attemptId = "selector-modal-attempt"}};
  return {
      .identity = ModernChartRecordId{.attemptId = modern.result.attemptId},
      .capabilities = {.watch = true, .videoExport = true},
      .modern = std::move(modern),
  };
}

void testSelectedModernRecordDispatchesWatchAndExport() {
  ChartMetaRecord chart;
  chart.meta.Title = "Selected chart";
  const auto summary = modernChartRecord();
  std::string watched;
  std::string exported;
  const ReplayRecordsModalCallbacks callbacks{
      .watchModernChart = [&](const ChartMetaRecord &record,
                              const ModernChartResultRecord &modern) {
        watched = record.meta.Title + ":" + modern.result.attemptId;
      },
      .exportModernChart = [&](const ChartMetaRecord &record,
                               const ModernChartResultRecord &modern,
                               ReplayVideoExportOptions) {
        exported = record.meta.Title + ":" + modern.result.attemptId;
      },
  };

  expect(ReplayRecordsModal::dispatchAction(ReplayRecordsModalAction::Watch,
                                             chart, summary, callbacks) &&
             watched == "Selected chart:selector-modal-attempt",
         "selected modern record requests replay watch through its owner");
  expect(ReplayRecordsModal::dispatchAction(
             ReplayRecordsModalAction::VideoExport, chart, summary,
             callbacks) &&
             exported == "Selected chart:selector-modal-attempt",
         "selected modern record requests video export through its owner");
}

void testNonModernRecordCannotCrossTheActionBoundary() {
  auto summary = modernChartRecord();
  summary.identity = LegacyChartRecordId{.legacyReplayId = 19};
  ChartMetaRecord chart;
  bool called = false;
  const ReplayRecordsModalCallbacks callbacks{
      .watchModernChart = [&](const ChartMetaRecord &,
                              const ModernChartResultRecord &) {
        called = true;
      },
  };
  expect(!ReplayRecordsModal::dispatchAction(ReplayRecordsModalAction::Watch,
                                              chart, summary, callbacks) &&
             !called,
         "forged legacy identity cannot dispatch a modern selector replay");
}

void testRetainedModalActivatesSelectedRecordThroughOwner() {
  View parent;
  ChartMetaRecord chart;
  chart.meta.Title = "Retained chart";
  const auto summary = modernChartRecord();
  std::string watched;
  std::string exported;
  const ReplayRecordsModalCallbacks callbacks{
      .loadRecords =
          [&](const ChartMetaRecord &) {
            return std::vector<ResultRecordSummary>{summary};
          },
      .watchModernChart = [&](const ChartMetaRecord &record,
                              const ModernChartResultRecord &modern) {
        watched = record.meta.Title + ":" + modern.result.attemptId;
      },
      .exportModernChart = [&](const ChartMetaRecord &record,
                               const ModernChartResultRecord &modern,
                               ReplayVideoExportOptions) {
        exported = record.meta.Title + ":" + modern.result.attemptId;
      },
  };

  auto modal = ReplayRecordsModal::Create(&parent, callbacks);
  expect(modal != nullptr, "shared records modal is created in the parent view");
  modal->showChart(chart);
  expect(modal->isVisible(), "showChart presents the retained records modal");
  modal->selectRecord(summary);
  expect(modal->activate(ReplayRecordsModalAction::Watch) &&
             watched == "Retained chart:selector-modal-attempt",
         "selected modern record requests replay watch through its owner");
  expect(modal->activate(ReplayRecordsModalAction::VideoExport) &&
             exported == "Retained chart:selector-modal-attempt",
         "selected modern record requests video export through its owner");
  modal->hide();
  expect(!modal->isVisible(), "hide dismisses the retained records modal");
}
Button *findVisibleButton(View *view, const std::string &label) {
  if (!view->getVisible()) return nullptr;
  if (auto *button = dynamic_cast<Button *>(view)) {
    auto *text = dynamic_cast<TextView *>(button->getContentView());
    if (text && text->getText() == label) return button;
  }
  for (auto *child : view->getChildren()) {
    if (auto *found = findVisibleButton(child, label)) return found;
  }
  return nullptr;
}

void clickButton(Button *button) {
  expect(button != nullptr, "requested option button is visible");
  if (!button) return;
  SDL_Event event{};
  event.type = SDL_MOUSEBUTTONDOWN;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.x = button->getX() + button->getWidth() / 2;
  event.button.y = button->getY() + button->getHeight() / 2;
  button->handleEvents(event);
  event.type = SDL_MOUSEBUTTONUP;
  button->handleEvents(event);
}

void clickModalButton(ReplayRecordsModal &modal, const char *key) {
  clickButton(findVisibleButton(modal.root(), i18n::tr(key)));
}

View *findVisibleOptionRow(View *view, const std::string &label) {
  if (!view->getVisible()) return nullptr;
  for (auto *child : view->getChildren()) {
    if (auto *text = dynamic_cast<TextView *>(child);
        text && text->getText() == label) return view;
    if (auto *row = findVisibleOptionRow(child, label)) return row;
  }
  return nullptr;
}

void hideReplayOption(ReplayRecordsModal &modal, const char *label) {
  auto *row = findVisibleOptionRow(modal.root(), i18n::tr(label));
  expect(row != nullptr, "visualization option row is visible");
  if (row) clickButton(findVisibleButton(row, i18n::tr("records.hide.label")));
}

void testReplayPreferencesRestoreAcrossModalInstances() {
  player_settings::ReplayPreferences saved;
  const auto record = modernChartRecord();
  ReplayVideoExportOptions exported;
  int saves = 0;
  auto callbacks = [&] {
    return ReplayRecordsModalCallbacks{
        .loadRecords = [&](const ChartMetaRecord &) {
          return std::vector<ResultRecordSummary>{record};
        },
        .exportModernChart = [&](const ChartMetaRecord &,
                                 const ModernChartResultRecord &,
                                 ReplayVideoExportOptions options) { exported = options; },
        .loadPreferences = [&] { return saved; },
        .savePreferences = [&](const player_settings::ReplayPreferences &preferences) {
          saved = preferences;
          ++saves;
        }};
  };
  {
    View parent(0, 0, rendering::design_width, rendering::design_height);
    auto modal = ReplayRecordsModal::Create(&parent, callbacks());
    modal->showChart({});
    modal->selectRecord(record);
    clickModalButton(*modal, "records.export_video.label");
    clickButton(findVisibleButton(modal->root(), "60 fps"));
    clickButton(findVisibleButton(modal->root(), "1080p"));
    hideReplayOption(*modal, "records.touch_points.label");
    hideReplayOption(*modal, "records.ghosts.label");
    clickModalButton(*modal, "settings.controls.keysound.auto_timed.label");
    expect(saves == 5 && saved.exportFps == 60 && !saved.exportFullResolution &&
               !saved.renderTouchPoints && !saved.renderGhosts && saved.autoKeySound,
           "each replay option change is saved immediately");
  }
  {
    View parent(0, 0, rendering::design_width, rendering::design_height);
    auto modal = ReplayRecordsModal::Create(&parent, callbacks());
    modal->showChart({});
    modal->selectRecord(record);
    expect(modal->autoKeySound() && !modal->renderTouchPoints() &&
               !modal->renderReplayGhosts(),
           "a new modal restores watch preferences from shared settings");
    clickModalButton(*modal, "records.export_video.label");
    clickModalButton(*modal, "records.export_video.label");
    expect(exported.fps == 60 && exported.height == 1080 &&
               !exported.renderTouchPoints && !exported.renderReplayGhosts &&
               exported.autoKeySound,
           "a new modal restores all five preferences for export");
    modal->returnToList();
    clickModalButton(*modal, "records.export_video.label");
    clickModalButton(*modal, "records.export_video.label");
    expect(exported.fps == 60 && exported.height == 1080,
           "reopening the export page preserves FPS and resolution");
    expect(saves == 5, "loading and navigating options does not rewrite settings");

    // Reloading a retained modal must use the active profile's preferences.
    saved = {};
    modal->hide();
    modal->showChart({});
    modal->selectRecord(record);
    const ResultRecordSummary autoPlay{
        .identity = AutoPlayRecordId{},
        .capabilities = {.watch = true, .videoExport = true},
        .autoPlay = true};
    modal->selectRecord(autoPlay);
    expect(modal->autoKeySound() && !modal->renderTouchPoints() &&
               !modal->renderReplayGhosts(), "autoplay applies its temporary overrides");
    modal->selectRecord(record);
    expect(!modal->autoKeySound() && modal->renderTouchPoints() &&
               modal->renderReplayGhosts(),
           "autoplay leaves the active profile's saved visualization choices intact");
    expect(saves == 5, "autoplay overrides are never persisted");
  }
}

void testKeysoundSelectionFlowsThroughWatchAndExport() {
  View parent(0, 0, rendering::design_width, rendering::design_height);
  auto record = modernChartRecord();
  bool exportedAuto = false;
  auto modal = ReplayRecordsModal::Create(&parent, {
      .loadRecords = [&](const ChartMetaRecord &) {
        return std::vector<ResultRecordSummary>{record};
      },
      .exportModernChart = [&](const ChartMetaRecord &,
                               const ModernChartResultRecord &,
                               ReplayVideoExportOptions options) {
        exportedAuto = options.autoKeySound;
      }});
  modal->showChart({});
  modal->selectRecord(record);
  expect(!modal->autoKeySound(), "saved replays default to input timing");
  clickModalButton(*modal, "records.watch.label");
  clickModalButton(*modal, "settings.controls.keysound.auto_timed.label");
  expect(modal->autoKeySound(), "watch uses the selected automatic timing");
  modal->returnToList();
  clickModalButton(*modal, "records.export_video.label");
  clickModalButton(*modal, "records.export_video.label");
  expect(exportedAuto, "export receives the same selected automatic timing");
  clickModalButton(*modal, "settings.controls.keysound.input_trigger.label");
  clickModalButton(*modal, "records.export_video.label");
  expect(!exportedAuto && !modal->autoKeySound(),
         "export selector switches both consumers back to input timing");
  clickModalButton(*modal, "settings.controls.keysound.auto_timed.label");
  modal->hide();
  ChartMetaRecord nextChart;
  nextChart.meta.Title = "Another chart";
  modal->showChart(nextChart);
  modal->selectRecord(record);
  expect(modal->autoKeySound(), "reopening records retains the keysound choice");
  clickModalButton(*modal, "records.export_video.label");
  clickModalButton(*modal, "records.export_video.label");
  expect(exportedAuto, "reopened export reuses the saved keysound choice");
}

void testCourseTargetsHaveIndependentIdentity() {
  MusicSelectBar course;
  course.kind = skin::MusicSelectBarKind::Grade;
  course.title = "Saved course";
  course.courseKey = "course-identity";
  ChartMetaRecord stage;
  stage.meta.SHA256 = "stage-hash";
  stage.meta.BmsPath = "stage.bms";
  course.courseCharts.push_back(stage);
  const auto target = musicSelectRecordsTarget(course);
  expect(target && target->courseStart && target->meta.Title == "Saved course" &&
             target->meta.SHA256.empty() && target->meta.BmsPath.empty(),
         "saved courses browse course records without inheriting stage identity");
  course.courseCharts.front().unavailable = true;
  expect(musicSelectRecordsTarget(course).has_value(),
         "missing course stages do not hide result history");
  course.courseKey.clear();
  expect(!musicSelectRecordsTarget(course), "anonymous courses have no saved record identity");
  course.courseId = 27;
  expect(musicSelectRecordsTarget(course).has_value(), "legacy course identity remains browsable");
  MusicSelectBar song;
  song.chart = stage;
  expect(musicSelectRecordsTarget(song).has_value(), "available songs open chart records");
  song.chart->solidArchive = true;
  expect(!musicSelectRecordsTarget(song), "solid archive targets cannot open chart records");
}

} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  if (!bgfx::init(init)) {
    std::cerr << "FAIL: headless bgfx did not initialize\n";
    return 1;
  }
  testReplayPreferencesRestoreAcrossModalInstances();
  testKeysoundSelectionFlowsThroughWatchAndExport();
  testCourseTargetsHaveIndependentIdentity();
  testSelectedModernRecordDispatchesWatchAndExport();
  testNonModernRecordCannotCrossTheActionBoundary();
  testRetainedModalActivatesSelectedRecordThroughOwner();
  {
    auto replay = std::make_shared<ReplayData>();
    replay->playOption = "RANDOM";
    replay->playOptionSeed = 123;
    replay->playOption2 = "MIRROR";
    replay->playOption2Seed = 456;
    replay->chartMeta.LnMode = 2;
    replay->assistOption = "LEGACY";
    replay->initialGaugeType = GaugeType::Normal;
    main_menu_profile::Selections selections;
    selections.gaugeType = GaugeType::Hard;
    result_persistence::ChartScoreWrite score;
    score.score = 1234;
    auto *returnScene = reinterpret_cast<Scene *>(std::uintptr_t{0x1234});
    const auto options = musicSelectGhostBattleOptions(
        replay, score, selections, true, {.percent = 80}, returnScene);
    expect(options.gbattleRecordData == replay && !options.replayData &&
               !options.autoPlay && options.targetScore &&
               options.targetScore->score == 1234,
           "G-BATTLE plays live against the saved score, not as replay autoplay");
    expect(options.gaugeType == GaugeType::Hard && options.autoKeySound &&
               options.playback.percent == 80 &&
               options.playOption == replay->playOption &&
               options.playOptionSeed == replay->playOptionSeed &&
               options.playOption2 == replay->playOption2 &&
               options.playOption2Seed == replay->playOption2Seed &&
               options.longNoteMode == 2 &&
               options.assistOption == replay->assistOption &&
               options.pacemakerTarget == pacemaker::kTargetOff &&
               options.replayGhostRenderingEnabled == false &&
               options.returnScene == returnScene,
           "selector G-BATTLE retains its owner and Main Menu play-option parity");
  }
  {
    ChartMetaRecord chart;
    chart.meta.Title = "Unplayed selector chart";
    chart.meta.TotalNotes = 100;
    main_menu_profile::Selections selections;
    selections.longNoteMode = "CN";
    const auto records = musicSelectChartRecords(chart, selections, {}, {});
    expect(records.size() == 1 &&
               std::holds_alternative<AutoPlayRecordId>(records.front().identity) &&
               records.front().capabilities.watch &&
               records.front().capabilities.videoExport,
           "selector loader exposes watchable and exportable autoplay without history");
    View root(0, 0, 640, 480);
    bool watched = false;
    bool exported = false;
    std::vector<ResultRecordSummary> loaded;
    auto modal = std::unique_ptr<ReplayRecordsModal>(ReplayRecordsModal::Create(
        &root, {.loadRecords = [&](const ChartMetaRecord &selected) {
                  loaded = musicSelectChartRecords(selected, selections, {}, {});
                  return loaded;
                },
                .watchAutoPlay = [&](const ChartMetaRecord &) { watched = true; },
                .exportAutoPlay = [&](const ChartMetaRecord &,
                                      ReplayVideoExportOptions) { exported = true; }}));
    modal->showChart(chart);
    if (!loaded.empty()) modal->selectRecord(loaded.front());
    expect(modal->autoKeySound(), "autoplay exposes automatic keysound timing");
    modal->selectRecord(modernChartRecord());
    expect(!modal->autoKeySound(), "autoplay does not overwrite the saved replay timing choice");
    if (!loaded.empty()) modal->selectRecord(loaded.front());
    expect(modal->activate(ReplayRecordsModalAction::Watch) && watched,
           "unplayed selector chart watches autoplay through the actual modal loader");
    expect(modal->activate(ReplayRecordsModalAction::VideoExport) && exported,
           "unplayed selector chart exports autoplay through the actual modal loader");
  }
  bgfx::shutdown();
  return failures == 0 ? 0 : 1;
}
