#include "ColorPickerView.h"

#include "../input/SDLPointerEvent.h"

namespace {
constexpr float kInset = 10;
constexpr float kHueHeight = 36;
constexpr float kGap = 16;

struct Geometry {
  float x, y, width, squareHeight, hueY;
};
Geometry geometry(const View &view) {
  const float squareHeight = std::max(0.0F, view.getHeight() - 2 * kInset - kHueHeight - kGap);
  return {view.getX() + kInset, view.getY() + kInset,
          std::max(0.0F, view.getWidth() - 2 * kInset), squareHeight,
          view.getY() + kInset + squareHeight + kGap};
}
void quad(RenderContext &context, float x, float y, float width, float height,
          std::array<std::uint32_t, 4> colors) {
  const std::array vertices{
      rendering::PosColorVertex{x, y, 0, colors[0]},
      rendering::PosColorVertex{x + width, y, 0, colors[1]},
      rendering::PosColorVertex{x + width, y + height, 0, colors[2]},
      rendering::PosColorVertex{x, y + height, 0, colors[3]}};
  constexpr std::array<std::uint16_t, 6> indices{0, 1, 2, 0, 2, 3};
  const auto program = rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  context.appendUiColor(vertices, indices, context.makeUiBatchState(
      program, BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA));
}
void disc(RenderContext &context, float x, float y, float radius, std::uint32_t color) {
  constexpr int segments = 32;
  std::array<rendering::PosColorVertex, segments + 1> vertices;
  std::array<std::uint16_t, segments * 3> indices;
  vertices[0] = {x, y, 0, color};
  for (int i = 0; i < segments; ++i) {
    const float angle = float(i) * 6.28318530718F / segments;
    vertices[i + 1] = {x + std::cos(angle) * radius, y + std::sin(angle) * radius, 0, color};
    indices[i * 3] = 0;
    indices[i * 3 + 1] = static_cast<std::uint16_t>(i + 1);
    indices[i * 3 + 2] = static_cast<std::uint16_t>((i + 1) % segments + 1);
  }
  const auto program = rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  context.appendUiColor(vertices, indices, context.makeUiBatchState(
      program, BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA));
}
std::uint32_t abgr(std::uint32_t rgb) { return Color(0xFF000000U | rgb).toABGR(); }
} // namespace

ColorPickerView::ColorPickerView(color_picker::Hsv initial, Callback onChanged)
    : current(initial), onChanged(std::move(onChanged)) {
  setWidthPercent(100);
  setHeight(240);
  setFlexShrink(0);
}

void ColorPickerView::renderImpl(RenderContext &context) {
  const auto g = geometry(*this);
  if (g.width <= 0 || g.squareHeight <= 0) return;
  const auto hue = abgr(color_picker::toRgb({current.hue, 1, 1}));
  quad(context, g.x, g.y, g.width, g.squareHeight, {0xFFFFFFFF, hue, hue, 0xFFFFFFFF});
  // A separate black alpha gradient produces the HSV square's bilinear color
  // field without a diagonal seam from interpolating only four RGB corners.
  quad(context, g.x, g.y, g.width, g.squareHeight, {0, 0, 0xFF000000, 0xFF000000});
  for (int i = 0; i < 6; ++i) {
    const auto left = abgr(color_picker::toRgb({float(i) / 6, 1, 1}));
    const auto right = abgr(color_picker::toRgb({float(i + 1) / 6, 1, 1}));
    quad(context, g.x + g.width * i / 6, g.hueY, g.width / 6, kHueHeight,
         {left, right, right, left});
  }
  const float markerX = g.x + current.saturation * g.width;
  const float markerY = g.y + (1 - current.value) * g.squareHeight;
  disc(context, markerX, markerY, 8, 0xFF000000);
  disc(context, markerX, markerY, 6, 0xFFFFFFFF);
  disc(context, markerX, markerY, 4, abgr(color_picker::toRgb(current)));
  const float hueX = g.x + current.hue * g.width;
  quad(context, hueX - 4, g.hueY - 3, 8, kHueHeight + 6,
       {0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000});
  quad(context, hueX - 2, g.hueY - 1, 4, kHueHeight + 2,
       {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF});
}

ColorPickerView::Area ColorPickerView::areaAt(float x, float y) const {
  const auto g = geometry(*this);
  if (g.width <= 0 || g.squareHeight <= 0 || x < g.x || x > g.x + g.width) return Area::None;
  if (y >= g.y && y <= g.y + g.squareHeight) return Area::SaturationValue;
  if (y >= g.hueY && y <= g.hueY + kHueHeight) return Area::Hue;
  return Area::None;
}

void ColorPickerView::updatePointer(float x, float y) {
  const auto g = geometry(*this);
  if (dragging == Area::Hue) {
    current.hue = std::clamp((x - g.x) / std::max(1.0F, g.width), 0.0F, 1.0F);
  } else if (dragging == Area::SaturationValue) {
    current.saturation = std::clamp((x - g.x) / std::max(1.0F, g.width), 0.0F, 1.0F);
    current.value = 1 - std::clamp((y - g.y) / std::max(1.0F, g.squareHeight), 0.0F, 1.0F);
  }
}

void ColorPickerView::finish() {
  if (dragging == Area::None) return;
  dragging = Area::None;
  mouseDragging = false;
  touch.reset();
  const auto callback = onChanged;
  if (callback) callback(current, true);
}

void ColorPickerView::onPointerEventConsumed(const SDL_Event &event) {
  if ((mouseDragging && event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
       event.button.which != SDL_TOUCH_MOUSEID && event.button.button == SDL_BUTTON_LEFT) ||
      (touch && (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) && !sdl_pointer_event::isMouseSynthesizedTouch(event) &&
       event.tfinger.fingerID == *touch)) finish();
}

bool ColorPickerView::handleEventsImpl(SDL_Event &event) {
  float x = 0, y = 0;
  bool released = false;
  const auto mouseToUi = [&](int rawX, int rawY) {
    rendering::screenToUi(rawX * rendering::widthScale, rawY * rendering::heightScale, x, y);
  };
  switch (event.type) {
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (event.button.which == SDL_TOUCH_MOUSEID || event.button.button != SDL_BUTTON_LEFT ||
        dragging != Area::None) return true;
    mouseToUi(event.button.x, event.button.y);
    dragging = areaAt(x, y);
    if (dragging == Area::None) return true;
    mouseDragging = true;
    break;
  case SDL_EVENT_MOUSE_MOTION:
    if (event.motion.which == SDL_TOUCH_MOUSEID || !mouseDragging) return true;
    mouseToUi(event.motion.x, event.motion.y);
    break;
  case SDL_EVENT_MOUSE_BUTTON_UP:
    if (event.button.which == SDL_TOUCH_MOUSEID || event.button.button != SDL_BUTTON_LEFT ||
        !mouseDragging) return true;
    mouseToUi(event.button.x, event.button.y);
    released = true;
    break;
  case SDL_EVENT_FINGER_DOWN:
    if (sdl_pointer_event::isMouseSynthesizedTouch(event) || dragging != Area::None) return true;
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, x, y);
    dragging = areaAt(x, y);
    if (dragging == Area::None) return true;
    touch = event.tfinger.fingerID;
    break;
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_UP:
    if (sdl_pointer_event::isMouseSynthesizedTouch(event) || !touch || *touch != event.tfinger.fingerID)
      return true;
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, x, y);
    released = event.type == SDL_EVENT_FINGER_UP;
    break;
  case SDL_EVENT_WINDOW_MOUSE_LEAVE:
  case SDL_EVENT_WINDOW_FOCUS_LOST:
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE)
      finish();
    return true;
  case SDL_EVENT_WILL_ENTER_BACKGROUND:
    finish();
    return true;
  default:
    return true;
  }
  updatePointer(x, y);
  if (released) finish();
  else {
    const auto callback = onChanged;
    if (callback) callback(current, false);
  }
  return false;
}
