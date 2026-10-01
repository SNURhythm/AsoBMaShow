#include "../src/view/View.h"
#include <functional>
#include <memory>
#include <string>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "../src/view/Button.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

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

SDL_Event mouseMotion(Uint32 which, int x, int y) {
  SDL_Event event{};
  event.type = SDL_MOUSEMOTION;
  event.motion.which = which;
  event.motion.x = x;
  event.motion.y = y;
  return event;
}

SDL_Event fingerEvent(Uint32 type, int x, int y) {
  SDL_Event event{};
  event.type = type;
  event.tfinger.touchId = 1;
  event.tfinger.fingerId = 7;
  event.tfinger.x = static_cast<float>(x) / rendering::window_width;
  event.tfinger.y = static_cast<float>(y) / rendering::window_height;
  return event;
}

void testTouchReleaseClearsHoverBeforeClick() {
  for (const bool releaseInside : {true, false}) {
    Button button(0, 0, 100, 50);
    int clicks = 0;
    button.setOnClickListener([&]() {
      REQUIRE(!button.isHovered);
      REQUIRE(button.activeTouchId == -1);
      ++clicks;
    });
    auto mouse = mouseMotion(1, 10, 10);
    button.handleEvents(mouse);
    REQUIRE(button.isHovered);
    auto down = fingerEvent(SDL_FINGERDOWN, 10, 10);
    REQUIRE(!button.handleEvents(down));
    auto syntheticMotion = mouseMotion(SDL_TOUCH_MOUSEID, 10, 10);
    button.handleEvents(syntheticMotion);
    auto up = fingerEvent(SDL_FINGERUP, releaseInside ? 10 : 150, 10);
    REQUIRE(!button.handleEvents(up));
    REQUIRE(!button.isHovered);
    REQUIRE(button.activeTouchId == -1);
    REQUIRE(clicks == (releaseInside ? 1 : 0));

    // SDL may deliver its touch-generated mouse motion after finger-up.
    button.handleEvents(syntheticMotion);
    REQUIRE(!button.isHovered);
    button.handleEvents(mouse);
    REQUIRE(button.isHovered);
    auto mouseLeave = mouseMotion(1, 150, 10);
    button.handleEvents(mouseLeave);
    REQUIRE(!button.isHovered);
  }
}

void click(Button &button) {
  auto down = mouseEvent(SDL_MOUSEBUTTONDOWN, 10, 10);
  auto up = mouseEvent(SDL_MOUSEBUTTONUP, 10, 10);
  button.handleEvents(down);
  button.handleEvents(up);
}
} // namespace

int main() {
  testTouchReleaseClearsHoverBeforeClick();
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
