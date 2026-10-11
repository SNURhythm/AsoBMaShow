#include "ScrollView.h"

#include "../input/SDLPointerEvent.h"
#include "UiTheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {
constexpr float kWheelStepUi = 48.0f;
constexpr float kDragThresholdUi = 12.0f;
constexpr float kScrollbarInset = 4.0f;
constexpr float kScrollbarTrackWidth = 6.0f;
constexpr float kScrollbarMinThumbHeight = 36.0f;
constexpr float kPi = 3.14159265358979323846f;

void appendRoundedRect(RenderContext &context, float x, float y, float width,
                       float height, float radius, std::uint32_t color) {
  if (width <= 0.0f || height <= 0.0f) {
    return;
  }
  radius = std::clamp(radius, 0.0f, std::min(width, height) * 0.5f);
  std::vector<rendering::PosColorVertex> vertices;
  std::vector<std::uint16_t> indices;
  if (radius <= 0.5f) {
    vertices = {{x, y, 0.0f, color},
                {x + width, y, 0.0f, color},
                {x + width, y + height, 0.0f, color},
                {x, y + height, 0.0f, color}};
    indices = {0, 1, 2, 0, 2, 3};
  } else {
    const int segments =
        std::clamp(static_cast<int>(std::ceil(radius / 4.0f)), 4, 12);
    const auto ringVertexCount = static_cast<std::uint16_t>((segments + 1) * 4);
    vertices.reserve(static_cast<std::size_t>(ringVertexCount) + 1U);
    indices.reserve(static_cast<std::size_t>(ringVertexCount) * 3U);
    vertices.push_back({x + width * 0.5f, y + height * 0.5f, 0.0f, color});
    const auto appendCorner = [&](float centerX, float centerY,
                                  float startAngle) {
      for (int index = 0; index <= segments; ++index) {
        const float t = static_cast<float>(index) / static_cast<float>(segments);
        const float angle = startAngle + t * (kPi * 0.5f);
        vertices.push_back({centerX + std::cos(angle) * radius,
                            centerY + std::sin(angle) * radius, 0.0f, color});
      }
    };
    appendCorner(x + width - radius, y + radius, -kPi * 0.5f);
    appendCorner(x + width - radius, y + height - radius, 0.0f);
    appendCorner(x + radius, y + height - radius, kPi * 0.5f);
    appendCorner(x + radius, y + radius, kPi);
    for (std::uint16_t index = 0; index < ringVertexCount; ++index) {
      indices.push_back(0);
      indices.push_back(static_cast<std::uint16_t>(index + 1));
      indices.push_back(
          static_cast<std::uint16_t>((index + 1) % ringVertexCount + 1));
    }
  }
  static const bgfx::ProgramHandle kProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  context.appendUiColor(
      vertices, indices,
      context.makeUiBatchState(kProgram, BGFX_STATE_WRITE_RGB |
                                             BGFX_STATE_WRITE_A |
                                             BGFX_STATE_BLEND_ALPHA |
                                             BGFX_STATE_MSAA));
}

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

void ScrollView::propagateThemeChange() {
  View::propagateThemeChange();
  if (contentView != nullptr) {
    contentView->propagateThemeChange();
  }
}

void ScrollView::propagateLanguageChange() {
  View::propagateLanguageChange();
  if (contentView != nullptr) {
    contentView->propagateLanguageChange();
    refreshContentLayout();
  }
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
    if (touchMomentum.step(SDL_GetTicks(), momentumDelta) && !scrollBy(momentumDelta)) {
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

void ScrollView::onPointerInputCancelled() {
  mousePressedInside = false;
  mouseDragging = false;
  mouseCapturedByContent = false;
  cancelMouseClick = false;
  activeTouchId = -1;
  touchPressedInside = false;
  touchDragging = false;
  touchCapturedByContent = false;
  cancelTouchClick = false;
  touchMomentum.stop();
  if (contentView) contentView->cancelPointerInput();
}

void ScrollView::onPointerEventConsumed(const SDL_Event &event) {
  if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
      event.button.button == SDL_BUTTON_LEFT &&
      event.button.which != SDL_TOUCH_MOUSEID) {
    mousePressedInside = false;
    mouseDragging = false;
    mouseCapturedByContent = false;
    cancelMouseClick = false;
  } else if ((event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) &&
             !sdl_pointer_event::isMouseSynthesizedTouch(event) &&
             event.tfinger.fingerID == activeTouchId) {
    activeTouchId = -1;
    touchPressedInside = false;
    touchDragging = false;
    touchCapturedByContent = false;
    cancelTouchClick = false;
    touchMomentum.stop();
  }
  // Content is owned separately from View's children. Cancel its pointer
  // state without forwarding a release that could activate a covered control.
  if (contentView) contentView->notifyPointerEventConsumed(event);
}

bool ScrollView::handleEventsImpl(SDL_Event &event) {
  if (contentView == nullptr) {
    return true;
  }

  switch (event.type) {
  case SDL_EVENT_MOUSE_WHEEL: {
    float uiX = 0;
    float uiY = 0;
    rendering::screenToUi(event.wheel.mouse_x * rendering::widthScale,
                          event.wheel.mouse_y * rendering::heightScale, uiX, uiY);
    if (!isInside(static_cast<float>(uiX), static_cast<float>(uiY))) {
      return true;
    }
    touchMomentum.stop();
    scrollBy(sdl_pointer_event::verticalWheelScrollDelta(event.wheel,
                                                         kWheelStepUi));
    return false;
  }
  case SDL_EVENT_MOUSE_BUTTON_DOWN: {
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
  case SDL_EVENT_MOUSE_MOTION: {
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
  case SDL_EVENT_MOUSE_BUTTON_UP: {
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
  case SDL_EVENT_FINGER_DOWN: {
    if (activeTouchId != -1) {
      return true;
    }
    float uiX = 0.0f;
    float uiY = 0.0f;
    eventToUi(event.tfinger, uiX, uiY);
    if (!isInside(uiX, uiY)) {
      return true;
    }
    touchMomentum.beginDrag(event.tfinger.timestamp / 1000000);
    activeTouchId = event.tfinger.fingerID;
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
  case SDL_EVENT_FINGER_MOTION: {
    if (event.tfinger.fingerID != activeTouchId) {
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
      touchMomentum.recordDragDelta(delta, event.tfinger.timestamp / 1000000);
      lastTouchUiY = uiY;
      return false;
    }
    return false;
  }
  case SDL_EVENT_FINGER_UP: {
    if (event.tfinger.fingerID != activeTouchId) {
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
      touchMomentum.release(event.tfinger.timestamp / 1000000);
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
                                                0.0, 1.0);
  const float trackX = static_cast<float>(getViewportX() + getViewportWidth()) -
                       kScrollbarInset - kScrollbarTrackWidth;
  const float trackY = static_cast<float>(getViewportY()) + kScrollbarInset;

  appendRoundedRect(context, trackX, trackY, kScrollbarTrackWidth, trackHeight,
                    kScrollbarTrackWidth * 0.5f,
                    ui_theme::withAlpha(ui_theme::hairlineStrong(), 118)
                        .toABGR());
  appendRoundedRect(context, trackX, thumbY, kScrollbarTrackWidth, thumbHeight,
                    kScrollbarTrackWidth * 0.5f,
                    ui_theme::withAlpha(ui_theme::textSecondary(), 218)
                        .toABGR());
}

void ScrollView::clampScrollOffset() {
  if (contentView == nullptr) {
    scrollOffset = 0.0f;
    return;
  }
  const double maxOffset = std::max(
      0.0,
      static_cast<double>(contentView->getHeight() - getScrollContentHeight()));
  scrollOffset = std::clamp(scrollOffset, 0.0, maxOffset);
}

bool ScrollView::scrollBy(float delta) {
  if (contentView == nullptr) {
    return false;
  }
  const double previousOffset = scrollOffset;
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
