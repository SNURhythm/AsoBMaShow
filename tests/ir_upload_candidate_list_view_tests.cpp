#include "../src/rendering/UniformCache.h"
#include "../src/view/Button.h"
#include "../src/view/ImageView.h"
#include "../src/view/IrUploadCandidateListView.h"
#include "../src/view/TextView.h"

#include <SDL2/SDL.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>

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

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

TextView *text(View *row, const char *name) {
  return row == nullptr ? nullptr
                        : dynamic_cast<TextView *>(row->findViewByName(name));
}

ImageView *image(View *row, const char *name) {
  return row == nullptr ? nullptr
                        : dynamic_cast<ImageView *>(row->findViewByName(name));
}

Button *button(View *row, const char *name) {
  return row == nullptr ? nullptr
                        : dynamic_cast<Button *>(row->findViewByName(name));
}

void click(IrUploadCandidateListView &list, const Button &target) {
  SDL_Event down{};
  down.type = SDL_MOUSEBUTTONDOWN;
  down.button.type = SDL_MOUSEBUTTONDOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.which = 1;
  down.button.x = target.getX() + target.getWidth() / 2;
  down.button.y = target.getY() + target.getHeight() / 2;
  SDL_Event up = down;
  up.type = SDL_MOUSEBUTTONUP;
  list.handleEvents(down);
  list.handleEvents(up);
}

ir::IrUploadCandidate candidate(int replayId, std::string title,
                                std::string artist, std::string jacket,
                                ir::IrRecordState state) {
  ir::IrUploadCandidate value;
  value.replay.id = replayId;
  value.replay.createdAt = "2026-07-20 19:30:00";
  value.replay.initialGaugeType = GaugeType::Hard;
  value.replay.finalGauge = 82.5F;
  value.replay.playOption = "RANDOM";
  value.replay.finalScore = 1'500;
  value.replay.maxScore = 2'000;
  value.replay.maxCombo = 777;
  value.replay.clearType = kClearTypeHardClearRank;
  value.chart.meta.Title = std::move(title);
  value.chart.meta.Artist = std::move(artist);
  value.chart.meta.Folder = "/charts/first";
  value.chart.meta.StageFile = std::move(jacket);
  value.chart.meta.PlayLevel = 12.0;
  value.chart.meta.KeyMode = 7;
  value.state = state;
  return value;
}

} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  expect(bgfx::init(init), "headless bgfx initializes for IR upload list");

  {
    const path_t firstJacket = "/charts/first/first.png";
    const path_t secondJacket = "/charts/second/second.png";
    const path_t fourthJacket = "/charts/fourth/fourth.png";
    auto first = candidate(41, "First Song", "First Artist", "first.png",
                           ir::IrRecordState::Eligible);
    auto second = candidate(42, "Second Song", "Second Artist", "second.png",
                            ir::IrRecordState::Failed);
    second.failureReason = "provider rejected this score";
    second.replay.id = first.replayId();
    second.chart.meta.Folder = "/charts/second";
    second.replay.createdAt = "2026-07-20 20:00:00";
    second.replay.initialGaugeType = GaugeType::Normal;
    second.replay.finalGauge = 0.0F;
    second.replay.playOption.reset();
    second.replay.finalScore = 0;
    second.replay.maxScore = 0;
    second.replay.maxCombo = 0;
    second.replay.clearType = -1;
    second.chart.meta.PlayLevel = 0.0;
    second.chart.meta.KeyMode = 14;

    IrUploadCandidateListView list;
    list.setSize(960, 140);
    list.applyYogaLayout();
    std::unordered_set<int> selected{first.replayId()};
    int toggles = 0;
    int toggledReplayId = 0;
    int rowSelections = 0;
    list.onSelectionToggle = [&](int replayId) {
      ++toggles;
      toggledReplayId = replayId;
    };
    list.onSelected = [&](const ir::IrUploadCandidate &, int) {
      ++rowSelections;
    };
    list.setCandidates({first}, selected);

    View *row = list.getViewByIndex(0);
    View *const firstRow = row;
    auto *selection = button(row, "irUploadSelection");
    expect(row != nullptr && selection != nullptr,
           "eligible candidate binds a selectable row");
    expect(text(row, "irUploadTitle")->getText() == "First Song",
           "row shows chart title");
    expect(image(row, "irUploadJacket")->imagePath() == firstJacket,
           "row shows the first jacket");
    expect(text(row, "irUploadArtist")->getText() == "First Artist" &&
               text(row, "irUploadAttempt")->getText().find("2026-07-20") !=
                   std::string::npos &&
               text(row, "irUploadAttempt")->getText().find("Combo 777") !=
                   std::string::npos &&
               text(row, "irUploadAttempt")->getText().find("HARD") !=
                   std::string::npos &&
               text(row, "irUploadAttempt")->getText().find("RANDOM") !=
                   std::string::npos &&
               text(row, "irUploadDifficulty")->getText() == "12" &&
               text(row, "irUploadKeyMode")->getText() == "7K" &&
               text(row, "irUploadScore")->getText() == "1500" &&
               text(row, "irUploadRank")->getText() == "A",
           "row shows chart, combo, date, option, and replay metadata");
    expect(selection->isSelected(), "row reflects external checkbox selection");
    click(list, *selection);
    expect(toggles == 1 && toggledReplayId == first.replayId() &&
               rowSelections == 0,
           "checkbox toggles its replay ID without selecting the recycler row");

    list.setCandidates({second}, {});
    row = list.getViewByIndex(0);
    selection = button(row, "irUploadSelection");
    expect(row == firstRow &&
               image(row, "irUploadJacket")->imagePath() == secondJacket,
           "rebind replaces jacket identity");
    expect(text(row, "irUploadStatus")->getText() == "Retry",
           "rebind replaces status");
    expect(text(row, "irUploadFailure") != nullptr &&
               text(row, "irUploadFailure")->getText() ==
                   "Failed: provider rejected this score",
           "failed attempt row shows its upload reason");
    expect(
        !selection->isSelected() &&
            text(row, "irUploadAttempt")->getText().find("NORMAL") !=
                std::string::npos &&
            text(row, "irUploadAttempt")->getText().find("RANDOM") ==
                std::string::npos &&
            text(row, "irUploadDifficulty")->getText().empty() &&
            text(row, "irUploadKeyMode")->getText() == "7KDP" &&
            text(row, "irUploadScore")->getText().empty() &&
            text(row, "irUploadRank")->getText().empty() &&
            !dynamic_cast<IrUploadCandidateListItemView *>(row)->hasClearLamp(),
        "rebind clears checkbox, optional metadata, score rank, and lamp");

    second.replay.maxScore = 2'000;
    list.setCandidates({second}, {});
    row = list.getViewByIndex(0);
    expect(text(row, "irUploadScore")->getText() == "0" &&
               text(row, "irUploadRank")->getText() == "F",
           "a legitimate zero score keeps its score and rank metadata");

    second.chart.meta.StageFile.clear();
    list.setCandidates({second}, {});
    row = list.getViewByIndex(0);
    expect(image(row, "irUploadJacket")->imagePath().empty(),
           "no jacket clears a recycled image identity");

    auto third = first;
    third.replay.id = 99;
    third.failureReason.clear();
    third.chart.meta.Folder = "/charts/third";
    third.chart.meta.StageFile = "third.png";
    list.setCandidates({third}, {});
    row = list.getViewByIndex(0);
    expect(text(row, "irUploadFailure")->getText().empty(),
           "recycled eligible row clears another attempt's failure reason");

    auto fourth = second;
    fourth.replay.id = 100;
    fourth.chart.meta.Folder = "/charts/fourth";
    fourth.chart.meta.StageFile = "fourth.png";
    list.setCandidates({fourth}, {});
    row = list.getViewByIndex(0);
    selection = button(row, "irUploadSelection");
    expect(row == firstRow &&
               image(row, "irUploadJacket")->imagePath() == fourthJacket,
           "recycled row receives its next jacket identity");
    const float preservedScrollOffset = list.scrollOffset;
    list.setSelectedReplayIds({fourth.replayId()});
    row = list.getViewByIndex(0);
    selection = button(row, "irUploadSelection");
    expect(selection->isSelected() &&
               list.scrollOffset == preservedScrollOffset,
           "selection-only rebind preserves the current viewport");
    click(list, *selection);
    expect(toggles == 2 && toggledReplayId == fourth.replayId() &&
               rowSelections == 0,
           "recycled checkbox callback captures its current replay ID");

    list.setSelectionLocked(true);
    selection = button(row, "irUploadSelection");
    click(list, *selection);
    expect(
        toggles == 2 && rowSelections == 0,
        "locked checkbox consumes input without toggling or row fallthrough");
  }
  bgfx::shutdown();
  return 0;
}
