#pragma once

#include "../practice/PracticeConfiguration.h"

namespace chart_viewer_listen {

[[nodiscard]] inline long long
resolveStartMicros(const practice::Configuration &configuration,
                   long long lastCursorMicros,
                   practice::Marker activeMarker) noexcept {
  (void)lastCursorMicros;
  (void)activeMarker;
  return configuration.startMicros;
}

} // namespace chart_viewer_listen
