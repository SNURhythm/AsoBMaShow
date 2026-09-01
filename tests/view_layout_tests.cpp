#include "../src/view/BlockingOverlayView.h"
#include "../src/view/OverlayPortal.h"
#include "../src/view/View.h"
#include "scene/ResultLayoutGeometry.h"
#include "ir/IrRankingModal.h"
#include "scene/SettingsSceneInputActions.h"
#include "scene/SettingsSceneInputLayout.h"
#include "scene/SettingsSceneInputRebuild.h"
#include "scene/SettingsSceneProfileEditorState.h"
#include "skin/SkinTypes.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <optional>
#include <string_view>

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

static_assert(requires(ResultSkinData data) {
  data.showTimingAnalytics;
  data.showResultGraph;
});

class EventRecordingView final : public View {
public:
  int eventCount = 0;

private:
  bool handleEventsImpl(SDL_Event &) override {
    ++eventCount;
    return true;
  }
};

class EventConsumingView final : public View {
public:
  int eventCount = 0;

private:
  bool handleEventsImpl(SDL_Event &) override {
    ++eventCount;
    return false;
  }
};

class OrderedView final : public View {
public:
  OrderedView(int id, std::vector<int> &renderOrder,
              std::vector<int> &eventOrder)
      : id(id), renderOrder(renderOrder), eventOrder(eventOrder) {}

private:
  void renderImpl(RenderContext &) override { renderOrder.push_back(id); }
  bool handleEventsImpl(SDL_Event &) override {
    eventOrder.push_back(id);
    return true;
  }

  int id;
  std::vector<int> &renderOrder;
  std::vector<int> &eventOrder;
};

class TransformRecordingView final : public View {
public:
  using View::View;
  RenderContext::Point mappedPoint;

private:
  void renderImpl(RenderContext &context) override {
    mappedPoint = context.transformPoint(
        static_cast<float>(getX() + getWidth()),
        static_cast<float>(getY()) + static_cast<float>(getHeight()) * 0.5f);
  }
};

class RenderProbeView final : public View {
public:
  using View::View;
  int renderCalls = 0;

private:
  void renderImpl(RenderContext &) override { ++renderCalls; }
};

class VisibleOverflowProbeView final : public View {
public:
  VisibleOverflowProbeView(int layoutX, int layoutY, int layoutWidth,
                           int layoutHeight, int paintX, int paintY,
                           int paintWidth, int paintHeight)
      : View(layoutX, layoutY, layoutWidth, layoutHeight),
        paintBounds{static_cast<float>(paintX), static_cast<float>(paintY),
                    static_cast<float>(paintWidth),
                    static_cast<float>(paintHeight)} {}

  int renderCalls = 0;

private:
  RenderBounds renderingBounds() const override { return paintBounds; }
  void renderImpl(RenderContext &) override { ++renderCalls; }

  RenderBounds paintBounds;
};

void testViewSkipsOffscreenPaintingButStillVisitsVisibleChildren() {
  RenderContext context;
  context.pushScissor(0, 0, 100, 100);

  RenderProbeView visible(10, 10, 40, 40);
  visible.render(context);
  assert(visible.renderCalls == 1);

  RenderProbeView offscreenParent(0, 150, 40, 40);
  auto *visibleAbsoluteChild = new RenderProbeView(10, 0, 30, 30);
  offscreenParent.addView(visibleAbsoluteChild);
  visibleAbsoluteChild->setPositionNoLayout(10, -150);
  assert(visibleAbsoluteChild->getY() == 0);

  offscreenParent.render(context);
  assert(offscreenParent.renderCalls == 0);
  assert(visibleAbsoluteChild->renderCalls == 1);

  context.popScissor();
}

void testViewSkipsZeroExtentPaintingWithoutVisibleOverflowBounds() {
  RenderContext context;
  context.pushScissor(0, 0, 100, 100);

  // A collapsed ordinary view has no paint extent. Views that deliberately
  // overflow must provide their real bounds through renderingBounds().
  RenderProbeView zeroExtent(10, 10, 0, 0);
  zeroExtent.render(context);
  assert(zeroExtent.renderCalls == 0);

  context.popScissor();
}

void testViewUsesVisibleOverflowBoundsForOwnPainting() {
  RenderContext context;
  context.pushScissor(0, 0, 100, 100);

  // TextView's visible overflow may lie inside the viewport even when Yoga
  // positioned its layout slot outside it. Culling must use the actual paint
  // bounds rather than the layout slot.
  VisibleOverflowProbeView overflowing(0, 160, 20, 20, 10, 10, 40, 20);
  overflowing.render(context);
  assert(overflowing.renderCalls == 1);

  context.popScissor();
}

void testViewRotationTransformsRenderingAndScissor() {
  TransformRecordingView view(20, 30, 40, 20);
  view.setRotationDegrees(90.0f);
  RenderContext context;
  view.render(context);
  assert(std::abs(view.mappedPoint.x - 40.0f) < 0.001f);
  assert(std::abs(view.mappedPoint.y - 60.0f) < 0.001f);

  const auto unchanged = context.transformPoint(13.0f, 27.0f);
  assert(std::abs(unchanged.x - 13.0f) < 0.001f);
  assert(std::abs(unchanged.y - 27.0f) < 0.001f);

  context.pushRotation(90.0f, 50.0f, 50.0f);
  {
    ScissorScope scissor(context, 40, 45, 20, 10);
    assert(context.scissor.x == 45);
    assert(context.scissor.y == 40);
    assert(context.scissor.width == 10);
    assert(context.scissor.height == 20);
  }
  context.popTransform();
  assert(context.scissor.width == -1);
  assert(context.scissor.height == -1);
}

void testOverlayPortalDispatchesPresentedViewsAboveContent() {
  View root(0, 0, 640, 480);
  auto *background = new EventRecordingView();
  auto *portal = new OverlayPortal(0, 0, 640, 480);
  portal->setZIndex(900);
  EventConsumingView overlay;
  root.addView(background);
  root.addView(portal);
  portal->present(&overlay);
  portal->present(&overlay);
  assert(portal->isPresented(&overlay));

  SDL_Event event{};
  event.type = SDL_MOUSEBUTTONDOWN;
  assert(!root.handleEvents(event));
  assert(overlay.eventCount == 1);
  assert(background->eventCount == 0);

  portal->dismiss(&overlay);
  assert(!portal->isPresented(&overlay));
  assert(root.handleEvents(event));
  assert(overlay.eventCount == 1);
  assert(background->eventCount == 1);
}

void testRankingModalPanelStaysCenteredInsideSafeArea() {
  const auto geometry = ir::layoutIrRankingPanel(
      {.viewportWidth = 1000,
       .viewportHeight = 700,
       .safeTop = 20,
       .safeLeft = 40,
       .safeBottom = 30,
       .safeRight = 10,
       .margin = 24,
       .maximumWidth = 1180,
       .maximumHeight = 840});
  assert(geometry.x == 64);
  assert(geometry.y == 44);
  assert(geometry.width == 902);
  assert(geometry.height == 602);
  assert(geometry.compact);

  const auto wide = ir::layoutIrRankingPanel(
      {.viewportWidth = 1280,
       .viewportHeight = 800,
       .safeTop = 0,
       .safeLeft = 0,
       .safeBottom = 0,
       .safeRight = 0,
       .margin = 24,
       .maximumWidth = 1180,
       .maximumHeight = 840});
  assert(wide.width == 1180);
  assert(wide.compact == false);

  const auto compact = ir::layoutIrRankingPanel(
      {.viewportWidth = 640,
       .viewportHeight = 480,
       .safeTop = 0,
       .safeLeft = 0,
       .safeBottom = 0,
       .safeRight = 0});
  assert(compact.x >= 0);
  assert(compact.y >= 0);
  assert(compact.x + compact.width <= 640);
  assert(compact.y + compact.height <= 480);
  assert(compact.compact);
}

void testRankingJudgementColumnsUseOneSharedMeasurement() {
  const auto wide = ir::layoutIrRankingJudgementColumns(696.0f);
  assert(std::abs(wide.labelWidth - 152.0f) <= 0.001f);
  assert(std::abs(wide.valueWidth - (696.0f - 152.0f) / 3.0f) <= 0.001f);
  assert(std::abs(wide.labelWidth + wide.valueWidth * 3.0f - 696.0f) <= 0.001f);

  const auto compact = ir::layoutIrRankingJudgementColumns(300.0f);
  assert(compact.labelWidth < wide.labelWidth);
  assert(compact.valueWidth > 0.0f);
  assert(std::abs(compact.labelWidth + compact.valueWidth * 3.0f - 300.0f) <=
         0.001f);

  const auto empty = ir::layoutIrRankingJudgementColumns(-20.0f);
  assert(empty.labelWidth == 0.0f);
  assert(empty.valueWidth == 0.0f);
}

void testRankingDetailLampShrinksInsideCompactMetricCard() {
  View summary(0, 0, 282, 82);
  summary.setFlexDirection(FlexDirection::Row);
  summary.setAlignItems(YGAlignStretch);
  summary.setGap(10);

  View *lampCard = nullptr;
  View *lamp = nullptr;
  for (int index = 0; index < 3; ++index) {
    auto *card = new View();
    card->setFlex(1.0f)->setMinWidth(0);
    card->setPadding(Edge::All, 10);
    summary.addView(card);
    if (index == 2) {
      lampCard = card;
      lamp = new View();
      lamp->setWidth(174)->setHeight(32);
      ir::configureIrRankingDetailLampBadge(*lamp);
      card->addView(lamp);
    }
  }

  summary.applyYogaLayout();
  assert(lampCard != nullptr);
  assert(lamp != nullptr);
  assert(lampCard->getWidth() < 100);
  assert(lamp->getWidth() == lampCard->getContentWidth());
  assert(lamp->getWidth() < 174);
}

void testBlockingOverlayStopsAllInteractiveEvents() {
  View root(0, 0, 640, 480);
  auto *background = new EventRecordingView();
  auto *overlay = new BlockingOverlayView(0, 0, 640, 480);
  root.addView(background);
  root.addView(overlay);

  constexpr std::array eventTypes{
      SDL_MOUSEBUTTONDOWN, SDL_MOUSEWHEEL, SDL_FINGERDOWN, SDL_KEYDOWN,
      SDL_TEXTINPUT, SDL_TEXTEDITING, SDL_TEXTEDITING_EXT};
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

void testSiblingInsertionPreservesLayoutAndZOrders() {
  View root(0, 0, 500, 300);
  root.setFlexDirection(FlexDirection::Column);

  std::vector<int> renderOrder;
  std::vector<int> eventOrder;

  auto *content = new OrderedView(1, renderOrder, eventOrder);
  content->setName("content");
  content->setHeight(40);
  content->setZIndex(10);
  auto *actions = new OrderedView(2, renderOrder, eventOrder);
  actions->setName("resultActions");
  actions->setHeight(50);
  actions->setZIndex(-5);
  root.addView(content);
  root.addView(actions);

  SDL_Event sortEvent{};
  sortEvent.type = SDL_USEREVENT;
  root.handleEvents(sortEvent);
  assert(root.getChildren()[0] == actions && root.getChildren()[1] == content);
  eventOrder.clear();

  auto *analytics = new OrderedView(3, renderOrder, eventOrder);
  analytics->setName("timingAnalytics");
  analytics->setWidthPercent(100.0f);
  analytics->setHeight(80);
  analytics->setZIndex(0);
  root.insertViewBefore(analytics, actions);

  assert(YGNodeGetChildCount(root.getNode()) == 3);
  assert(YGNodeGetChild(root.getNode(), 0) == content->getNode());
  assert(YGNodeGetChild(root.getNode(), 1) == analytics->getNode());
  assert(YGNodeGetChild(root.getNode(), 2) == actions->getNode());
  assert(analytics->getX() == root.getContentX());
  assert(analytics->getWidth() == root.getContentWidth());
  assert(analytics->getY() == content->getY() + content->getHeight());
  assert(actions->getY() == analytics->getY() + analytics->getHeight());

  RenderContext renderContext;
  root.render(renderContext);
  assert(renderOrder == std::vector<int>({2, 3, 1}));
  root.handleEvents(sortEvent);
  assert(eventOrder == std::vector<int>({1, 3, 2}));

  renderOrder.clear();
  eventOrder.clear();
  actions->setZIndex(20);
  root.render(renderContext);
  assert(renderOrder == std::vector<int>({3, 1, 2}));
  root.handleEvents(sortEvent);
  assert(eventOrder == std::vector<int>({2, 1, 3}));
  assert(content->getY() == 0 && analytics->getY() == 40 &&
         actions->getY() == 120);

  renderOrder.clear();
  eventOrder.clear();
  content->setZIndex(0);
  analytics->setZIndex(0);
  actions->setZIndex(0);
  root.render(renderContext);
  assert(renderOrder == std::vector<int>({1, 3, 2}));
  root.handleEvents(sortEvent);
  assert(eventOrder == std::vector<int>({2, 3, 1}));

  root.clearChildren();
  assert(root.getChildren().empty() &&
         YGNodeGetChildCount(root.getNode()) == 0);
}

void testCompactResultVisualRowFitsActions() {
  const auto metrics = result_layout::metricsFor(885.0f, true);
  View root(0, 0, 1920, 885);
  root.setFlexDirection(FlexDirection::Column);
  root.setAlignItems(YGAlignStretch);
  root.setJustifyContent(YGJustifyCenter);
  root.setPadding(Edge::All, metrics.rootPadding);
  root.setGap(metrics.rootGap);

  const auto addFixedSection = [&](float height) {
    auto *section = new View();
    section->setHeight(height);
    section->setFlexShrink(0.0f);
    root.addView(section);
  };
  addFixedSection(result_layout::kHeaderHeight);
  addFixedSection(metrics.summaryHeight);
  addFixedSection(metrics.infoHeight);
  addFixedSection(metrics.detailsHeight);

  auto *visuals = new View();
  visuals->setHeight(metrics.visualHeight);
  visuals->setMinHeight(metrics.visualMinimumHeight);
  visuals->setFlexShrink(1.0f);
  visuals->setFlexDirection(FlexDirection::Row);
  visuals->setGap(metrics.visualGap);
  auto *graph = new View();
  graph->setFlexGrow(metrics.graphFlex);
  graph->setFlexBasis(0.0f);
  graph->setMinWidth(0.0f);
  auto *analytics = new View();
  analytics->setFlexGrow(metrics.analyticsFlex);
  analytics->setFlexBasis(0.0f);
  analytics->setMinWidth(0.0f);
  visuals->addView(graph);
  visuals->addView(analytics);
  root.addView(visuals);

  auto *actions = new View();
  actions->setHeight(result_layout::kActionHeight);
  actions->setFlexShrink(0.0f);
  root.addView(actions);
  root.applyYogaLayout();

  assert(graph->getWidth() < analytics->getWidth());
  assert(std::abs(static_cast<float>(graph->getWidth()) /
                      static_cast<float>(analytics->getWidth()) -
                  metrics.graphFlex / metrics.analyticsFlex) <
         0.02f);
  assert(actions->getY() + actions->getHeight() <= root.getHeight());
}

void testCompactIrFailureStatusPreservesResultActions() {
  const auto metrics = result_layout::metricsFor(885.0f, true);
  View root(0, 0, 1920, 885);
  root.setFlexDirection(FlexDirection::Column);
  root.setAlignItems(YGAlignStretch);
  root.setJustifyContent(YGJustifyCenter);
  root.setPadding(Edge::All, metrics.rootPadding);
  root.setGap(metrics.rootGap);

  const auto addFixedSection = [&](float height) {
    auto *section = new View();
    section->setHeight(height);
    section->setFlexShrink(0.0f);
    root.addView(section);
  };
  addFixedSection(result_layout::kHeaderHeight);
  addFixedSection(metrics.summaryHeight);
  addFixedSection(metrics.infoHeight);
  addFixedSection(metrics.detailsHeight);

  auto *visuals = new View();
  visuals->setName("resultVisuals");
  visuals->setHeight(metrics.visualHeight);
  visuals->setMinHeight(176.0f);
  visuals->setFlexShrink(1.0f);
  root.addView(visuals);

  auto *irFailure = new View();
  irFailure->setName("irResultStatus");
  irFailure->setHeight(72.0f);
  irFailure->setMinHeight(72.0f);
  irFailure->setFlexShrink(0.0f);
  root.addView(irFailure);

  auto *actions = new View();
  actions->setName("resultActions");
  actions->setHeight(result_layout::kActionHeight);
  actions->setMinHeight(result_layout::kActionHeight);
  actions->setFlexShrink(0.0f);
  actions->setFlexDirection(FlexDirection::Row);
  actions->setJustifyContent(YGJustifyCenter);
  actions->setGap(14.0f);
  auto *back = new View();
  back->setName("backButton");
  back->setSize(232, 64);
  auto *retry = new View();
  retry->setName("retryButton");
  retry->setSize(232, 64);
  auto *exportAction = new View();
  exportAction->setName("exportPhotoButton");
  exportAction->setSize(232, 64);
  actions->addView(back);
  actions->addView(retry);
  actions->addView(exportAction);
  root.addView(actions);

  root.applyYogaLayout();
  assert(irFailure->getVisible());
  assert(actions->getVisible());
  assert(back->getVisible() && retry->getVisible() &&
         exportAction->getVisible());
  assert(back->getWidth() > 0 && retry->getWidth() > 0 &&
         exportAction->getWidth() > 0);
  assert(actions->getY() + actions->getHeight() <= root.getHeight());
}

void testInputSettingsLayoutPolicy() {
  const auto wide = settings_scene::resolveInputSettingsLayout(1200, false);
  assert(!wide.stackSelectors);
  assert(!wide.stackBindingEditor);
  assert(wide.selectorWidth > 0 &&
         wide.selectorWidth * 3 + wide.selectorGap * 2 <= 1200);
  assert(wide.numericControlWidth > 0 &&
         wide.numericControlWidth * 5 + wide.selectorGap * 4 <= 1200);

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

void testInputBindingEditorCapabilitiesMatchControlSemantics() {
  const auto key = settings_scene::inputBindingEditorCapabilities(
      input::ControlKind::Key);
  assert(!key.deadZone && !key.activationThreshold &&
         !key.releaseThreshold && !key.inversion);
  assert(settings_scene::inputBindingEditorControlCount(key) == 1);
  const auto wideLayout =
      settings_scene::resolveInputSettingsLayout(1200, false);
  assert(settings_scene::resolveInputBindingEditorControlWidth(wideLayout,
                                                               key) == 1200);

  const auto axis = settings_scene::inputBindingEditorCapabilities(
      input::ControlKind::Axis);
  assert(axis.deadZone && axis.activationThreshold && axis.releaseThreshold &&
         axis.inversion);
  assert(settings_scene::inputBindingEditorControlCount(axis) == 5);
  assert(settings_scene::resolveInputBindingEditorControlWidth(wideLayout,
                                                               axis) == 230);

  const auto midiNote = settings_scene::inputBindingEditorCapabilities(
      input::ControlKind::MidiNote);
  assert(!midiNote.deadZone && midiNote.activationThreshold &&
         !midiNote.releaseThreshold && !midiNote.inversion);
  assert(settings_scene::inputBindingEditorControlCount(midiNote) == 2);
  assert(settings_scene::resolveInputBindingEditorControlWidth(wideLayout,
                                                               midiNote) ==
         594);

  const auto midiControl = settings_scene::inputBindingEditorCapabilities(
      input::ControlKind::MidiControl);
  assert(midiControl.deadZone && midiControl.activationThreshold &&
         midiControl.releaseThreshold && !midiControl.inversion);
  assert(settings_scene::inputBindingEditorControlCount(midiControl) == 4);

  const auto compactLayout =
      settings_scene::resolveInputSettingsLayout(520, true);
  assert(settings_scene::resolveInputBindingEditorControlWidth(compactLayout,
                                                               axis) == 520);

  for (const auto kind : {input::ControlKind::Button,
                          input::ControlKind::Hat,
                          input::ControlKind::TouchRegion}) {
    const auto digital =
        settings_scene::inputBindingEditorCapabilities(kind);
    assert(!digital.deadZone && !digital.activationThreshold &&
           !digital.releaseThreshold && !digital.inversion);
  }
}

void testLegacyDigitalScratchBindingsRemainManageable() {
  const input::InputBinding playerOneLegacy{
      .id = "legacy-p1-scratch",
      .scope = {1, 7},
      .action = {input::LogicalActionKind::Lane, 7},
  };
  const auto playerOneActions = settings_scene::inputActionsForScope(
      {1, 7}, std::span<const input::InputBinding>(&playerOneLegacy, 1));
  const auto playerOneRow = std::ranges::find_if(
      playerOneActions, [&](const auto &definition) {
        return definition.action == playerOneLegacy.action;
      });
  assert(playerOneRow != playerOneActions.end());
  assert(playerOneRow->label == "Scratch (legacy digital)");
  assert(!playerOneRow->bindable);

  const input::InputBinding playerTwoLegacy{
      .id = "legacy-p2-scratch",
      .scope = {2, 14},
      .action = {input::LogicalActionKind::Lane, 15},
  };
  const auto playerTwoActions = settings_scene::inputActionsForScope(
      {2, 14}, std::span<const input::InputBinding>(&playerTwoLegacy, 1));
  assert(std::ranges::any_of(playerTwoActions, [&](const auto &definition) {
    return definition.action == playerTwoLegacy.action &&
           definition.label == "Scratch (legacy digital)" &&
           !definition.bindable;
  }));

  const auto newProfileActions = settings_scene::inputActionsForScope(
      {1, 7}, std::span<const input::InputBinding>{});
  assert(std::ranges::none_of(newProfileActions, [](const auto &definition) {
    return definition.label == "Scratch (legacy digital)";
  }));
}

void testGyroscopeSettingsLayoutAndPresentation() {
  const auto wide = settings_scene::resolveGyroscopeSettingsLayout(900, false);
  assert(!wide.stackEditors);
  assert(wide.editorWidth > 0 && wide.editorWidth * 2 <= 900);

  const auto compact =
      settings_scene::resolveGyroscopeSettingsLayout(480, true);
  assert(compact.stackEditors);
  assert(compact.editorWidth == 480);

  const auto narrow =
      settings_scene::resolveGyroscopeSettingsLayout(540, false);
  assert(narrow.stackEditors);
  assert(narrow.editorWidth == 540);

  const auto empty = settings_scene::resolveGyroscopeSettingsLayout(-20, true);
  assert(empty.stackEditors);
  assert(empty.editorWidth == 0);

  assert(settings_scene::deviceClassLabel(input::DeviceClass::Gyroscope) ==
         "Gyroscope");
  assert(settings_scene::axisControlLabel(input::DeviceClass::Gyroscope, 0,
                                          input::ControlDirection::Positive) ==
         "Turntable +");
  assert(settings_scene::axisControlLabel(input::DeviceClass::Gyroscope, 0,
                                          input::ControlDirection::Negative) ==
         "Turntable -");
  assert(settings_scene::axisControlLabel(input::DeviceClass::Joystick, 2,
                                          input::ControlDirection::Any) ==
         "Axis 2");

  assert(settings_scene::inputDeviceStatusLabel(
             input::InputDeviceStatus::Ready) == "Ready");
  assert(settings_scene::inputDeviceStatusLabel(
             input::InputDeviceStatus::Calibrating) == "Calibrating");
  assert(settings_scene::inputDeviceStatusLabel(
             input::InputDeviceStatus::Disconnected) == "Disconnected");
  assert(settings_scene::inputDeviceStatusLabel(
             input::InputDeviceStatus::Retrying) == "Retrying");

  assert(settings_scene::parseGyroscopeSettingInteger("3") ==
         std::optional<int>{3});
  assert(settings_scene::parseGyroscopeSettingInteger("-20") ==
         std::optional<int>{-20});
  assert(!settings_scene::parseGyroscopeSettingInteger("").has_value());
  assert(!settings_scene::parseGyroscopeSettingInteger("3.0").has_value());
  assert(
      !settings_scene::parseGyroscopeSettingInteger("3 degrees").has_value());
  assert(!settings_scene::parseGyroscopeSettingInteger("999999999999999999")
              .has_value());

  assert(settings_scene::shouldShowGyroscopeSettingsCard(
      "builtin:gyroscope-turntable"));
  assert(!settings_scene::shouldShowGyroscopeSettingsCard("keyboard"));
  assert(settings_scene::kGyroscopeStepAngleLabel == "Step angle (°)");
  assert(settings_scene::kGyroscopeReleaseDelayLabel == "Release delay (ms)");
  assert(settings_scene::gyroscopeSettingsErrorLabel("").empty());
  assert(settings_scene::gyroscopeSettingsErrorLabel("disk full") ==
         "Not saved: disk full");
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
}

void testProfileInlineEditorClearsWhenUnavailable() {
  settings_scene::ProfileInlineEditorState editor;
  editor.beginRename("alpha", "Alpha");

  editor.clearIfUnavailable(true, true);
  assert(editor.activeFor("alpha"));

  editor.clearIfUnavailable(true, false);
  assert(!editor.active());

  editor.beginDuplicate("bravo", "Bravo");
  editor.clearIfUnavailable(false, true);
  assert(!editor.active());
}

} // namespace

int main() {
  testViewSkipsOffscreenPaintingButStillVisitsVisibleChildren();
  testViewSkipsZeroExtentPaintingWithoutVisibleOverflowBounds();
  testViewUsesVisibleOverflowBoundsForOwnPainting();
  testViewRotationTransformsRenderingAndScissor();
  testOverlayPortalDispatchesPresentedViewsAboveContent();
  testRankingModalPanelStaysCenteredInsideSafeArea();
  testRankingJudgementColumnsUseOneSharedMeasurement();
  testRankingDetailLampShrinksInsideCompactMetricCard();
  testBlockingOverlayStopsAllInteractiveEvents();
  testInputSettingsLayoutPolicy();
  testInputBindingEditorCapabilitiesMatchControlSemantics();
  testLegacyDigitalScratchBindingsRemainManageable();
  testGyroscopeSettingsLayoutAndPresentation();
  testInputSettingsRebuildWaitsForPointerTransaction();
  testProfileInlineEditorStaysBoundToItsCard();
  testProfileInlineEditorClearsWhenUnavailable();
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
  testSiblingInsertionPreservesLayoutAndZOrders();
  testCompactResultVisualRowFitsActions();
  testCompactIrFailureStatusPreservesResultActions();

  return 0;
}
