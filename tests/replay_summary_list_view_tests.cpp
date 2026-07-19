#include "../src/view/Button.h"
#include "../src/view/ReplaySummaryListView.h"
#include "../src/rendering/UniformCache.h"

#include <SDL2/SDL.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <vector>

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

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

ReplaySummary summary(int id, bool pending = true) {
  ReplaySummary value;
  value.id = id;
  value.createdAt = "2026-07-19 12:00 UTC";
  value.finalScore = 1500;
  value.maxScore = 2000;
  value.maxCombo = 700;
  value.irUploadPending = pending;
  return value;
}

Button *uploadButton(ReplaySummaryListView &list, int index) {
  auto *row = list.getViewByIndex(index);
  return row == nullptr
             ? nullptr
             : dynamic_cast<Button *>(row->findViewByName("irUploadBadge"));
}

void clickThroughList(ReplaySummaryListView &list, const Button &button) {
  SDL_Event down{};
  down.type = SDL_MOUSEBUTTONDOWN;
  down.button.type = SDL_MOUSEBUTTONDOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.which = 1;
  down.button.x = button.getX() + button.getWidth() / 2;
  down.button.y = button.getY() + button.getHeight() / 2;
  SDL_Event up = down;
  up.type = SDL_MOUSEBUTTONUP;
  up.button.type = SDL_MOUSEBUTTONUP;
  list.handleEvents(down);
  list.handleEvents(up);
}

} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for replay list");

  {
    ReplaySummaryListView list;
    list.setSize(700, 160);
    list.applyYogaLayout();

    int uploads = 0;
    int uploadedReplayId = 0;
    int selections = 0;
    list.onIrUploadRequested = [&](const ReplaySummary &value) {
      ++uploads;
      uploadedReplayId = value.id;
    };
    list.onSelectionChanged = [&](int) { ++selections; };
    list.setReplaySummaries({summary(11), summary(12, false)});

    auto *button = uploadButton(list, 0);
    require(button != nullptr && button->getVisible(),
            "pending replay exposes an upload button");
    clickThroughList(list, *button);
    require(uploads == 1 && uploadedReplayId == 11,
            "upload button dispatches the bound replay");
    require(selections == 0 && list.selectedReplayIndex() == -1,
            "upload button does not select its row");

    list.setIrUploadInProgress(11);
    button = uploadButton(list, 0);
    require(button != nullptr && button->isEnabled(),
            "busy marker remains an event sink");
    clickThroughList(list, *button);
    require(uploads == 1, "disabled upload marker ignores repeat clicks");
    require(selections == 0 && list.selectedReplayIndex() == -1,
            "busy upload marker does not fall through to row selection");

    list.setIrUploadInProgress(std::nullopt);
    list.setReplaySummaries({summary(21)});
    button = uploadButton(list, 0);
    require(button != nullptr && button->isEnabled(),
            "rebound upload marker is enabled");
    clickThroughList(list, *button);
    require(uploads == 2 && uploadedReplayId == 21,
            "recycled row dispatches its newly bound replay");
  }

  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
