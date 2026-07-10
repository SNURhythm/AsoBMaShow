//
// Created by XF on 8/25/2024.
//

#include "Button.h"

#include <cmath>
#include <utility>

namespace {
constexpr float kPi = 3.14159265358979323846f;

bool isInsideButton(const Button &button, float uiX, float uiY) {
  return uiX >= button.getX() && uiX <= button.getX() + button.getWidth() &&
         uiY >= button.getY() && uiY <= button.getY() + button.getHeight();
}

void mouseEventToUi(const SDL_MouseButtonEvent &event, int &uiX, int &uiY) {
  const int screenX = static_cast<int>(event.x * rendering::widthScale);
  const int screenY = static_cast<int>(event.y * rendering::heightScale);
  rendering::screenToUi(screenX, screenY, uiX, uiY);
}

void mouseCoordsToUi(int rawX, int rawY, int &uiX, int &uiY) {
  const int screenX = static_cast<int>(rawX * rendering::widthScale);
  const int screenY = static_cast<int>(rawY * rendering::heightScale);
  rendering::screenToUi(screenX, screenY, uiX, uiY);
}

void fingerEventToUi(const SDL_TouchFingerEvent &event, float &uiX,
                     float &uiY) {
  rendering::normalizedToUi(event.x, event.y, uiX, uiY);
}

void drawButtonRect(const RenderContext &context, int x, int y, int width,
                    int height, float radius, const Color &color) {
  if (width <= 0 || height <= 0 || color.a == 0) {
    return;
  }
  radius = std::clamp(radius, 0.0f,
                      static_cast<float>(std::min(width, height)) * 0.5f);
  if (radius > 0.5f) {
    const int segments =
        std::clamp(static_cast<int>(std::ceil(radius / 4.0f)), 4, 12);
    const uint16_t ringVertexCount = static_cast<uint16_t>((segments + 1) * 4);
    const uint16_t vertexCount = static_cast<uint16_t>(ringVertexCount + 1);
    const uint16_t indexCount = static_cast<uint16_t>(ringVertexCount * 3);
    if (bgfx::getAvailTransientVertexBuffer(
            vertexCount, rendering::PosColorVertex::ms_decl) < vertexCount ||
        bgfx::getAvailTransientIndexBuffer(indexCount) < indexCount) {
      return;
    }
    bgfx::TransientVertexBuffer tvb{};
    bgfx::TransientIndexBuffer tib{};
    bgfx::allocTransientVertexBuffer(&tvb, vertexCount,
                                     rendering::PosColorVertex::ms_decl);
    bgfx::allocTransientIndexBuffer(&tib, indexCount);
    auto *vertices = reinterpret_cast<rendering::PosColorVertex *>(tvb.data);
    auto *indices = reinterpret_cast<uint16_t *>(tib.data);
    const uint32_t abgr = color.toABGR();
    uint16_t vertexIndex = 0;
    vertices[vertexIndex++] = {
        static_cast<float>(x) + static_cast<float>(width) * 0.5f,
        static_cast<float>(y) + static_cast<float>(height) * 0.5f, 0.0f, abgr};
    const auto appendCorner = [&](float cx, float cy, float startAngle) {
      for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const float angle = startAngle + t * (kPi * 0.5f);
        vertices[vertexIndex++] = {cx + std::cos(angle) * radius,
                                   cy + std::sin(angle) * radius, 0.0f, abgr};
      }
    };
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);
    appendCorner(fx + fw - radius, fy + radius, -kPi * 0.5f);
    appendCorner(fx + fw - radius, fy + fh - radius, 0.0f);
    appendCorner(fx + radius, fy + fh - radius, kPi * 0.5f);
    appendCorner(fx + radius, fy + radius, kPi);
    uint16_t index = 0;
    for (uint16_t i = 0; i < ringVertexCount; ++i) {
      indices[index++] = 0;
      indices[index++] = static_cast<uint16_t>(i + 1);
      indices[index++] = static_cast<uint16_t>((i + 1) % ringVertexCount + 1);
    }
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
  } else {
    bgfx::TransientVertexBuffer tvb{};
    bgfx::TransientIndexBuffer tib{};
    rendering::createRect(tvb, tib, x, y, width, height, color.toABGR());
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
  }
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA |
                 BGFX_STATE_MSAA);
  static const bgfx::ProgramHandle kSimpleProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  bgfx::submit(rendering::ui_view, kSimpleProgram);
}
} // namespace

namespace {
void syncContentFrame(Button &button, View *contentView, bool updateSize) {
  if (contentView == nullptr) {
    return;
  }
  contentView->setPositionNoLayout(button.getX(), button.getY(),
                                   YGPositionTypeAbsolute);
  if (updateSize) {
    contentView->setSize(button.getWidth(), button.getHeight());
  }
}
} // namespace

void Button::renderImpl(RenderContext &context) {
  const bool isPressed = mousePressedInside || activeTouchId != -1;
  Color background = normalBackgroundColor;
  Color border = normalBorderColor;
  if (isPressed) {
    background = pressedBackgroundColor;
    border = pressedBorderColor;
  } else if (isHovered) {
    background = hoverBackgroundColor;
    border = hoverBorderColor;
  }
  if (!enabled) {
    background.a = static_cast<uint8_t>(
        static_cast<unsigned int>(background.a) * 45U / 100U);
    border.a = static_cast<uint8_t>(
        static_cast<unsigned int>(border.a) * 45U / 100U);
  }

  const float radius = getCornerRadius();
  if (hasStyledBorder && styleBorderWidth > 0) {
    drawButtonRect(context, getX(), getY(), getWidth(), getHeight(), radius,
                   border);
  }
  if (hasStyledBackground) {
    const int inset = hasStyledBorder ? styleBorderWidth : 0;
    drawButtonRect(context, getX() + inset, getY() + inset,
                   getWidth() - inset * 2, getHeight() - inset * 2,
                   std::max(0.0f, radius - inset), background);
  }

  ScissorScope scissor(context, getX(), getY(), getWidth(), getHeight());
  if (contentView) {
    contentView->render(context);
  }
}

void Button::setOnClickListener(std::function<void()> listener) {
  this->onClickListener = listener;
}

void Button::setEnabled(bool value) {
  enabled = value;
  if (!enabled) {
    mousePressedInside = false;
    isHovered = false;
    activeTouchId = -1;
  }
}

void Button::setContentView(View *view) {
  if (contentView.get() == view) {
    return;
  }
  contentView.reset(view);
  syncContentFrame(*this, contentView.get(), true);
}

Button *Button::setBackgroundColors(const Color &normal, const Color &hover,
                                    const Color &pressed) {
  normalBackgroundColorProvider = nullptr;
  hoverBackgroundColorProvider = nullptr;
  pressedBackgroundColorProvider = nullptr;
  normalBackgroundColor = normal;
  hoverBackgroundColor = hover;
  pressedBackgroundColor = pressed;
  hasStyledBackground = true;
  return this;
}

Button *Button::setThemedBackgroundColors(ThemeColorProvider normal,
                                          ThemeColorProvider hover,
                                          ThemeColorProvider pressed) {
  normalBackgroundColorProvider = std::move(normal);
  hoverBackgroundColorProvider = std::move(hover);
  pressedBackgroundColorProvider = std::move(pressed);
  if (normalBackgroundColorProvider) {
    normalBackgroundColor = normalBackgroundColorProvider();
  }
  if (hoverBackgroundColorProvider) {
    hoverBackgroundColor = hoverBackgroundColorProvider();
  }
  if (pressedBackgroundColorProvider) {
    pressedBackgroundColor = pressedBackgroundColorProvider();
  }
  hasStyledBackground = true;
  return this;
}

Button *Button::setBorderColors(const Color &normal, const Color &hover,
                                const Color &pressed) {
  normalBorderColorProvider = nullptr;
  hoverBorderColorProvider = nullptr;
  pressedBorderColorProvider = nullptr;
  normalBorderColor = normal;
  hoverBorderColor = hover;
  pressedBorderColor = pressed;
  hasStyledBorder = true;
  return this;
}

Button *Button::setThemedBorderColors(ThemeColorProvider normal,
                                      ThemeColorProvider hover,
                                      ThemeColorProvider pressed) {
  normalBorderColorProvider = std::move(normal);
  hoverBorderColorProvider = std::move(hover);
  pressedBorderColorProvider = std::move(pressed);
  if (normalBorderColorProvider) {
    normalBorderColor = normalBorderColorProvider();
  }
  if (hoverBorderColorProvider) {
    hoverBorderColor = hoverBorderColorProvider();
  }
  if (pressedBorderColorProvider) {
    pressedBorderColor = pressedBorderColorProvider();
  }
  hasStyledBorder = true;
  return this;
}

Button *Button::setStyledBorderWidth(int width) {
  styleBorderWidth = std::max(0, width);
  return this;
}

void Button::onThemeChanged() {
  View::onThemeChanged();
  if (normalBackgroundColorProvider) {
    normalBackgroundColor = normalBackgroundColorProvider();
  }
  if (hoverBackgroundColorProvider) {
    hoverBackgroundColor = hoverBackgroundColorProvider();
  }
  if (pressedBackgroundColorProvider) {
    pressedBackgroundColor = pressedBackgroundColorProvider();
  }
  if (normalBorderColorProvider) {
    normalBorderColor = normalBorderColorProvider();
  }
  if (hoverBorderColorProvider) {
    hoverBorderColor = hoverBorderColorProvider();
  }
  if (pressedBorderColorProvider) {
    pressedBorderColor = pressedBorderColorProvider();
  }
}

void Button::propagateThemeChange() {
  View::propagateThemeChange();
  if (contentView) {
    contentView->propagateThemeChange();
  }
}

Button::~Button() = default;
void Button::onLayout() { syncContentFrame(*this, contentView.get(), true); }

void Button::onMove(int newX, int newY) {
  (void)newX;
  (void)newY;
  syncContentFrame(*this, contentView.get(), false);
}

void Button::onResize(int newWidth, int newHeight) {
  (void)newWidth;
  (void)newHeight;
  syncContentFrame(*this, contentView.get(), true);
}

bool Button::handleEventsImpl(SDL_Event &event) {
  if (!enabled) {
    mousePressedInside = false;
    isHovered = false;
    activeTouchId = -1;
    return true;
  }
  if (contentView) {
    if (!contentView->handleEvents(event)) {
      return false;
    }
  }

  switch (event.type) {
  case SDL_MOUSEBUTTONDOWN: {
    if (event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID) {
      return true;
    }

    int uiX = 0;
    int uiY = 0;
    mouseEventToUi(event.button, uiX, uiY);
    mousePressedInside = isInsideButton(*this, uiX, uiY);
    return !mousePressedInside;
  }
  case SDL_MOUSEBUTTONUP: {
    if (event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID) {
      return true;
    }

    const bool wasPressedInside = mousePressedInside;
    mousePressedInside = false;
    if (!wasPressedInside) {
      return true;
    }

    int uiX = 0;
    int uiY = 0;
    mouseEventToUi(event.button, uiX, uiY);
    if (isInsideButton(*this, uiX, uiY) && onClickListener) {
      onClickListener();
    }
    return false;
  }
  case SDL_MOUSEMOTION: {
    int uiX = 0;
    int uiY = 0;
    mouseCoordsToUi(event.motion.x, event.motion.y, uiX, uiY);
    isHovered = isInsideButton(*this, uiX, uiY);
    break;
  }
  case SDL_FINGERDOWN: {
    if (activeTouchId != -1) {
      return true;
    }

    float uiX = 0.0f;
    float uiY = 0.0f;
    fingerEventToUi(event.tfinger, uiX, uiY);
    if (!isInsideButton(*this, uiX, uiY)) {
      return true;
    }

    activeTouchId = event.tfinger.fingerId;
    return false;
  }
  case SDL_FINGERUP: {
    if (event.tfinger.fingerId != activeTouchId) {
      return true;
    }

    activeTouchId = -1;
    float uiX = 0.0f;
    float uiY = 0.0f;
    fingerEventToUi(event.tfinger, uiX, uiY);
    if (isInsideButton(*this, uiX, uiY) && onClickListener) {
      onClickListener();
    }
    return false;
  }
  case SDL_WINDOWEVENT:
    if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
      isHovered = false;
    }
    break;
  default:
    break;
  }

  return true;
}
