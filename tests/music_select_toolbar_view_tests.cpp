#include "rendering/UniformCache.h"
#include "rendering/common.h"
#include "scene/MusicSelectToolbarView.h"
#include "view/IconText.h"
#include "view/Button.h"
#include "view/TextView.h"

#include <bgfx/bgfx.h>

#include <iostream>
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

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

MusicSelectToolbarCallbacks callbacks(std::vector<std::string> &actions,
                                      std::vector<MusicSelectToolbarState> &saved) {
  return {
      .openChartMenu = [&] { actions.emplace_back("chart-menu"); },
      .openMoreMenu = [&] { actions.emplace_back("more-menu"); },
      .openChartViewer = [&] { actions.emplace_back("viewer"); },
      .openChartRecords = [&] { actions.emplace_back("records"); },
      .openRankings = [&] { actions.emplace_back("rankings"); },
      .revealChart = [&] { actions.emplace_back("reveal"); },
      .openMusicPlayer = [&] { actions.emplace_back("music"); },
      .openTasks = [&] { actions.emplace_back("tasks"); },
      .openPlayOptions = [&] { actions.emplace_back("play-options"); },
      .openIrUploads = [&] { actions.emplace_back("ir"); },
      .openSettings = [&] { actions.emplace_back("settings"); },
      .persist = [&](MusicSelectToolbarState state) { saved.push_back(state); },
  };
}

void testExpandedShowsLabeledMenuEntrypoints() {
  std::vector<std::string> actions;
  std::vector<MusicSelectToolbarState> saved;
  auto toolbar = MusicSelectToolbarView::Create(
      {}, callbacks(actions, saved), 800, 300);
  toolbar->applyYogaLayout();
  expect(toolbar->controls().size() == 6,
         "expanded toolbar includes a direct rankings action");
  int labels = 0;
  for (const auto &control : toolbar->controls()) {
    if (control.label) {
      ++labels;
      expect(!control.label->getText().empty(), "primary actions have readable labels");
      expect(control.button && control.button->getWidth() > 48,
             "label buttons reserve more room than icon buttons");
      toolbar->activateControl(control.control);
    } else {
      expect(control.icon &&
                 control.icon->primaryFontPath() == ui_icons::kFontAwesomeSolidPath,
             "drag and collapse retain recognizable icons");
    }
  }
  expect(labels == 4 && actions == std::vector<std::string>{
             "chart-menu", "play-options", "rankings", "more-menu"},
         "labeled controls expose chart menu, play options, rankings, and more menu in order");
}

void testUnavailableCallbacksDisableButtons() {
  auto toolbar = MusicSelectToolbarView::Create({}, {}, 800, 300);
  for (std::size_t index = 0; index < toolbar->controls().size(); ++index) {
    const auto control = toolbar->controls()[index].control;
    if (control == MusicSelectToolbarControl::Drag ||
        control == MusicSelectToolbarControl::Collapse) continue;
    const auto *button = dynamic_cast<const Button *>(toolbar->getChildren()[index]);
    expect(button && !button->isEnabled(),
           "an unavailable toolbar action is rendered as a disabled Button");
  }
}

void testDisabledActionsStayDisabledAcrossRebuilds() {
  std::vector<std::string> actions;
  std::vector<MusicSelectToolbarState> saved;
  auto toolbar = MusicSelectToolbarView::Create({}, callbacks(actions, saved), 800, 300);
  toolbar->setControlEnabled(MusicSelectToolbarControl::Rankings, false);
  toolbar->activateControl(MusicSelectToolbarControl::Rankings);
  expect(actions.empty(), "disabled toolbar actions cannot dispatch callbacks");
  toolbar->applyState({.mode = MusicSelectToolbarMode::Collapsed});
  toolbar->applyState({.mode = MusicSelectToolbarMode::Expanded});
  toolbar->setViewportSize(300, 600);
  expect(!toolbar->isControlEnabled(MusicSelectToolbarControl::Rankings),
         "disabled menu action state survives expand and viewport rebuilds");
  toolbar->activateControl(MusicSelectToolbarControl::Rankings);
  expect(actions.empty(), "rebuilding cannot re-enable an ineligible menu action");
  toolbar->setControlEnabled(MusicSelectToolbarControl::Rankings, true);
  toolbar->activateControl(MusicSelectToolbarControl::Rankings);
  expect(actions == std::vector<std::string>{"rankings"},
         "a newly eligible selection re-enables the rankings action");
}

void testCollapsedAndHiddenShapes() {
  std::vector<std::string> actions;
  std::vector<MusicSelectToolbarState> saved;
  MusicSelectToolbarState collapsed{
      .mode = MusicSelectToolbarMode::Collapsed};
  auto toolbar = MusicSelectToolbarView::Create(
      collapsed, callbacks(actions, saved), rendering::window_width,
      rendering::window_height);
  expect(toolbar != nullptr && toolbar->controls().size() == 2,
         "collapsed toolbar has only drag and expand");
  expect(toolbar->controls()[0].codepoint == ui_icons::kDrag &&
             toolbar->controls()[1].codepoint == ui_icons::kExpand,
         "collapsed toolbar uses exact drag and expand icons");

  MusicSelectToolbarState hidden{.mode = MusicSelectToolbarMode::Hidden};
  expect(MusicSelectToolbarView::Create(
             hidden, callbacks(actions, saved), rendering::window_width,
             rendering::window_height) == nullptr,
         "hidden mode constructs no toolbar view or hit target");
}

void testExpandedToolbarWrapsWithinANarrowViewport() {
  std::vector<std::string> actions;
  std::vector<MusicSelectToolbarState> saved;
  auto toolbar = MusicSelectToolbarView::Create({}, callbacks(actions, saved),
                                                 800, 300);
  toolbar->applyYogaLayout();
  expect(toolbar->getWidth() < 800 && toolbar->getHeight() == 66,
         "expanded controls fit on one row in a wide viewport");

  toolbar->setViewportSize(260, 300);
  toolbar->applyYogaLayout();
  expect(toolbar->getWidth() <= 260 && toolbar->getHeight() > 64,
         "a narrower viewport reflows expanded controls within its bounds");
}

void testControlsFitInsideToolbar() {
  std::vector<std::string> actions;
  std::vector<MusicSelectToolbarState> saved;
  for (const auto mode : {MusicSelectToolbarMode::Expanded,
                          MusicSelectToolbarMode::Collapsed}) {
    auto toolbar = MusicSelectToolbarView::Create(
        {.mode = mode}, callbacks(actions, saved), 800, 400);
    for (const int width : {800, 604, 606, 500, 260, 120, 118, 66}) {
      toolbar->setViewportSize(width, 800);
      toolbar->applyYogaLayout();
      expect(toolbar->getWidth() <= width,
             "toolbar including its border fits within the viewport");
      for (const auto *child : toolbar->getChildren()) {
        expect(child->getX() >= toolbar->getX() + 9 &&
                   child->getY() >= toolbar->getY() + 9 &&
                   child->getX() + child->getWidth() <=
                       toolbar->getX() + toolbar->getWidth() - 9 &&
                   child->getY() + child->getHeight() <=
                       toolbar->getY() + toolbar->getHeight() - 9,
               "each control fits inside the toolbar border and padding");
        if (width == 800) {
          expect(child->getY() == toolbar->getChildren().front()->getY(),
                 "all controls stay on one row when the viewport is wide");
        }
      }
    }
  }
}

void testActionsModesAndDragPersist() {
  std::vector<std::string> actions;
  std::vector<MusicSelectToolbarState> saved;
  auto toolbar = MusicSelectToolbarView::Create(
      {}, callbacks(actions, saved), 800, 300);
  toolbar->applyYogaLayout();
  toolbar->activateControl(MusicSelectToolbarControl::ChartViewer);
  toolbar->activateControl(MusicSelectToolbarControl::ChartRecords);
  toolbar->activateControl(MusicSelectToolbarControl::RevealChart);
  toolbar->activateControl(MusicSelectToolbarControl::MusicPlayer);
  toolbar->activateControl(MusicSelectToolbarControl::Tasks);
  toolbar->activateControl(MusicSelectToolbarControl::PlayOptions);
  toolbar->activateControl(MusicSelectToolbarControl::IrUploads);
  toolbar->activateControl(MusicSelectToolbarControl::Settings);
  expect(actions == std::vector<std::string>({"viewer", "records", "reveal",
                                               "music", "tasks", "play-options",
                                               "ir", "settings"}),
         "toolbar exposes chart and application actions");

  toolbar->activateControl(MusicSelectToolbarControl::Collapse);
  expect(toolbar->controls().size() == 6,
         "collapse keeps event targets alive until deferred callbacks run");
  View::dispatchDeferredEventCallbacks();
  expect(toolbar->state().mode == MusicSelectToolbarMode::Collapsed &&
             toolbar->controls().size() == 2,
         "collapse persists and rebuilds the toolbar");
  expect(toolbar->getX() == 24 && toolbar->getY() == 24,
         "mode changes retain the declared default placement");
  toolbar->activateControl(MusicSelectToolbarControl::Expand);
  View::dispatchDeferredEventCallbacks();
  expect(toolbar->state().mode == MusicSelectToolbarMode::Expanded &&
             toolbar->controls().size() == 6,
         "expand persists and rebuilds the toolbar");

  toolbar->applyYogaLayout();
  const int startX = toolbar->getX() + 20;
  const int startY = toolbar->getY() + 20;
  SDL_Event down{};
  down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.x = startX;
  down.button.y = startY;
  expect(!toolbar->handleEvents(down), "drag handle consumes pointer down");
  SDL_Event motion{};
  motion.type = SDL_EVENT_MOUSE_MOTION;
  motion.motion.x = startX + 70;
  motion.motion.y = startY + 35;
  expect(!toolbar->handleEvents(motion), "active drag consumes pointer motion");
  const int draggedX = toolbar->getX();
  const int draggedY = toolbar->getY();
  toolbar->setViewportSize(800, 300);
  expect(toolbar->getX() == draggedX && toolbar->getY() == draggedY,
         "per-frame viewport updates preserve in-progress drag placement");
  SDL_Event up{};
  up.type = SDL_EVENT_MOUSE_BUTTON_UP;
  up.button.button = SDL_BUTTON_LEFT;
  up.button.x = startX + 70;
  up.button.y = startY + 35;
  expect(!toolbar->handleEvents(up), "active drag consumes pointer up");
  expect(toolbar->state().hasPosition && !saved.empty() &&
             saved.back() == toolbar->state(),
         "drag release persists the final authored position");
}

void testPersistedSettingsStateAppliesToAnExistingToolbar() {
  std::vector<std::string> actions;
  std::vector<MusicSelectToolbarState> saved;
  auto toolbar = MusicSelectToolbarView::Create(
      {.mode = MusicSelectToolbarMode::Collapsed}, callbacks(actions, saved),
      800, 300);
  toolbar->applyState({.mode = MusicSelectToolbarMode::Hidden});
  expect(!toolbar->getVisible() && toolbar->controls().empty() && saved.empty(),
         "returning from Settings hides the retained toolbar without saving "
         "the already-persisted choice again");

  toolbar->applyState({.mode = MusicSelectToolbarMode::Expanded,
                       .x = 80.0F,
                       .y = 60.0F,
                       .hasPosition = true});
  expect(toolbar->getVisible() && toolbar->controls().size() == 6 &&
             toolbar->getX() == 80 && toolbar->getY() == 60 && saved.empty(),
         "returning from Settings rebuilds and places the retained toolbar "
         "from persisted state");
}

void testDisabledChildReleaseFinishesToolbarDrag() {
  for (const bool touch : {false, true}) {
    std::vector<std::string> actions;
    std::vector<MusicSelectToolbarState> saved;
    auto toolbar = MusicSelectToolbarView::Create(
        {}, callbacks(actions, saved), 800, 300);
    toolbar->setControlEnabled(MusicSelectToolbarControl::ChartMenu, false);
    toolbar->applyYogaLayout();
    const auto pointerEvent = [touch](Uint32 mouseType, Uint32 touchType,
                                     int x, int y, SDL_FingerID finger = 91) {
      SDL_Event event{};
      event.type = touch ? touchType : mouseType;
      if (touch) {
        event.tfinger.touchID = 1;
        event.tfinger.fingerID = finger;
        event.tfinger.x = static_cast<float>(x) / rendering::render_width;
        event.tfinger.y = static_cast<float>(y) / rendering::render_height;
      } else if (mouseType == SDL_EVENT_MOUSE_MOTION) {
        event.motion.x = x;
        event.motion.y = y;
      } else {
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = x;
        event.button.y = y;
      }
      return event;
    };
    auto down = pointerEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_FINGER_DOWN,
                             toolbar->getX() + 20, toolbar->getY() + 20);
    expect(!toolbar->handleEvents(down), "toolbar begins the release regression drag");
    auto motion = pointerEvent(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_FINGER_MOTION, 800, 44);
    toolbar->handleEvents(motion);
    const int clampedX = toolbar->getX();
    const int clampedY = toolbar->getY();
    expect(clampedX + toolbar->getWidth() == 800,
           "dragging beyond the viewport clamps the toolbar at its right edge");
    const auto *chart = toolbar->controls()[1].button;
    auto up = pointerEvent(SDL_EVENT_MOUSE_BUTTON_UP, SDL_EVENT_FINGER_UP,
                           chart->getX() + chart->getWidth() / 2,
                           chart->getY() + chart->getHeight() / 2);
    auto unrelatedUp = up;
    if (touch) unrelatedUp.tfinger.fingerID = 92;
    else unrelatedUp.button.button = SDL_BUTTON_RIGHT;
    expect(!toolbar->handleEvents(unrelatedUp),
           "disabled chart button consumes an unrelated pointer release");
    auto syntheticUp = up;
    if (touch) syntheticUp.tfinger.touchID = SDL_MOUSE_TOUCHID;
    else syntheticUp.button.which = SDL_TOUCH_MOUSEID;
    expect(!toolbar->handleEvents(syntheticUp),
           "disabled chart button consumes a synthesized pointer release");
    expect(saved.empty(), "unrelated and synthesized releases do not finish the drag");
    motion = pointerEvent(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_FINGER_MOTION, clampedX - 20, 44);
    toolbar->handleEvents(motion);
    expect(toolbar->getX() < clampedX,
           "the original drag remains active after unrelated releases");
    motion = pointerEvent(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_FINGER_MOTION, 800, 44);
    toolbar->handleEvents(motion);

    expect(!toolbar->handleEvents(up), "disabled chart button consumes the drag release");
    expect(saved.size() == 1 && saved.back().hasPosition &&
               saved.back().x == clampedX && saved.back().y == clampedY,
           "a consumed drag release persists the current clamped toolbar position");
    motion = pointerEvent(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_FINGER_MOTION, 80, 100);
    toolbar->handleEvents(motion);
    expect(toolbar->getX() == clampedX && toolbar->getY() == clampedY,
           "motion after a consumed release cannot keep dragging the toolbar");

    down = pointerEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_FINGER_DOWN,
                         toolbar->getX() + 20, toolbar->getY() + 20, 92);
    expect(!toolbar->handleEvents(down), "a new pointer can start the next toolbar drag");
    const int nextStartX = toolbar->getX();
    motion = pointerEvent(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_FINGER_MOTION,
                           nextStartX - 40, toolbar->getY() + 20, 92);
    toolbar->handleEvents(motion);
    expect(toolbar->getX() < nextStartX,
           "the next pointer moves the toolbar after the previous release was consumed");
    expect(actions.empty(), "releasing over a disabled chart control never activates it");
  }
}
} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  if (!bgfx::init(init)) {
    std::cerr << "FAIL: bgfx noop initialization failed\n";
    return 1;
  }
  testExpandedShowsLabeledMenuEntrypoints();
  testUnavailableCallbacksDisableButtons();
  testDisabledActionsStayDisabledAcrossRebuilds();
  testCollapsedAndHiddenShapes();
  testExpandedToolbarWrapsWithinANarrowViewport();
  testControlsFitInsideToolbar();
  testActionsModesAndDragPersist();
  testPersistedSettingsStateAppliesToAnExistingToolbar();
  testDisabledChildReleaseFinishesToolbarDrag();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  if (failures != 0) {
    std::cerr << failures << " music select toolbar test(s) failed\n";
    return 1;
  }
  std::cout << "music select toolbar view tests passed\n";
  return 0;
}
