#pragma once

#include "LatencyTelemetry.h"
#include "../video/MetalPresentationObserver.h"
#include <cmath>
#include <limits>

namespace perf::latency {
inline void recordMetalPresentation(video::MetalPresentationEvent event,
                                    double started, double finished) noexcept {
  if (!std::isfinite(started) || !std::isfinite(finished) ||
      started <= 0.0 || finished < started) return;
  const double micros = (finished - started) * 1'000'000.0;
  if (!std::isfinite(micros) ||
      micros >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) return;
  record(event == video::MetalPresentationEvent::Presented
             ? Stage::DrawablePresent : Stage::DrawableWait,
         static_cast<std::uint64_t>(std::ceil(micros)));
}
}
