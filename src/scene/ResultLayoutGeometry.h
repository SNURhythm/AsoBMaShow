#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace result_layout {

inline constexpr float kShortMobileMaximumHeight = 920.0f;
inline constexpr float kHeaderHeight = 96.0f;
inline constexpr float kActionHeight = 64.0f;
inline constexpr float kLegacyGraphHeight = 136.0f;

enum class PhotoVisual { Gauge, Histogram, Lanes, Sections };

struct Metrics {
  bool compact = false;
  float rootPadding = 32.0f;
  float rootGap = 12.0f;
  float summaryHeight = 198.0f;
  float summaryPanelPadding = 14.0f;
  float gradePanelPadding = 12.0f;
  float infoHeight = 100.0f;
  float infoTilePadding = 7.0f;
  float detailsHeight = 108.0f;
  float detailsTilePadding = 8.0f;
  float visualHeight = 250.0f;
  float visualMinimumHeight = 236.0f;
  float visualGap = 12.0f;
  float graphFlex = 2.0f;
  float analyticsFlex = 3.0f;
  float photoPrimaryHeight = 206.0f;
  float photoSecondaryHeight = 120.0f;
  float photoGridGap = 12.0f;
};

[[nodiscard]] constexpr Metrics metricsFor(float viewportHeight,
                                           bool mobileTarget) noexcept {
  Metrics result;
  result.compact =
      mobileTarget && viewportHeight <= kShortMobileMaximumHeight;
  if (!result.compact) {
    return result;
  }

  result.rootPadding = 24.0f;
  result.rootGap = 8.0f;
  result.summaryHeight = 184.0f;
  result.summaryPanelPadding = 10.0f;
  result.gradePanelPadding = 8.0f;
  result.infoHeight = 90.0f;
  result.infoTilePadding = 4.0f;
  result.detailsHeight = 96.0f;
  result.detailsTilePadding = 4.0f;
  result.visualHeight = 236.0f;
  result.visualMinimumHeight = 236.0f;
  result.visualGap = 8.0f;
  result.photoPrimaryHeight = 196.0f;
  result.photoSecondaryHeight = 112.0f;
  result.photoGridGap = 8.0f;
  return result;
}

[[nodiscard]] constexpr float
screenContentHeight(const Metrics &metrics, bool hasAnalytics) noexcept {
  const float visualHeight =
      hasAnalytics ? metrics.visualHeight : kLegacyGraphHeight;
  return kHeaderHeight + metrics.summaryHeight + metrics.infoHeight +
         metrics.detailsHeight + visualHeight + kActionHeight +
         metrics.rootPadding * 2.0f + metrics.rootGap * 5.0f;
}

[[nodiscard]] constexpr float
photoContentHeight(const Metrics &metrics) noexcept {
  const float visualHeight = metrics.photoPrimaryHeight +
                             metrics.photoSecondaryHeight +
                             metrics.photoGridGap;
  return kHeaderHeight + metrics.summaryHeight + metrics.infoHeight +
         metrics.detailsHeight + visualHeight + metrics.rootPadding * 2.0f +
         metrics.rootGap * 4.0f;
}

[[nodiscard]] inline int
photoCanvasPixelHeight(int drawableWidth, int drawableHeight,
                       float logicalWidth, const Metrics &metrics) noexcept {
  if (drawableWidth <= 0 || drawableHeight <= 0 || logicalWidth <= 0.0f) {
    return drawableHeight;
  }
  const float scale = static_cast<float>(drawableWidth) / logicalWidth;
  const int requiredHeight =
      static_cast<int>(std::ceil(photoContentHeight(metrics) * scale));
  return std::max(drawableHeight, requiredHeight);
}

[[nodiscard]] constexpr std::array<PhotoVisual, 4>
photoVisualOrder() noexcept {
  return {PhotoVisual::Gauge, PhotoVisual::Histogram, PhotoVisual::Lanes,
          PhotoVisual::Sections};
}

} // namespace result_layout
