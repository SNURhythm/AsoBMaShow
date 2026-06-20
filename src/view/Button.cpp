//
// Created by XF on 8/25/2024.
//

#include "Button.h"

namespace {
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

void fingerEventToUi(const SDL_TouchFingerEvent &event, float &uiX, float &uiY) {
  rendering::normalizedToUi(event.x, event.y, uiX, uiY);
}

void drawButtonRect(const RenderContext &context, int x, int y, int width,
                    int height, const Color &color) {
  if (width <= 0 || height <= 0 || color.a == 0) {
    return;
  }
  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};
  rendering::createRect(tvb, tib, x, y, width, height, color.toABGR());
  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
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

  if (hasStyledBorder && styleBorderWidth > 0) {
    drawButtonRect(context, getX(), getY(), getWidth(), getHeight(), border);
  }
  if (hasStyledBackground) {
    const int inset = hasStyledBorder ? styleBorderWidth : 0;
    drawButtonRect(context, getX() + inset, getY() + inset,
                   getWidth() - inset * 2, getHeight() - inset * 2,
                   background);
  }

  ScissorScope scissor(context, getX(), getY(), getWidth(), getHeight());
  if (contentView) {
    contentView->render(context);
  }
}

void Button::setOnClickListener(std::function<void()> listener) {
  this->onClickListener = listener;
}

void Button::setContentView(View *view) {
  if (contentView == view) {
    return;
  }
  delete contentView;
  this->contentView = view;
  syncContentFrame(*this, contentView, true);
}

Button *Button::setBackgroundColors(const Color &normal, const Color &hover,
                                    const Color &pressed) {
  normalBackgroundColor = normal;
  hoverBackgroundColor = hover;
  pressedBackgroundColor = pressed;
  hasStyledBackground = true;
  return this;
}

Button *Button::setBorderColors(const Color &normal, const Color &hover,
                                const Color &pressed) {
  normalBorderColor = normal;
  hoverBorderColor = hover;
  pressedBorderColor = pressed;
  hasStyledBorder = true;
  return this;
}

Button *Button::setStyledBorderWidth(int width) {
  styleBorderWidth = std::max(0, width);
  return this;
}

Button::~Button() { delete contentView; }
void Button::onLayout() {
  syncContentFrame(*this, contentView, true);
}

void Button::onMove(int newX, int newY) {
  (void)newX;
  (void)newY;
  syncContentFrame(*this, contentView, false);
}

void Button::onResize(int newWidth, int newHeight) {
  (void)newWidth;
  (void)newHeight;
  syncContentFrame(*this, contentView, true);
}

bool Button::handleEventsImpl(SDL_Event &event) {
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
