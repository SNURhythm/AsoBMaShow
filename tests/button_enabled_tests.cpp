#include "../src/view/Button.h"

#include <cstdlib>
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
#define REQUIRE(condition) require((condition), #condition, __LINE__)

void require(bool condition, const char *expression, int line) {
  if (condition) {
    return;
  }
  std::cerr << "requirement failed at line " << line << ": " << expression
            << '\n';
  std::exit(1);
}

SDL_Event mouseEvent(Uint32 type, int x, int y) {
  SDL_Event event{};
  event.type = type;
  event.button.type = type;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.which = 1;
  event.button.x = x;
  event.button.y = y;
  return event;
}

SDL_Event mouseSynthesizedFingerEvent(Uint32 type, float x, float y) {
  SDL_Event event{};
  event.type = type;
  event.tfinger.type = type;
  event.tfinger.touchId = SDL_MOUSE_TOUCHID;
  event.tfinger.fingerId = 0;
  event.tfinger.x = x;
  event.tfinger.y = y;
  return event;
}

void click(Button &button) {
  auto down = mouseEvent(SDL_MOUSEBUTTONDOWN, 10, 10);
  auto up = mouseEvent(SDL_MOUSEBUTTONUP, 10, 10);
  button.handleEvents(down);
  button.handleEvents(up);
}
} // namespace

int main() {
  Button button(0, 0, 100, 50);
  int clicks = 0;
  button.setOnClickListener([&]() { ++clicks; });

  button.setEnabled(false);
  REQUIRE(!button.isEnabled());
  click(button);
  REQUIRE(clicks == 0);

  button.setEnabled(true);
  REQUIRE(button.isEnabled());
  click(button);
  REQUIRE(clicks == 1);

  auto down = mouseEvent(SDL_MOUSEBUTTONDOWN, 10, 10);
  auto up = mouseEvent(SDL_MOUSEBUTTONUP, 10, 10);
  button.handleEvents(down);
  button.setEnabled(false);
  button.handleEvents(up);
  REQUIRE(clicks == 1);

  Button trackpadButton(0, 0, 100, 50);
  int trackpadClicks = 0;
  trackpadButton.setOnClickListener([&]() { ++trackpadClicks; });
  auto syntheticDown =
      mouseSynthesizedFingerEvent(SDL_FINGERDOWN, 0.005F, 0.01F);
  auto mouseDown = mouseEvent(SDL_MOUSEBUTTONDOWN, 10, 10);
  auto syntheticUp =
      mouseSynthesizedFingerEvent(SDL_FINGERUP, 0.005F, 0.01F);
  auto mouseUp = mouseEvent(SDL_MOUSEBUTTONUP, 10, 10);
  trackpadButton.handleEvents(syntheticDown);
  trackpadButton.handleEvents(mouseDown);
  trackpadButton.handleEvents(syntheticUp);
  trackpadButton.handleEvents(mouseUp);
  REQUIRE(trackpadClicks == 1);
  return 0;
}
