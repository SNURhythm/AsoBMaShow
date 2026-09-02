#include "rendering/UniformCache.h"
#include "rendering/common.h"
#include "scene/MusicSelectToolbarView.h"
#include "view/IconText.h"
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
      .openMusicPlayer = [&] { actions.emplace_back("music"); },
      .openChartViewer = [&] { actions.emplace_back("viewer"); },
      .openChartRecords = [&] { actions.emplace_back("records"); },
      .revealChart = [&] { actions.emplace_back("reveal"); },
      .openTasks = [&] { actions.emplace_back("tasks"); },
      .openPlayOptions = [&] { actions.emplace_back("play-options"); },
      .openIrUploads = [&] { actions.emplace_back("ir"); },
      .openSettings = [&] { actions.emplace_back("settings"); },
      .persist = [&](MusicSelectToolbarState state) { saved.push_back(state); },
  };
}

void testExpandedUsesOnlyExactFontAwesomeControls() {
  std::vector<std::string> actions;
  std::vector<MusicSelectToolbarState> saved;
  auto toolbar = MusicSelectToolbarView::Create(
      {}, callbacks(actions, saved), rendering::window_width,
      rendering::window_height);
  expect(toolbar != nullptr, "expanded state constructs a toolbar");
  const std::vector<std::uint32_t> expected = {
      ui_icons::kDrag,        ui_icons::kChartLine, ui_icons::kRecords,
      ui_icons::kReveal,      ui_icons::kMusic,     ui_icons::kTasks,
      ui_icons::kPlayOptions, ui_icons::kIrUploads, ui_icons::kSettings,
      ui_icons::kCollapse,    ui_icons::kHide};
  expect(toolbar->controls().size() == expected.size(),
         "expanded toolbar has drag plus chart and application controls");
  for (std::size_t index = 0;
       index < toolbar->controls().size() && index < expected.size(); ++index) {
    const auto &control = toolbar->controls()[index];
    expect(control.codepoint == expected[index],
           "expanded control uses its assigned codepoint");
    expect(control.icon->primaryFontPath() == ui_icons::kFontAwesomeSolidPath,
           "every toolbar control uses Font Awesome Solid");
    expect(control.icon->getText() ==
               ui_icons::textForCodepoint(expected[index]),
           "every toolbar control contains only its icon glyph");
  }
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
                                                 500, 300);
  toolbar->applyYogaLayout();
  expect(toolbar->getWidth() <= 500 && toolbar->getHeight() > 64,
         "expanded controls wrap instead of extending past a narrow viewport");

  toolbar->setViewportSize(260, 300);
  toolbar->applyYogaLayout();
  expect(toolbar->getWidth() <= 260 && toolbar->getHeight() > 64,
         "a narrower viewport reflows expanded controls within its bounds");
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
  View::dispatchDeferredEventCallbacks();
  expect(toolbar->state().mode == MusicSelectToolbarMode::Collapsed &&
             toolbar->controls().size() == 2,
         "collapse persists and rebuilds the toolbar");
  expect(toolbar->getX() == 24 && toolbar->getY() == 24,
         "mode changes retain the declared default placement");
  toolbar->activateControl(MusicSelectToolbarControl::Expand);
  View::dispatchDeferredEventCallbacks();
  expect(toolbar->state().mode == MusicSelectToolbarMode::Expanded &&
             toolbar->controls().size() == 11,
         "expand persists and rebuilds the toolbar");

  toolbar->applyYogaLayout();
  const int startX = toolbar->getX() + 20;
  const int startY = toolbar->getY() + 20;
  SDL_Event down{};
  down.type = SDL_MOUSEBUTTONDOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.x = startX;
  down.button.y = startY;
  expect(!toolbar->handleEvents(down), "drag handle consumes pointer down");
  SDL_Event motion{};
  motion.type = SDL_MOUSEMOTION;
  motion.motion.x = startX + 70;
  motion.motion.y = startY + 35;
  expect(!toolbar->handleEvents(motion), "active drag consumes pointer motion");
  const int draggedX = toolbar->getX();
  const int draggedY = toolbar->getY();
  toolbar->setViewportSize(800, 300);
  expect(toolbar->getX() == draggedX && toolbar->getY() == draggedY,
         "per-frame viewport updates preserve in-progress drag placement");
  SDL_Event up{};
  up.type = SDL_MOUSEBUTTONUP;
  up.button.button = SDL_BUTTON_LEFT;
  up.button.x = startX + 70;
  up.button.y = startY + 35;
  expect(!toolbar->handleEvents(up), "active drag consumes pointer up");
  expect(toolbar->state().hasPosition && !saved.empty() &&
             saved.back() == toolbar->state(),
         "drag release persists the final authored position");

  toolbar->activateControl(MusicSelectToolbarControl::Hide);
  expect(toolbar->state().mode == MusicSelectToolbarMode::Hidden,
         "hide persists hidden state for owner removal");
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
  expect(toolbar->getVisible() && toolbar->controls().size() == 11 &&
             toolbar->getX() == 80 && toolbar->getY() == 60 && saved.empty(),
         "returning from Settings rebuilds and places the retained toolbar "
         "from persisted state");
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
  testExpandedUsesOnlyExactFontAwesomeControls();
  testCollapsedAndHiddenShapes();
  testExpandedToolbarWrapsWithinANarrowViewport();
  testActionsModesAndDragPersist();
  testPersistedSettingsStateAppliesToAnExistingToolbar();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  if (failures != 0) {
    std::cerr << failures << " music select toolbar test(s) failed\n";
    return 1;
  }
  std::cout << "music select toolbar view tests passed\n";
  return 0;
}
