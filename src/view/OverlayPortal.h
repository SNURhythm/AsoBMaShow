#pragma once

#include "View.h"

#include <algorithm>
#include <vector>

struct OverlayAnchor {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct NormalizedOverlayAnchor {
  float x = 0.5f;
  float y = 0.5f;
  float width = 0.0f;
  float height = 0.0f;
};

[[nodiscard]] inline NormalizedOverlayAnchor
normalizeOverlayAnchor(const OverlayAnchor &anchor, int viewportWidth,
                       int viewportHeight) {
  if (viewportWidth <= 0 || viewportHeight <= 0) {
    return {};
  }
  const float inverseWidth = 1.0f / static_cast<float>(viewportWidth);
  const float inverseHeight = 1.0f / static_cast<float>(viewportHeight);
  const float left =
      std::clamp(static_cast<float>(anchor.x) * inverseWidth, 0.0f, 1.0f);
  const float top =
      std::clamp(static_cast<float>(anchor.y) * inverseHeight, 0.0f, 1.0f);
  const float right = std::clamp(
      (static_cast<float>(anchor.x) + std::max(0, anchor.width)) *
          inverseWidth,
      0.0f, 1.0f);
  const float bottom = std::clamp(
      (static_cast<float>(anchor.y) + std::max(0, anchor.height)) *
          inverseHeight,
      0.0f, 1.0f);
  return {.x = left,
          .y = top,
          .width = std::max(0.0f, right - left),
          .height = std::max(0.0f, bottom - top)};
}

struct OverlayPlacement {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool opensBelow = true;
};

[[nodiscard]] inline OverlayPlacement
placeAnchoredOverlay(const OverlayAnchor &anchor, int desiredWidth,
                     int desiredHeight, int minimumHeight, int viewportWidth,
                     int viewportHeight, int margin, int gap) {
  const int safeMargin = std::max(0, margin);
  const int safeGap = std::max(0, gap);
  const int availableWidth = std::max(0, viewportWidth - safeMargin * 2);
  const int requestedWidth =
      desiredWidth > 0 ? desiredWidth : std::max(0, anchor.width);
  const int width = std::min(requestedWidth, availableWidth);
  const int maximumX = std::max(safeMargin, viewportWidth - safeMargin - width);
  const int x = std::clamp(anchor.x, safeMargin, maximumX);

  const int belowSpace = std::max(0, viewportHeight - safeMargin - safeGap -
                                         anchor.y - anchor.height);
  const int aboveSpace = std::max(0, anchor.y - safeMargin - safeGap);
  const int requestedHeight = std::max(0, desiredHeight);
  const int preferredMinimum = std::max(0, minimumHeight);
  const bool belowFits = belowSpace >= requestedHeight;
  const bool aboveFits = aboveSpace >= requestedHeight;
  const bool belowUsable = belowSpace >= preferredMinimum;
  const bool aboveUsable = aboveSpace >= preferredMinimum;

  bool opensBelow = false;
  if (belowFits) {
    opensBelow = true;
  } else if (aboveFits) {
    opensBelow = false;
  } else if (belowUsable != aboveUsable) {
    opensBelow = belowUsable;
  } else {
    opensBelow = belowSpace >= aboveSpace;
  }

  const int height =
      std::min(requestedHeight, opensBelow ? belowSpace : aboveSpace);
  const int y = opensBelow ? anchor.y + anchor.height + safeGap
                           : anchor.y - safeGap - height;
  return {.x = x,
          .y = y,
          .width = width,
          .height = height,
          .opensBelow = opensBelow};
}

// Presents caller-owned views at the current scene's root overlay layer.
// Presented views must outlive their registration with the portal.
class OverlayPortal : public View {
public:
  using View::View;

  void present(View *overlay) {
    if (overlay == nullptr || isPresented(overlay)) {
      return;
    }
    presented.push_back(overlay);
  }

  void dismiss(View *overlay) { std::erase(presented, overlay); }

  [[nodiscard]] bool isPresented(const View *overlay) const {
    return std::ranges::find(presented, overlay) != presented.end();
  }

  void propagateThemeChange() override {
    View::propagateThemeChange();
    for (auto *overlay : presented) {
      if (overlay != nullptr) {
        overlay->propagateThemeChange();
      }
    }
  }

protected:
  void renderImpl(RenderContext &context) override {
    for (auto *overlay : presented) {
      if (overlay != nullptr) {
        overlay->render(context);
      }
    }
  }

  bool handleEventsImpl(SDL_Event &event) override {
    for (auto it = presented.rbegin(); it != presented.rend(); ++it) {
      if (*it != nullptr && !(*it)->handleEvents(event)) {
        return false;
      }
    }
    return true;
  }

private:
  std::vector<View *> presented;
};
