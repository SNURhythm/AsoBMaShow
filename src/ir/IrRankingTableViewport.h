#pragma once

#include "../input/SDLPointerEvent.h"
#include "../platform/SDLMainThread.h"
#include "../view/UiTheme.h"
#include "../view/View.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace ir {

// Keep the ranking header and virtualized rows on the same horizontal offset.
// Vertical drags and taps continue to be handled by the table's RecyclerView.
class RankingTableViewport final : public View {
public:
  void setContentView(View *view) {
    content_.reset(view);
    refreshContentLayout();
  }

  void resetScroll() {
    offset_ = 0;
    refreshContentLayout();
  }

  void propagateThemeChange() override {
    View::propagateThemeChange();
    if (content_) content_->propagateThemeChange();
  }

  void propagateLanguageChange() override {
    View::propagateLanguageChange();
    if (content_) content_->propagateLanguageChange();
  }

protected:
  void onLayout() override { refreshContentLayout(); }
  void onMove(int, int) override { refreshContentLayout(); }
  void onResize(int, int) override { refreshContentLayout(); }

  void renderImpl(RenderContext &context) override {
    if (!content_) return;
    ScissorScope clip(context, getX(), getY(), getWidth(), getHeight());
    content_->render(context);
    if (content_->getWidth() > getWidth() && getWidth() > 0) {
      const float ratio = float(getWidth()) / content_->getWidth();
      scrollbar_.setBackgroundColor(ui_theme::textMuted());
      scrollbar_.setCornerRadius(2);
      scrollbar_.setSize(getWidth() * ratio, 4);
      scrollbar_.setPositionNoLayout(getX() + offset_ * ratio,
                                     getY() + getHeight() - 5, YGPositionTypeAbsolute);
      scrollbar_.render(context);
    }
  }

  void onPointerInputCancelled() override {
    pointerActive_ = false;
    touchId_ = -1;
    axis_ = Axis::Undecided;
    if (content_) content_->cancelPointerInput();
  }

  void onPointerEventConsumed(const SDL_Event &event) override {
    const bool touchEnd = (event.type == SDL_EVENT_FINGER_UP ||
                           event.type == SDL_EVENT_FINGER_CANCELED) &&
                          !sdl_pointer_event::isMouseSynthesizedTouch(event) &&
                          touchId_ == event.tfinger.fingerID;
    const bool mouseEnd = event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                          event.button.button == SDL_BUTTON_LEFT &&
                          event.button.which != SDL_TOUCH_MOUSEID && touchId_ == -1;
    if (pointerActive_ && (touchEnd || mouseEnd)) {
      pointerActive_ = false;
      touchId_ = -1;
      axis_ = Axis::Undecided;
    }
    if (content_) {
      const auto forwarded = asTouchEvent(event);
      content_->notifyPointerEventConsumed(forwarded);
    }
  }

  bool handleEventsImpl(SDL_Event &event) override {
    if (!content_ || sdl_pointer_event::isMouseSynthesizedTouch(event)) return true;
    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
      const float mouseX = event.wheel.mouse_x;
      const float mouseY = event.wheel.mouse_y;
      float x, y;
      rendering::screenToUiNormalized(mouseX * rendering::widthScale,
                                      mouseY * rendering::heightScale, x, y);
      if (!inside(x * rendering::window_width, y * rendering::window_height)) return true;
      const float dx = event.wheel.x;
      const float dy = event.wheel.y;
      if (std::abs(dx) > std::abs(dy) || (platform::sdlMain<SDL_GetModState>() & SDL_KMOD_SHIFT)) {
        scrollBy(-((platform::sdlMain<SDL_GetModState>() & SDL_KMOD_SHIFT) ? dy : dx) * 32);
        return false;
      }
      return content_->handleEvents(event);
    }
    const bool touch = event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION ||
                       event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED;
    const bool mouse = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_MOTION ||
                       event.type == SDL_EVENT_MOUSE_BUTTON_UP;
    if (!touch && !mouse) return content_->handleEvents(event);
    if (mouse && ((event.type == SDL_EVENT_MOUSE_MOTION && event.motion.which == SDL_TOUCH_MOUSEID) ||
        (event.type != SDL_EVENT_MOUSE_MOTION &&
         (event.button.which == SDL_TOUCH_MOUSEID || event.button.button != SDL_BUTTON_LEFT)))) return true;
    float x, y;
    if (touch) {
      rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, x, y);
    } else {
      const float px = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.x : event.button.x;
      const float py = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.y : event.button.y;
      rendering::screenToUiNormalized(px * rendering::widthScale, py * rendering::heightScale, x, y);
      x *= rendering::window_width;
      y *= rendering::window_height;
    }
    const bool down = event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    const bool up = event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_MOUSE_BUTTON_UP;
    const bool cancelled = event.type == SDL_EVENT_FINGER_CANCELED;
    if (down) {
      if (pointerActive_ || !inside(x, y)) return true;
      pointerActive_ = true;
      touchId_ = touch ? event.tfinger.fingerID : -1;
      axis_ = Axis::Undecided;
      startX_ = lastX_ = x;
      startY_ = y;
      forwardPointer(event);
      return false;
    }
    if (!pointerActive_ || (touch && touchId_ != event.tfinger.fingerID) ||
        (!touch && touchId_ != -1)) return true;
    if (up || cancelled) {
      pointerActive_ = false;
      touchId_ = -1;
      if (cancelled) content_->notifyPointerEventConsumed(event);
      else if (axis_ != Axis::Horizontal) forwardPointer(event);
      axis_ = Axis::Undecided;
      return false;
    }
    if (axis_ == Axis::Undecided &&
        std::max(std::abs(x - startX_), std::abs(y - startY_)) >= 6) {
      axis_ = std::abs(x - startX_) > std::abs(y - startY_) ? Axis::Horizontal : Axis::Vertical;
      if (axis_ == Axis::Horizontal) {
        auto cancel = asTouchEvent(event);
        cancel.type = SDL_EVENT_FINGER_UP;
        content_->notifyPointerEventConsumed(cancel);
      }
    }
    if (axis_ == Axis::Horizontal) scrollBy(lastX_ - x);
    else forwardPointer(event);
    lastX_ = x;
    return false;
  }

private:
  enum class Axis { Undecided, Horizontal, Vertical };
  std::unique_ptr<View> content_;
  View scrollbar_;
  float offset_ = 0;
  float startX_ = 0, startY_ = 0, lastX_ = 0;
  SDL_FingerID touchId_ = -1;
  bool pointerActive_ = false;
  Axis axis_ = Axis::Undecided;

  SDL_Event asTouchEvent(const SDL_Event &event) const {
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN && event.type != SDL_EVENT_MOUSE_BUTTON_UP &&
        event.type != SDL_EVENT_MOUSE_MOTION) return event;
    SDL_Event converted{};
    converted.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? SDL_EVENT_FINGER_DOWN
        : event.type == SDL_EVENT_MOUSE_BUTTON_UP ? SDL_EVENT_FINGER_UP : SDL_EVENT_FINGER_MOTION;
    converted.tfinger.touchID = 0;
    converted.tfinger.fingerID = 0;
    const float x = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.x : event.button.x;
    const float y = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.y : event.button.y;
    converted.tfinger.x = x * rendering::widthScale / std::max(1, rendering::render_width);
    converted.tfinger.y = y * rendering::heightScale / std::max(1, rendering::render_height);
    return converted;
  }

  void forwardPointer(const SDL_Event &event) {
    // Defer mouse selection until release just like touch, so a horizontal
    // drag cannot open a ranking entry before its direction is known.
    auto forwarded = asTouchEvent(event);
    content_->handleEvents(forwarded);
  }

  bool inside(float x, float y) const {
    return x >= getX() && x <= getX() + getWidth() &&
           y >= getY() && y <= getY() + getHeight();
  }

  void scrollBy(float delta) {
    offset_ += delta;
    refreshContentLayout();
  }

  void refreshContentLayout() {
    if (!content_) return;
    const float width = std::max(900, getWidth());
    offset_ = std::clamp(offset_, 0.0F, std::max(0.0F, width - getWidth()));
    content_->setSize(width, std::max(0, getHeight() - (width > getWidth() ? 8 : 0)));
    content_->setPositionNoLayout(static_cast<int>(std::lround(getX() - offset_)),
                                  getY(), YGPositionTypeAbsolute);
  }
};

} // namespace ir
