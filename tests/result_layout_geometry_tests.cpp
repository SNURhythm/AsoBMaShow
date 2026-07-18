#include "scene/ResultLayoutGeometry.h"

#include <cassert>
#include <cmath>

namespace {
void testShortMobileMetricsFitControls() {
  const auto metrics = result_layout::metricsFor(885.0f, true);
  assert(metrics.compact);
  assert(metrics.rootPadding == 24.0f);
  assert(metrics.rootGap == 8.0f);
  assert(metrics.summaryHeight == 184.0f);
  assert(metrics.infoHeight == 90.0f);
  assert(metrics.detailsHeight == 96.0f);
  assert(metrics.visualHeight == 236.0f);
  assert(result_layout::screenContentHeight(metrics, true) <= 885.0f);
}

void testDesktopMetricsRemainRegular() {
  const auto metrics = result_layout::metricsFor(1080.0f, false);
  assert(!metrics.compact);
  assert(metrics.rootPadding == 32.0f);
  assert(metrics.rootGap == 12.0f);
  assert(metrics.summaryHeight == 198.0f);
  assert(metrics.infoHeight == 100.0f);
  assert(metrics.detailsHeight == 108.0f);
  assert(metrics.visualHeight == 250.0f);
}

void testPhotoGridOrderAndCanvasFit() {
  using result_layout::PhotoVisual;
  constexpr auto order = result_layout::photoVisualOrder();
  static_assert(order[0] == PhotoVisual::Gauge);
  static_assert(order[1] == PhotoVisual::Histogram);
  static_assert(order[2] == PhotoVisual::Lanes);
  static_assert(order[3] == PhotoVisual::Sections);

  const auto mobile = result_layout::metricsFor(885.0f, true);
  assert(mobile.photoPrimaryHeight == 196.0f);
  assert(mobile.photoSecondaryHeight == 112.0f);
  assert(mobile.photoGridGap == 8.0f);
  assert(result_layout::photoContentHeight(mobile) <= 885.0f);
  assert(result_layout::photoCanvasPixelHeight(2532, 1170, 1920.0f,
                                               mobile) == 1170);

  const auto desktop = result_layout::metricsFor(1080.0f, false);
  assert(result_layout::photoContentHeight(desktop) <= 1080.0f);
  assert(result_layout::photoCanvasPixelHeight(1920, 800, 1920.0f,
                                               mobile) ==
         static_cast<int>(
             std::ceil(result_layout::photoContentHeight(mobile))));
}
} // namespace

int main() {
  testShortMobileMetricsFitControls();
  testDesktopMetricsRemainRegular();
  testPhotoGridOrderAndCanvasFit();
  return 0;
}
