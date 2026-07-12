#include "SnappedSlider.h"

#include "UiTheme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr float kHorizontalInset = 12.0F;
constexpr float kTrackHeight = 6.0F;
constexpr float kThumbRadius = 10.0F;

void mouseToUi(int rawX, int rawY, float &x, float &y) {
  rendering::screenToUi(rawX * rendering::widthScale,
                        rawY * rendering::heightScale, x, y);
}
} // namespace

SnappedSlider::SnappedSlider(std::function<void(int)> onValueChanged)
    : onValueChanged(std::move(onValueChanged)) {
  batch.setSubmitView(rendering::ui_view);
  setHeight(44);
  setMinWidth(120);
}

void SnappedSlider::refresh(const State &state) {
  current.minimum = std::min(state.minimum, state.maximum);
  current.maximum = std::max(state.minimum, state.maximum);
  current.step = std::max(1, state.step);
  current.enabled = state.enabled;
  const int clamped =
      std::clamp(state.value, current.minimum, current.maximum);
  const int offset = clamped - current.minimum;
  current.value = std::clamp(
      current.minimum +
          static_cast<int>(std::lround(static_cast<double>(offset) /
                                       current.step)) *
              current.step,
      current.minimum, current.maximum);
  if (!current.enabled) {
    cancelInteraction();
  }
}

void SnappedSlider::renderImpl(RenderContext &context) {
  const float width = static_cast<float>(getWidth());
  const float height = static_cast<float>(getHeight());
  const float trackWidth = std::max(0.0F, width - kHorizontalInset * 2.0F);
  if (trackWidth <= 0.0F || height <= 0.0F) {
    return;
  }

  const float fraction = current.maximum == current.minimum
                             ? 0.0F
                             : static_cast<float>(current.value -
                                                  current.minimum) /
                                   static_cast<float>(current.maximum -
                                                      current.minimum);
  const float trackX = static_cast<float>(getX()) + kHorizontalInset;
  const float trackY = static_cast<float>(getY()) +
                       (height - kTrackHeight) * 0.5F;
  const float thumbX = trackX + trackWidth * fraction;
  const float thumbY = static_cast<float>(getY()) + height * 0.5F;

  Color trackColor = ui_theme::control();
  Color fillColor = ui_theme::cyan();
  Color thumbColor = hovered || mouseDragging || activeTouchId != -1
                         ? ui_theme::textPrimary()
                         : ui_theme::cyan();
  if (!current.enabled) {
    trackColor = ui_theme::withAlpha(trackColor, 110);
    fillColor = ui_theme::withAlpha(fillColor, 90);
    thumbColor = ui_theme::withAlpha(thumbColor, 110);
  }

  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  batch.begin();
  batch.addRoundedRect(trackX, trackY, trackWidth, kTrackHeight,
                       kTrackHeight * 0.5F, trackColor.toABGR());
  const float fillWidth = std::clamp(trackWidth * fraction, 0.0F, trackWidth);
  if (fillWidth > 0.0F) {
    batch.addRoundedRect(trackX, trackY, fillWidth, kTrackHeight,
                         kTrackHeight * 0.5F, fillColor.toABGR());
  }
  batch.addCircle(thumbX, thumbY, kThumbRadius, thumbColor.toABGR());
  batch.end();
}

bool SnappedSlider::handleEventsImpl(SDL_Event &event) {
  if (!current.enabled) {
    cancelInteraction();
    return true;
  }

  float x = 0.0F;
  float y = 0.0F;
  switch (event.type) {
  case SDL_MOUSEBUTTONDOWN:
    if (event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID) {
      return true;
    }
    mouseToUi(event.button.x, event.button.y, x, y);
    if (!contains(x, y)) {
      return true;
    }
    mouseDragging = true;
    updateFromX(x);
    return false;
  case SDL_MOUSEMOTION:
    mouseToUi(event.motion.x, event.motion.y, x, y);
    hovered = contains(x, y);
    if (!mouseDragging) {
      return true;
    }
    updateFromX(x);
    return false;
  case SDL_MOUSEBUTTONUP:
    if (event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID || !mouseDragging) {
      return true;
    }
    mouseToUi(event.button.x, event.button.y, x, y);
    updateFromX(x);
    mouseDragging = false;
    return false;
  case SDL_FINGERDOWN:
    if (activeTouchId != -1) {
      return true;
    }
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, x, y);
    if (!contains(x, y)) {
      return true;
    }
    activeTouchId = event.tfinger.fingerId;
    updateFromX(x);
    return false;
  case SDL_FINGERMOTION:
    if (event.tfinger.fingerId != activeTouchId) {
      return true;
    }
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, x, y);
    updateFromX(x);
    return false;
  case SDL_FINGERUP:
    if (event.tfinger.fingerId != activeTouchId) {
      return true;
    }
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, x, y);
    updateFromX(x);
    activeTouchId = -1;
    return false;
  case SDL_WINDOWEVENT:
    if (event.window.event == SDL_WINDOWEVENT_LEAVE ||
        event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
      cancelInteraction();
    }
    return true;
  default:
    return true;
  }
}

bool SnappedSlider::contains(float x, float y) const {
  return x >= static_cast<float>(getX()) &&
         x <= static_cast<float>(getX() + getWidth()) &&
         y >= static_cast<float>(getY()) &&
         y <= static_cast<float>(getY() + getHeight());
}

int SnappedSlider::snappedValueForX(float x) const {
  const float trackWidth =
      std::max(1.0F, static_cast<float>(getWidth()) - kHorizontalInset * 2.0F);
  const float fraction = std::clamp(
      (x - static_cast<float>(getX()) - kHorizontalInset) / trackWidth, 0.0F,
      1.0F);
  const double raw = static_cast<double>(current.minimum) +
                     fraction * (current.maximum - current.minimum);
  const int stepIndex = static_cast<int>(std::lround(
      (raw - static_cast<double>(current.minimum)) / current.step));
  return std::clamp(current.minimum + stepIndex * current.step,
                    current.minimum, current.maximum);
}

void SnappedSlider::updateFromX(float x) {
  const int next = snappedValueForX(x);
  if (next == current.value) {
    return;
  }
  current.value = next;
  if (onValueChanged) {
    onValueChanged(next);
  }
}

void SnappedSlider::cancelInteraction() {
  mouseDragging = false;
  hovered = false;
  activeTouchId = -1;
}
