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

void fingerEventToUi(const SDL_TouchFingerEvent &event, float &uiX, float &uiY) {
  rendering::normalizedToUi(event.x, event.y, uiX, uiY);
}
} // namespace

void Button::renderImpl(RenderContext &context) {
  ScissorScope scissor(context, getX(), getY(), getWidth(), getHeight());
  if (contentView) {
    contentView->render(context);
  }
}

void Button::setOnClickListener(std::function<void()> listener) {
  this->onClickListener = listener;
}

void Button::setContentView(View *view) {
  this->contentView = view;
  view->setPosition(getX(), getY());
  view->setSize(getWidth(), getHeight());
}
Button::~Button() { delete contentView; }
void Button::onLayout() {
  if (contentView) {
    contentView->setPosition(getX(), getY());
    contentView->setSize(getWidth(), getHeight());
  }
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
  default:
    break;
  }

  return true;
}
