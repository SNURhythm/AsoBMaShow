#include "rendering/UniformCache.h"
#include "view/TextInputBox.h"

#include <SDL2/SDL.h>

#include <cstdlib>
#include <iostream>
#include <string>

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

int clearCompositionCalls = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

class InspectableTextInputBox final : public TextInputBox {
public:
  using TextInputBox::TextInputBox;

  [[nodiscard]] SDL_Rect textRect() const { return resolvedTextRect(); }
};

void click(TextInputBox &input, int x, int y) {
  SDL_Event down{};
  down.type = SDL_MOUSEBUTTONDOWN;
  down.button.type = SDL_MOUSEBUTTONDOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.which = 1;
  down.button.x = x;
  down.button.y = y;
  SDL_Event up = down;
  up.type = SDL_MOUSEBUTTONUP;
  up.button.type = SDL_MOUSEBUTTONUP;
  input.handleEvents(down);
  input.handleEvents(up);
}

void testDefaultHorizontalPadding() {
  InspectableTextInputBox input("assets/fonts/notosanscjkjp.ttf", 18);
  input.setSize(240, 52);
  input.applyYogaLayout();
  input.setEditingText("query");

  expect(input.textRect().x == input.getX() + 12,
         "left-aligned text uses the default leading inset");
}

void testClearButtonVisibilityAndCallback() {
  TextInputBox input("assets/fonts/notosanscjkjp.ttf", 18);
  input.setSize(240, 52);
  input.applyYogaLayout();
  input.setEditingText("query");

  int notifications = 0;
  std::string lastText;
  input.onTextChanged([&](const std::string &text) {
    ++notifications;
    lastText = text;
  });
  input.setClearable(true);

  expect(input.isClearButtonVisible(),
         "configured non-empty input shows its clear button");
  clearCompositionCalls = 0;
  click(input, input.getX() + input.getWidth() - 18,
        input.getY() + input.getHeight() / 2);
  expect(input.getText().empty(), "clear button clears the editing value");
  expect(clearCompositionCalls == 1,
         "clear button cancels the platform IME composition");
  expect(notifications == 1 && lastText.empty(),
         "clear button publishes exactly one normal text change");
  expect(!input.isClearButtonVisible(),
         "clear button hides after the field becomes empty");
}

void testEmptyInputHasNoClearHitTarget() {
  TextInputBox input("assets/fonts/notosanscjkjp.ttf", 18);
  input.setSize(240, 52);
  input.applyYogaLayout();
  input.setEditingText("");
  input.setClearable(true);

  expect(!input.isClearButtonVisible(),
         "configured empty input keeps its clear button hidden");
  click(input, input.getX() + input.getWidth() - 18,
        input.getY() + input.getHeight() / 2);
  expect(input.getSelected(),
         "empty input trailing edge remains part of the text field");
  input.onUnselected();
}

} // namespace

extern "C" void SDLCALL TextInputBoxTest_ClearComposition() {
  ++clearCompositionCalls;
}

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  expect(bgfx::init(init), "headless bgfx initializes for text input tests");

  testDefaultHorizontalPadding();
  testClearButtonVisibilityAndCallback();
  testEmptyInputHasNoClearHitTarget();

  TextInputBox::releaseCachedCursors();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
