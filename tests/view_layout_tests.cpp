#include "../src/view/BlockingOverlayView.h"
#include "../src/view/View.h"
#include "scene/SettingsSceneInputLayout.h"
#include "scene/SettingsSceneInputRebuild.h"
#include "scene/SettingsSceneProfileEditorState.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0f;
float heightScale = 1.0f;
float ui_scale_x = 1.0f;
float ui_scale_y = 1.0f;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

namespace {

class EventRecordingView final : public View {
public:
  int eventCount = 0;

private:
  bool handleEventsImpl(SDL_Event &) override {
    ++eventCount;
    return true;
  }
};

void testBlockingOverlayStopsAllInteractiveEvents() {
  View root(0, 0, 640, 480);
  auto *background = new EventRecordingView();
  auto *overlay = new BlockingOverlayView(0, 0, 640, 480);
  root.addView(background);
  root.addView(overlay);

  constexpr std::array eventTypes{
      SDL_MOUSEBUTTONDOWN, SDL_MOUSEWHEEL, SDL_FINGERDOWN, SDL_KEYDOWN,
      SDL_TEXTINPUT, SDL_TEXTEDITING};
  for (const Uint32 eventType : eventTypes) {
    SDL_Event event{};
    event.type = eventType;
    assert(!root.handleEvents(event));
  }
  assert(background->eventCount == 0);

  overlay->setVisible(false);
  SDL_Event event{};
  event.type = SDL_TEXTINPUT;
  assert(root.handleEvents(event));
  assert(background->eventCount == 1);
}

void expectNear(float actual, float expected, const char *label) {
  if (std::abs(actual - expected) <= 0.5f) {
    return;
  }
  std::cerr << label << ": expected " << expected << ", got " << actual
            << "\n";
  std::abort();
}

View *makeGridRow() {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setAlignItems(YGAlignStretch);
  row->setGap(8);
  row->setHeight(42);
  return row;
}

View *makeGridCell() {
  auto *cell = new View();
  cell->setFlexDirection(FlexDirection::Row);
  cell->setAlignItems(YGAlignStretch);
  cell->setHeight(42);
  cell->setWidth(160);
  cell->setFlexGrow(1);
  cell->setFlexBasis(0);
  cell->setFlexShrink(1);
  return cell;
}

View *makeGridContent(bool initialWidth = true) {
  auto *content = initialWidth ? new View(0, 0, 160, 42) : new View();
  content->setHeight(42);
  content->setWidthPercent(100);
  content->setFlexGrow(1);
  content->setFlexBasis(0);
  content->setFlexShrink(1);
  return content;
}

void applyMeasuredGridWidth(const std::vector<View *> &rows,
                            const std::vector<View *> &cells,
                            float panelWidth) {
  constexpr float kColumns = 3.0f;
  constexpr float kColumnGap = 8.0f;
  const float columnWidth =
      std::max(0.0f, (panelWidth - kColumnGap * (kColumns - 1.0f)) / kColumns);
  for (auto *row : rows) {
    row->setWidth(panelWidth);
    row->setFlexShrink(0);
  }
  for (auto *cell : cells) {
    cell->setWidth(columnWidth);
    cell->setFlexGrow(0);
    cell->setFlexBasis(columnWidth);
    cell->setFlexShrink(0);
    for (auto *child : cell->getChildren()) {
      child->setWidth(columnWidth);
      child->setFlexGrow(0);
      child->setFlexBasis(columnWidth);
      child->setFlexShrink(0);
    }
  }
}

void assertRowsAligned(View *referenceRow, View *row, int visibleCells = 3) {
  const auto &referenceCells = referenceRow->getChildren();
  const auto &cells = row->getChildren();
  assert(referenceCells.size() == 3);
  assert(static_cast<int>(cells.size()) >= visibleCells);
  for (int i = 0; i < visibleCells; ++i) {
    expectNear(cells[i]->getX(), referenceCells[i]->getX(), "cell x");
    expectNear(cells[i]->getWidth(), referenceCells[i]->getWidth(),
               "cell width");
    assert(!cells[i]->getChildren().empty());
    int widestChild = 0;
    for (auto *child : cells[i]->getChildren()) {
      widestChild = std::max(widestChild, child->getWidth());
    }
    expectNear(widestChild, referenceCells[i]->getWidth(), "cell child width");
  }
}

void testWrappedGridRowsKeepColumnMeasurements() {
  {
    View fixedCell(0, 0, 216, 42);
    fixedCell.setFlexDirection(FlexDirection::Row);
    fixedCell.setAlignItems(YGAlignStretch);
    auto *child = makeGridContent();
    fixedCell.addView(child);
    fixedCell.applyYogaLayout();
    expectNear(child->getWidth(), 216, "fixed cell child width");
  }
  {
    View flexRow(0, 0, 664, 42);
    flexRow.setFlexDirection(FlexDirection::Row);
    flexRow.setAlignItems(YGAlignStretch);
    flexRow.setGap(8);
    for (int i = 0; i < 3; ++i) {
      auto *cell = makeGridCell();
      cell->addView(makeGridContent());
      flexRow.addView(cell);
    }
    flexRow.applyYogaLayout();
    const auto &cells = flexRow.getChildren();
    expectNear(cells[0]->getWidth(), 216, "simple flex cell width");
    expectNear(cells[0]->getChildren().front()->getWidth(), 216,
               "simple flex cell child width");
  }
  {
    View column(0, 0, 664, 100);
    column.setFlexDirection(FlexDirection::Column);
    column.setAlignItems(YGAlignStretch);
    auto *row = makeGridRow();
    row->setWidth(664);
    column.addView(row);
    for (int i = 0; i < 3; ++i) {
      auto *cell = makeGridCell();
      cell->addView(makeGridContent());
      row->addView(cell);
    }
    column.applyYogaLayout();
    const auto &cells = row->getChildren();
    expectNear(cells[0]->getWidth(), 216, "nested flex cell width");
    expectNear(cells[0]->getChildren().front()->getWidth(), 216,
               "nested flex cell child width");
  }

  View root(0, 0, 1344, 860);
  root.setFlexDirection(FlexDirection::Row);
  root.setAlignItems(YGAlignStretch);
  root.setGap(24);
  root.setPadding(Edge::All, 20);

  auto *nav = new View();
  nav->setWidth(260);
  root.addView(nav);

  auto *left = new View();
  left->setFlexDirection(FlexDirection::Column);
  left->setAlignItems(YGAlignStretch);
  left->setFlex(1);
  left->setGap(14);
  left->setPadding(Edge::All, 16);
  root.addView(left);

  auto *right = new View();
  right->setWidth(300);
  root.addView(right);

  auto *sortPanel = new View();
  sortPanel->setFlexDirection(FlexDirection::Column);
  sortPanel->setAlignItems(YGAlignStretch);
  sortPanel->setGap(10);
  left->addView(sortPanel);

  auto *row1 = makeGridRow();
  auto *row2 = makeGridRow();
  auto *row3 = makeGridRow();
  std::vector<View *> gridRows{row1, row2, row3};
  std::vector<View *> gridCells;
  sortPanel->addView(row1);
  sortPanel->addView(row2);
  sortPanel->addView(row3);

  for (int i = 0; i < 3; ++i) {
    auto *cell = makeGridCell();
    cell->addView(makeGridContent());
    gridCells.push_back(cell);
    row1->addView(cell);
  }
  for (int i = 0; i < 3; ++i) {
    auto *cell = makeGridCell();
    cell->addView(makeGridContent());
    gridCells.push_back(cell);
    row2->addView(cell);
  }

  auto *difficultyCell = makeGridCell();
  auto *difficultyButton = makeGridContent();
  for (int i = 0; i < 2; ++i) {
    auto *cell = makeGridCell();
    cell->addView(makeGridContent());
    gridCells.push_back(cell);
    row3->addView(cell);
  }
  difficultyCell->addView(difficultyButton);
  gridCells.push_back(difficultyCell);
  row3->addView(difficultyCell);
  difficultyCell->setDisplay(YGDisplayNone);

  applyMeasuredGridWidth(gridRows, gridCells, 632.0f);
  root.applyYogaLayout();
  assertRowsAligned(row1, row2);
  assertRowsAligned(row1, row3, 2);

  difficultyCell->setDisplay(YGDisplayFlex);
  applyMeasuredGridWidth(gridRows, gridCells, 632.0f);
  root.applyYogaLayout();
  assertRowsAligned(row1, row3);
}

void testInputSettingsLayoutPolicy() {
  const auto wide = settings_scene::resolveInputSettingsLayout(1200, false);
  assert(!wide.stackSelectors);
  assert(!wide.stackBindingEditor);
  assert(wide.selectorWidth > 0 &&
         wide.selectorWidth * 3 + wide.selectorGap * 2 <= 1200);

  const auto compact = settings_scene::resolveInputSettingsLayout(520, true);
  assert(compact.stackSelectors);
  assert(compact.stackBindingEditor);
  assert(compact.selectorWidth == 520);
  assert(compact.numericControlWidth > 0 && compact.numericControlWidth <= 520);

  const auto narrow = settings_scene::resolveInputSettingsLayout(640, false);
  assert(narrow.stackSelectors);
  assert(narrow.stackBindingEditor);
  assert(narrow.selectorWidth == 640);

  const auto empty = settings_scene::resolveInputSettingsLayout(-50, true);
  assert(empty.selectorWidth == 0);
  assert(empty.numericControlWidth == 0);
}

void testInputSettingsRebuildWaitsForPointerTransaction() {
  settings_scene::InputSettingsRebuildGate gate;
  assert(gate.request());
  assert(!gate.request());
  gate.markEventComplete();
  assert(!gate.consume(true));
  assert(!gate.request());
  assert(gate.consume(false));
  assert(!gate.consume(false));

  assert(gate.request());
  gate.markEventComplete();
  gate.noticeStateChange();
  assert(gate.consume(false));

  gate.reset();
  gate.prepareForProfileReplacement();
  assert(!gate.consume(true));
  assert(gate.consume(false));
}

void testProfileInlineEditorStaysBoundToItsCard() {
  settings_scene::ProfileInlineEditorState editor;
  editor.beginRename("alpha", "Alpha");
  editor.updateDraft("Renamed Alpha");

  assert(!editor.requestFor("bravo").has_value());
  const auto rename = editor.requestFor("alpha");
  assert(rename.has_value());
  assert(rename->action == settings_scene::ProfileInlineEditAction::Rename);
  assert(rename->profileId == "alpha");
  assert(rename->name == "Renamed Alpha");

  editor.beginDuplicate("bravo", "Bravo");
  const auto duplicate = editor.requestFor("bravo");
  assert(duplicate.has_value());
  assert(duplicate->action ==
         settings_scene::ProfileInlineEditAction::Duplicate);
  assert(duplicate->name == "Bravo Copy");

  editor.clearIfTargetUnavailable(true);
  assert(editor.activeFor("bravo"));
  editor.clearIfTargetUnavailable(false);
  assert(!editor.active());
}

} // namespace

int main() {
  testBlockingOverlayStopsAllInteractiveEvents();
  testInputSettingsLayoutPolicy();
  testInputSettingsRebuildWaitsForPointerTransaction();
  testProfileInlineEditorStaysBoundToItsCard();
  bool deferredRan = false;
  View::deferAfterEvent([&]() { deferredRan = true; });
  assert(!deferredRan);
  View::dispatchDeferredEventCallbacks();
  assert(deferredRan);
  bool nestedDeferredRan = false;
  View::deferAfterEvent([&]() {
    View::deferAfterEvent([&]() { nestedDeferredRan = true; });
  });
  View::dispatchDeferredEventCallbacks();
  assert(!nestedDeferredRan);
  View::dispatchDeferredEventCallbacks();
  assert(nestedDeferredRan);

  View root(0, 0, 300, 100);
  root.setFlexDirection(FlexDirection::Row);
  root.setAlignItems(YGAlignFlexStart);

  auto *first = new View();
  first->setWidth(40.0f)->setHeight(20.0f);
  auto *second = new View();
  second->setWidth(40.0f)->setHeight(20.0f);

  root.addView(first);
  root.addView(second);
  assert(second->getX() == 40);

  first->setWidth(80.0f);
  assert(second->getX() == 80);

  second->setDisplay(YGDisplayNone);
  assert(first->getWidth() == 80);

  second->setDisplay(YGDisplayFlex);
  assert(second->getX() == 80);

  testWrappedGridRowsKeepColumnMeasurements();

  return 0;
}
