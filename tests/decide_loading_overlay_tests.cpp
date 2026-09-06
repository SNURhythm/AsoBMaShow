#include "../src/scene/DecideLoadingOverlay.h"
#include "../src/view/OverlayPortal.h"
#include "../src/view/UiTheme.h"
#include "../src/rendering/UniformCache.h"

#include <bgfx/bgfx.h>
#include <cstdio>
#include <iostream>
#include <string>

// The view layer references these rendering globals (main.cpp defines them in
// the app); tests provide fixed values so linking is self-contained.
namespace rendering {
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
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
}  // namespace rendering

namespace {
int failures = 0;
void expect(bool value, const std::string &message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

ChartMetaRecord makeRecord() {
  ChartMetaRecord record;
  record.meta.Title = "Black Wings";
  record.meta.SubTitle = "[Another]";
  record.meta.Artist = "Abel & litmus* feat. nayuta";
  record.meta.PlayLevelText = "12";
  record.meta.BmsPath = "/tmp/chart/black_wings.bms";
  return record;
}

void testOverlayBindsChartMetadata() {
  const auto record = makeRecord();
  DecideLoadingOverlay overlay(0, 0, 1280, 720, record);
  expect(overlay.titleText() == "Black Wings [Another]",
         "title includes subtitle");
  expect(overlay.artistText() == "Abel & litmus* feat. nayuta",
         "artist is bound");
  expect(overlay.difficultyText() == "12", "difficulty text is bound");

  ChartMetaRecord other;
  other.meta.Title = "Beatrice";
  other.meta.Artist = "xi";
  overlay.setChart(other);
  expect(overlay.titleText() == "Beatrice", "setChart rebinds title");
  expect(overlay.artistText() == "xi", "setChart rebinds artist");
}

void testOverlayBlocksInput() {
  const auto record = makeRecord();
  DecideLoadingOverlay overlay(0, 0, 1280, 720, record);
  overlay.setVisible(true);

  SDL_Event keyEvent{};
  keyEvent.type = SDL_KEYDOWN;
  keyEvent.key.keysym.sym = SDLK_RETURN;

  // BlockingOverlayView consumes input (handleEventsImpl returns false for
  // interaction events), so OverlayPortal should not pass it to content.
  SDL_Event clickEvent{};
  clickEvent.type = SDL_MOUSEBUTTONDOWN;
  clickEvent.button.button = SDL_BUTTON_LEFT;

  // If the overlay did not consume these, handleEvents would forward them.
  // Directly verify the overlay swallows them (returns false = consumed).
  expect(!overlay.handleEvents(keyEvent), "overlay consumes keyboard events");
  expect(!overlay.handleEvents(clickEvent), "overlay consumes mouse events");
}

void testOverlayReuseAcrossCharts() {
  const auto first = makeRecord();
  DecideLoadingOverlay overlay(0, 0, 1280, 720, first);
  overlay.setVisible(true);

  ChartMetaRecord second;
  second.meta.Title = "Another Chart";
  second.meta.Artist = "Someone";
  overlay.setChart(second);
  overlay.setChart(first);  // switch back
  expect(overlay.titleText() == "Black Wings [Another]",
         "overlay re-renders cleanly when chart switches back");
}
}  // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  if (!bgfx::init(init)) {
    std::cerr << "FAIL: headless bgfx could not initialize\n";
    return 1;
  }
  ui_theme::setActiveMode(ui_theme::ThemeMode::Dark);

  testOverlayBindsChartMetadata();
  testOverlayBlocksInput();
  testOverlayReuseAcrossCharts();

  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  if (failures != 0) {
    std::cerr << failures << " failures\n";
    return 1;
  }
  std::cout << "Decide loading overlay tests passed\n";
  return 0;
}