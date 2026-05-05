#include "ScrollView.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kWheelStepUi = 48.0f;
constexpr float kDragThresholdUi = 12.0f;

SDL_Event makeCancelledMouseUpEvent(const SDL_Event &event) {
  SDL_Event cancelled = event;
  cancelled.button.x = -1;
  cancelled.button.y = -1;
  return cancelled;
}

SDL_Event makeCancelledTouchUpEvent(const SDL_Event &event) {
  SDL_Event cancelled = event;
  cancelled.tfinger.x = -1.0f;
  cancelled.tfinger.y = -1.0f;
  return cancelled;
}
} // namespace

ScrollView::~ScrollView() { delete contentView; }

void ScrollView::setContentView(View *view) {
  if (contentView == view) {
    return;
  }
  delete contentView;
  contentView = view;
  refreshContentLayout();
}

void ScrollView::refreshContentLayout() {
  if (contentView == nullptr) {
    return;
  }
  contentView->setWidth(static_cast<float>(getWidth()));
  contentView->applyYogaLayout();
  clampScrollOffset();
  updateContentPosition();
}

void ScrollView::renderImpl(RenderContext &context) {
  if (contentView == nullptr) {
    return;
  }
  ScissorScope scissor(context, getX(), getY(), getWidth(), getHeight());
  contentView->render(context);
}

bool ScrollView::handleEventsImpl(SDL_Event &event) {
  if (contentView == nullptr) {
    return true;
  }

  switch (event.type) {
  case SDL_MOUSEWHEEL: {
    int mouseX = 0;
    int mouseY = 0;
    SDL_GetMouseState(&mouseX, &mouseY);
    mouseX = static_cast<int>(mouseX * rendering::widthScale);
    mouseY = static_cast<int>(mouseY * rendering::heightScale);
    int uiX = 0;
    int uiY = 0;
    rendering::screenToUi(mouseX, mouseY, uiX, uiY);
    if (!isInside(static_cast<float>(uiX), static_cast<float>(uiY))) {
      return true;
    }
    scrollBy(-event.wheel.y * kWheelStepUi);
    return false;
  }
  case SDL_MOUSEBUTTONDOWN: {
    if (event.button.button != SDL_BUTTON_LEFT || mousePressedInside) {
      return true;
    }
    if (event.button.which == SDL_TOUCH_MOUSEID) {
      break;
    }
    int uiX = 0;
    int uiY = 0;
    if (!eventToUi(event.button, uiX, uiY) ||
        !isInside(static_cast<float>(uiX), static_cast<float>(uiY))) {
      return true;
    }
    mousePressedInside = true;
    mouseDragging = false;
    cancelMouseClick = false;
    pressedMouseUiX = uiX;
    pressedMouseUiY = uiY;
    lastMouseUiY = uiY;
    contentView->handleEvents(event);
    return false;
  }
  case SDL_MOUSEMOTION: {
    int uiX = 0;
    int uiY = 0;
    if (!eventToUi(event.motion, uiX, uiY)) {
      return true;
    }
    if (mousePressedInside && !mouseDragging &&
        (std::abs(uiX - pressedMouseUiX) >= kDragThresholdUi ||
         std::abs(uiY - pressedMouseUiY) >= kDragThresholdUi)) {
      mouseDragging = true;
      cancelMouseClick = true;
    }
    if (mouseDragging) {
      scrollBy(static_cast<float>(lastMouseUiY - uiY));
      lastMouseUiY = uiY;
      return false;
    }
    contentView->handleEvents(event);
    return isInside(static_cast<float>(uiX), static_cast<float>(uiY)) ? false
                                                                      : true;
  }
  case SDL_MOUSEBUTTONUP: {
    if (event.button.button != SDL_BUTTON_LEFT) {
      return true;
    }
    if (event.button.which == SDL_TOUCH_MOUSEID) {
      break;
    }
    int uiX = 0;
    int uiY = 0;
    const bool hadPress = mousePressedInside;
    const bool shouldCancelClick = cancelMouseClick;
    mousePressedInside = false;
    mouseDragging = false;
    cancelMouseClick = false;
    if (!eventToUi(event.button, uiX, uiY)) {
      return false;
    }
    const bool inside = isInside(static_cast<float>(uiX), static_cast<float>(uiY));
    if (!hadPress && !inside) {
      return true;
    }
    if (hadPress) {
      SDL_Event forwarded = shouldCancelClick ? makeCancelledMouseUpEvent(event)
                                              : event;
      contentView->handleEvents(forwarded);
      return false;
    }
    return inside ? false : true;
  }
  case SDL_FINGERDOWN: {
    if (activeTouchId != -1) {
      return true;
    }
    float uiX = 0.0f;
    float uiY = 0.0f;
    eventToUi(event.tfinger, uiX, uiY);
    if (!isInside(uiX, uiY)) {
      return true;
    }
    activeTouchId = event.tfinger.fingerId;
    touchPressedInside = true;
    touchDragging = false;
    cancelTouchClick = false;
    pressedTouchUiX = uiX;
    pressedTouchUiY = uiY;
    lastTouchUiY = uiY;
    contentView->handleEvents(event);
    return false;
  }
  case SDL_FINGERMOTION: {
    if (event.tfinger.fingerId != activeTouchId) {
      return true;
    }
    float uiX = 0.0f;
    float uiY = 0.0f;
    eventToUi(event.tfinger, uiX, uiY);
    if (touchPressedInside && !touchDragging &&
        (std::fabs(uiX - pressedTouchUiX) >= kDragThresholdUi ||
         std::fabs(uiY - pressedTouchUiY) >= kDragThresholdUi)) {
      touchDragging = true;
      cancelTouchClick = true;
    }
    if (touchDragging) {
      scrollBy(lastTouchUiY - uiY);
      lastTouchUiY = uiY;
      return false;
    }
    return false;
  }
  case SDL_FINGERUP: {
    if (event.tfinger.fingerId != activeTouchId) {
      return true;
    }
    const bool shouldCancelClick = cancelTouchClick;
    activeTouchId = -1;
    touchPressedInside = false;
    touchDragging = false;
    cancelTouchClick = false;
    SDL_Event forwarded =
        shouldCancelClick ? makeCancelledTouchUpEvent(event) : event;
    contentView->handleEvents(forwarded);
    return false;
  }
  default:
    break;
  }

  return contentView->handleEvents(event);
}

void ScrollView::onLayout() { refreshContentLayout(); }

void ScrollView::onMove(int newX, int newY) {
  (void)newX;
  (void)newY;
  updateContentPosition();
}

void ScrollView::onResize(int newWidth, int newHeight) {
  (void)newWidth;
  (void)newHeight;
  refreshContentLayout();
}

bool ScrollView::isInside(float uiX, float uiY) const {
  return uiX >= getX() && uiX <= getX() + getWidth() && uiY >= getY() &&
         uiY <= getY() + getHeight();
}

void ScrollView::clampScrollOffset() {
  if (contentView == nullptr) {
    scrollOffset = 0.0f;
    return;
  }
  const float maxOffset =
      std::max(0.0f, static_cast<float>(contentView->getHeight() - getHeight()));
  scrollOffset = std::clamp(scrollOffset, 0.0f, maxOffset);
}

void ScrollView::scrollBy(float delta) {
  if (contentView == nullptr) {
    return;
  }
  scrollOffset += delta;
  clampScrollOffset();
  updateContentPosition();
}

void ScrollView::updateContentPosition() {
  if (contentView == nullptr) {
    return;
  }
  contentView->setPositionNoLayout(getX(), getY() - static_cast<int>(scrollOffset),
                                   YGPositionTypeAbsolute);
}

bool ScrollView::eventToUi(const SDL_MouseButtonEvent &event, int &uiX,
                           int &uiY) const {
  const int screenX = static_cast<int>(event.x * rendering::widthScale);
  const int screenY = static_cast<int>(event.y * rendering::heightScale);
  rendering::screenToUi(screenX, screenY, uiX, uiY);
  return true;
}

bool ScrollView::eventToUi(const SDL_MouseMotionEvent &event, int &uiX,
                           int &uiY) const {
  const int screenX = static_cast<int>(event.x * rendering::widthScale);
  const int screenY = static_cast<int>(event.y * rendering::heightScale);
  rendering::screenToUi(screenX, screenY, uiX, uiY);
  return true;
}

void ScrollView::eventToUi(const SDL_TouchFingerEvent &event, float &uiX,
                           float &uiY) const {
  rendering::normalizedToUi(event.x, event.y, uiX, uiY);
}
