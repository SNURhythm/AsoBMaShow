#include "../src/scene/DecideLoadingOverlay.h"
#include "../src/view/OverlayPortal.h"
#include "../src/view/UiTheme.h"
#include "../src/view/ImageView.h"
#include "../src/view/TextView.h"
#include "../src/rendering/UniformCache.h"

#include <bgfx/bgfx.h>
#include <cstdio>
#include <cmath>
#include <filesystem>
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
void testOverlayReflowsForPortraitAndRotation() {
  auto record = makeRecord();
  record.meta.Title = "A deliberately long chart title that needs wrapping in portrait";
  record.meta.SubTitle.clear();
  record.meta.BmsPath = std::filesystem::path(__FILE__).parent_path() /
      "fixtures/beatoraja_skin/charts/preview.bms";
  record.meta.StageFile = "acceptance_bga_base.png";
  DecideLoadingOverlay overlay(0, 0, 1920, 1080, record);
  for (const auto size : {std::pair{1080, 1920}, std::pair{320, 900},
                          std::pair{720, 1280}, std::pair{1920, 1080}}) {
    overlay.setSize(size.first, size.second);
    auto *panel = overlay.getChildren().front();
    expect(panel->getX() >= 24 && panel->getX() + panel->getWidth() <= size.first - 24,
           "decide panel fits the current viewport width after rotation");
    expect(std::abs(panel->getX() + panel->getWidth() / 2 - size.first / 2) <= 1 &&
               std::abs(panel->getY() + panel->getHeight() / 2 - size.second / 2) <= 1,
           "decide panel remains centered in the current orientation");
    bool foundStage = false;
    for (auto *child : panel->getChildren()) {
      expect(child->getX() >= panel->getContentX() &&
                 child->getX() + child->getWidth() <= panel->getContentX() + panel->getContentWidth(),
             "decide image and metadata fit inside the portrait card");
      auto *image = dynamic_cast<ImageView *>(child);
      if (!image && !child->getChildren().empty())
        image = dynamic_cast<ImageView *>(child->getChildren().front());
      if (image) {
        foundStage = true;
        expect(image->getWidth() <= panel->getContentWidth(),
               "stage image stays inside the portrait card");
        expect(std::abs(image->getWidth() * 9 - image->getHeight() * 16) <= 16,
               "decide stage image preserves its frame aspect ratio when resized");
      }
      if (auto *text = dynamic_cast<TextView *>(child)) {
        expect(text->textureWidth() <= text->getContentWidth(),
               "decide metadata wraps instead of extending beyond the viewport");
      }
    }
    expect(foundStage, "decide stage image remains present after rotation");
  }
  overlay.setSize(1080, 1920);
  overlay.setChart(makeRecord());
  auto *panel = overlay.getChildren().front();
  expect(panel->getX() >= 24 && panel->getX() + panel->getWidth() <= 1056,
         "changing the chart keeps the current portrait layout");
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
  testOverlayReflowsForPortraitAndRotation();

  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  if (failures != 0) {
    std::cerr << failures << " failures\n";
    return 1;
  }
  std::cout << "Decide loading overlay tests passed\n";
  return 0;
}