#include "ScrollView.h"

#include "../rendering/SimpleBatchRenderer.h"
#include "UiTheme.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kWheelStepUi = 48.0f;
constexpr float kDragThresholdUi = 12.0f;
constexpr float kScrollbarInset = 4.0f;
constexpr float kScrollbarTrackWidth = 6.0f;
constexpr float kScrollbarMinThumbHeight = 36.0f;

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

ScrollView::~ScrollView() = default;

void ScrollView::setContentView(View *view) {
  if (contentView.get() == view) {
    return;
  }
  contentView.reset(view);
  refreshContentLayout();
}

ScrollView *ScrollView::setContentPadding(Edge edge, float padding) {
  updateStoredContentPadding(edge, padding);
  refreshContentLayout();
  return this;
}

void ScrollView::refreshContentLayout() {
  if (contentView == nullptr) {
    return;
  }
  contentView->setWidth(static_cast<float>(getScrollContentWidth()));
  contentView->applyYogaLayout();
  clampScrollOffset();
  updateContentPosition();
}

void ScrollView::scrollToBottom() {
  if (contentView == nullptr) {
    return;
  }
  refreshContentLayout();
  scrollOffset = std::max(0.0f,
                          static_cast<float>(contentView->getHeight() -
                                             getScrollContentHeight()));
  updateContentPosition();
}

void ScrollView::setScrollOffset(float offset) {
  scrollOffset = std::max(0.0f, offset);
  clampScrollOffset();
  updateContentPosition();
}

void ScrollView::renderImpl(RenderContext &context) {
  if (contentView == nullptr) {
    return;
  }
  if (!touchPressedInside) {
    float momentumDelta = 0.0f;
    if (touchMomentum.step(momentumDelta) && !scrollBy(momentumDelta)) {
      touchMomentum.stop();
    }
  }
  {
    ScissorScope scissor(context, getScrollContentX(), getScrollContentY(),
                         getScrollContentWidth(), getScrollContentHeight());
    contentView->render(context);
  }
  renderPersistentScrollbar(context);
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
    touchMomentum.stop();
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
    touchMomentum.stop();
    mousePressedInside = true;
    mouseDragging = false;
    mouseCapturedByContent = false;
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
    if (mouseCapturedByContent) {
      contentView->handleEvents(event);
      return false;
    }
    bool forwardedToContent = false;
    if (mousePressedInside) {
      forwardedToContent = true;
      if (!contentView->handleEvents(event)) {
        mouseCapturedByContent = true;
        return false;
      }
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
    if (!forwardedToContent) {
      contentView->handleEvents(event);
    }
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
    const bool wasCapturedByContent = mouseCapturedByContent;
    mousePressedInside = false;
    mouseDragging = false;
    mouseCapturedByContent = false;
    cancelMouseClick = false;
    if (!eventToUi(event.button, uiX, uiY)) {
      return false;
    }
    const bool inside =
        isInside(static_cast<float>(uiX), static_cast<float>(uiY));
    if (!hadPress && !inside) {
      return true;
    }
    if (wasCapturedByContent) {
      contentView->handleEvents(event);
      return false;
    }
    if (hadPress) {
      SDL_Event forwarded =
          shouldCancelClick ? makeCancelledMouseUpEvent(event) : event;
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
    touchMomentum.stop();
    activeTouchId = event.tfinger.fingerId;
    touchPressedInside = true;
    touchDragging = false;
    touchCapturedByContent = false;
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
    if (touchCapturedByContent) {
      contentView->handleEvents(event);
      return false;
    }
    if (!contentView->handleEvents(event)) {
      touchCapturedByContent = true;
      return false;
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
      const float delta = lastTouchUiY - uiY;
      scrollBy(delta);
      touchMomentum.recordDragDelta(delta);
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
    const bool hadDrag = touchDragging;
    const bool wasCapturedByContent = touchCapturedByContent;
    activeTouchId = -1;
    touchPressedInside = false;
    touchDragging = false;
    touchCapturedByContent = false;
    cancelTouchClick = false;
    if (wasCapturedByContent) {
      touchMomentum.stop();
      contentView->handleEvents(event);
      return false;
    }
    if (hadDrag) {
      touchMomentum.release();
    } else {
      touchMomentum.stop();
    }
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
  return uiX >= getViewportX() && uiX <= getViewportX() + getViewportWidth() &&
         uiY >= getViewportY() && uiY <= getViewportY() + getViewportHeight();
}

int ScrollView::getViewportX() const { return getContentX(); }

int ScrollView::getViewportY() const { return getContentY(); }

int ScrollView::getViewportWidth() const { return getContentWidth(); }

int ScrollView::getViewportHeight() const { return getContentHeight(); }

int ScrollView::getScrollContentX() const {
  return getViewportX() + contentPaddingLeft;
}

int ScrollView::getScrollContentY() const {
  return getViewportY() + contentPaddingTop;
}

int ScrollView::getScrollContentWidth() const {
  return std::max(0, getViewportWidth() - contentPaddingLeft -
                         contentPaddingRight);
}

int ScrollView::getScrollContentHeight() const {
  return std::max(0, getViewportHeight() - contentPaddingTop -
                         contentPaddingBottom);
}

void ScrollView::updateStoredContentPadding(Edge edge, float padding) {
  const int value =
      std::max(0, static_cast<int>(std::round(std::max(0.0f, padding))));
  switch (edge) {
  case Edge::Left:
  case Edge::Start:
    contentPaddingLeft = value;
    break;
  case Edge::Top:
    contentPaddingTop = value;
    break;
  case Edge::Right:
  case Edge::End:
    contentPaddingRight = value;
    break;
  case Edge::Bottom:
    contentPaddingBottom = value;
    break;
  case Edge::All:
    contentPaddingLeft = value;
    contentPaddingTop = value;
    contentPaddingRight = value;
    contentPaddingBottom = value;
    break;
  }
}

bool ScrollView::hasScrollableOverflow() const {
  return contentView != nullptr &&
         contentView->getHeight() > getScrollContentHeight() + 1;
}

void ScrollView::renderPersistentScrollbar(RenderContext &context) const {
  if (!hasScrollableOverflow() || getViewportWidth() <= 0 ||
      getViewportHeight() <= 0) {
    return;
  }

  const float viewportHeight = static_cast<float>(getScrollContentHeight());
  const float contentHeight =
      std::max(viewportHeight, static_cast<float>(contentView->getHeight()));
  const float maxOffset = std::max(1.0f, contentHeight - viewportHeight);
  const float trackViewportHeight = static_cast<float>(getViewportHeight());
  const float trackHeight =
      std::max(1.0f, trackViewportHeight - kScrollbarInset * 2.0f);
  const float minThumbHeight =
      std::min(kScrollbarMinThumbHeight, trackHeight);
  const float thumbHeight = std::clamp(
      trackHeight * viewportHeight / contentHeight, minThumbHeight,
      trackHeight);
  const float thumbTravel = std::max(0.0f, trackHeight - thumbHeight);
  const float thumbY = static_cast<float>(getViewportY()) + kScrollbarInset +
                       thumbTravel * std::clamp(scrollOffset / maxOffset,
                                                0.0f, 1.0f);
  const float trackX = static_cast<float>(getViewportX() + getViewportWidth()) -
                       kScrollbarInset - kScrollbarTrackWidth;
  const float trackY = static_cast<float>(getViewportY()) + kScrollbarInset;

  rendering::SimpleBatchRenderer batch;
  batch.setSubmitView(rendering::ui_view);
  batch.begin(context.getTransformMatrix());
  batch.addRoundedRect(trackX, trackY, kScrollbarTrackWidth, trackHeight,
                       kScrollbarTrackWidth * 0.5f,
                       ui_theme::withAlpha(ui_theme::hairlineStrong(), 118)
                           .toABGR());
  batch.addRoundedRect(trackX, thumbY, kScrollbarTrackWidth, thumbHeight,
                       kScrollbarTrackWidth * 0.5f,
                       ui_theme::withAlpha(ui_theme::textSecondary(), 218)
                           .toABGR());
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  batch.end();
}

void ScrollView::clampScrollOffset() {
  if (contentView == nullptr) {
    scrollOffset = 0.0f;
    return;
  }
  const float maxOffset = std::max(
      0.0f,
      static_cast<float>(contentView->getHeight() - getScrollContentHeight()));
  scrollOffset = std::clamp(scrollOffset, 0.0f, maxOffset);
}

bool ScrollView::scrollBy(float delta) {
  if (contentView == nullptr) {
    return false;
  }
  const float previousOffset = scrollOffset;
  scrollOffset += delta;
  clampScrollOffset();
  updateContentPosition();
  return std::fabs(scrollOffset - previousOffset) > 0.001f;
}

void ScrollView::updateContentPosition() {
  if (contentView == nullptr) {
    return;
  }
  contentView->setPositionNoLayout(
      getScrollContentX(),
      getScrollContentY() - static_cast<int>(scrollOffset),
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
