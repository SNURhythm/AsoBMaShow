#include "../src/rendering/UniformCache.h"
#include "../src/view/Button.h"
#include "../src/view/CheckboxButtonContent.h"
#include "../src/view/ImageView.h"
#include "../src/view/IrUploadCandidateListView.h"
#include "../src/view/TextView.h"
#include "../src/view/UiTheme.h"

#include <SDL2/SDL.h>

#include <cstdlib>
#include <cstdio>
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

bool sameColor(SDL_Color left, SDL_Color right) {
  return left.r == right.r && left.g == right.g && left.b == right.b &&
         left.a == right.a;
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

std::string attemptId(int suffix) {
  char value[37]{};
  std::snprintf(value, sizeof(value), "123e4567-e89b-42d3-a456-426614174%03d",
                suffix);
  return value;
}

ir::IrUploadCandidate candidate(int resultId, std::string title,
                                std::string artist, std::string jacket,
                                ir::IrRecordState state) {
  ir::IrUploadCandidate value;
  value.modernChartResultId = resultId;
  value.result.resultId = resultId;
  value.result.attemptId = attemptId(resultId);
  value.result.score.chartTitle = std::move(title);
  value.result.score.chartArtist = std::move(artist);
  value.result.score.score = 1'500;
  value.result.score.maxScore = 2'000;
  value.result.score.maxCombo = 777;
  value.result.score.finalGauge = 82.5F;
  value.result.score.clearType = kClearTypeHardClearRank;
  value.result.score.provenance = ScoreProvenance::Legacy();
  value.result.score.provenance.player1.option = "RANDOM";
  value.result.keyMode = 7;
  value.result.adoptedGaugeType = GaugeType::Hard;
  (void)jacket;
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
    auto first = candidate(41, "First Song", "First Artist", "first.png",
                           ir::IrRecordState::Eligible);
    auto second = candidate(42, "Second Song", "Second Artist", "second.png",
                            ir::IrRecordState::Failed);
    second.failureReason = "provider rejected this score";
    second.result.resultId = first.result.resultId;
    second.modernChartResultId = first.modernChartResultId;
    second.result.attemptId = first.result.attemptId;
    second.result.adoptedGaugeType = GaugeType::Normal;
    second.result.score.finalGauge = 0.0F;
    second.result.score.provenance.player1.option = "NORMAL";
    second.result.score.score = 0;
    second.result.score.maxScore = 0;
    second.result.score.maxCombo = 0;
    second.result.score.clearType = -1;
    second.result.keyMode = 14;

    IrUploadCandidateListView list;
    list.setSize(960, 140);
    list.applyYogaLayout();
    std::unordered_set<std::string> selected{first.result.attemptId};
    int toggles = 0;
    std::string toggledAttemptId;
    int rowSelections = 0;
    list.onSelectionToggle = [&](std::string attemptId) {
      ++toggles;
      toggledAttemptId = std::move(attemptId);
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
    expect(image(row, "irUploadJacket")->imagePath().empty(),
           "snapshot-only row does not hydrate chart artwork");
    expect(text(row, "irUploadArtist")->getText() == "First Artist" &&
               text(row, "irUploadAttempt")->getText().find("Attempt") !=
                   std::string::npos &&
               text(row, "irUploadAttempt")->getText().find("Combo 777") !=
                   std::string::npos &&
               text(row, "irUploadAttempt")->getText().find("HARD") !=
                   std::string::npos &&
               text(row, "irUploadAttempt")->getText().find("RANDOM") !=
                   std::string::npos &&
               text(row, "irUploadDifficulty")->getText().empty() &&
               text(row, "irUploadKeyMode")->getText() == "7K" &&
               text(row, "irUploadScore")->getText() == "1500" &&
               text(row, "irUploadRank")->getText() == "A",
           "row shows durable modern result and provenance facts");
    expect(selection->isSelected(), "row reflects external checkbox selection");
    auto *selectionContent = dynamic_cast<CheckboxButtonContent *>(
        selection->getContentView());
    expect(selectionContent != nullptr && selectionContent->checked(),
           "IR upload selection uses FontAwesome checkbox content");
    expect(selectionContent->iconView()->pointSize() == 30,
           "IR upload checkbox uses the large icon size");
    expect(selectionContent->iconView()->getWidth() >= 30,
           "IR upload checkbox gives the large glyph enough layout width");
    expect(!selection->hasStyledBackgroundStyle(),
           "IR upload checkbox has no outer selection box");
    expect(sameColor(selectionContent->iconView()->currentColor(),
                     ui_theme::sdl(ui_theme::cyan())),
           "selected IR upload checkbox tints the glyph cyan");
    click(list, *selection);
    expect(toggles == 1 && toggledAttemptId == first.result.attemptId &&
               rowSelections == 0,
           "checkbox toggles its attempt ID without selecting the row");

    list.setCandidates({second}, {});
    row = list.getViewByIndex(0);
    selection = button(row, "irUploadSelection");
    selectionContent = dynamic_cast<CheckboxButtonContent *>(
        selection->getContentView());
    expect(row == firstRow &&
               image(row, "irUploadJacket")->imagePath().empty(),
           "rebind remains independent of chart artwork");
    expect(!selection->hasStyledBackgroundStyle(),
           "unchecked IR upload checkbox remains unboxed");
    expect(sameColor(selectionContent->iconView()->currentColor(),
                     ui_theme::sdl(ui_theme::textMuted())),
           "unchecked IR upload checkbox uses the muted tint");
    expect(text(row, "irUploadStatus")->getText() == "Retry",
           "rebind replaces status");
    expect(text(row, "irUploadAttempt")
                   ->getText()
                   .find("Failed: provider rejected this score") !=
               std::string::npos,
           "failed attempt appends its upload reason to the detail line");
    expect(
        !selection->isSelected() &&
            !dynamic_cast<CheckboxButtonContent *>(selection->getContentView())
                 ->checked() &&
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

    second.result.score.maxScore = 2'000;
    list.setCandidates({second}, {});
    row = list.getViewByIndex(0);
    expect(text(row, "irUploadScore")->getText() == "0" &&
               text(row, "irUploadRank")->getText() == "F",
           "a legitimate zero score keeps its score and rank metadata");

    list.setCandidates({second}, {});
    row = list.getViewByIndex(0);
    expect(image(row, "irUploadJacket")->imagePath().empty(),
           "no jacket clears a recycled image identity");

    auto third = first;
    third.result.resultId = 99;
    third.modernChartResultId = 99;
    third.result.attemptId = attemptId(99);
    third.failureReason.clear();
    list.setCandidates({third}, {});
    row = list.getViewByIndex(0);
    expect(text(row, "irUploadAttempt")->getText().find("Failed:") ==
               std::string::npos,
           "recycled eligible row clears another attempt's failure suffix");

    auto fourth = second;
    fourth.result.resultId = 100;
    fourth.modernChartResultId = 100;
    fourth.result.attemptId = attemptId(100);
    list.setCandidates({fourth}, {});
    row = list.getViewByIndex(0);
    selection = button(row, "irUploadSelection");
    expect(row == firstRow &&
               image(row, "irUploadJacket")->imagePath().empty(),
           "recycled row stays free of chart-library identity");
    const float preservedScrollOffset = list.scrollOffset;
    list.setSelectedAttemptIds({fourth.result.attemptId});
    row = list.getViewByIndex(0);
    selection = button(row, "irUploadSelection");
    expect(selection->isSelected() &&
               list.scrollOffset == preservedScrollOffset,
           "selection-only rebind preserves the current viewport");
    click(list, *selection);
    expect(toggles == 2 && toggledAttemptId == fourth.result.attemptId &&
               rowSelections == 0,
           "recycled checkbox callback captures its current attempt ID");

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
