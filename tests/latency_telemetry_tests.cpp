#define ASOBMASHOW_ENABLE_PERF_TELEMETRY 1
#include "perf/LatencyTelemetry.h"
#include "perf/MetalPresentationTelemetry.h"

#include <iostream>
#include <limits>
#include <thread>

int main() {
  // Use drawable-presented timestamps, not the time its completion callback
  // happens to run. Invalid OS timestamps must not turn into giant samples.
  perf::latency::recordMetalPresentation(
      video::MetalPresentationEvent::Presented, 10.0, 10.016);
  perf::latency::recordMetalPresentation(
      video::MetalPresentationEvent::DrawableWait, 10.0, 10.002);
  for (const auto invalid : {0.0, 9.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
    perf::latency::recordMetalPresentation(
        video::MetalPresentationEvent::Presented, 10.0, invalid);
  }
  const auto presentation = perf::latency::snapshot(perf::latency::Stage::DrawablePresent);
  const auto acquisition = perf::latency::snapshot(perf::latency::Stage::DrawableWait);
  if (presentation.count != 1 || presentation.maximum < 15999 ||
      presentation.maximum > 16001 || acquisition.count != 1 ||
      acquisition.maximum < 1999 || acquisition.maximum > 2001) {
    std::cerr << "Metal timestamps were confused with callback timing or invalid samples\n";
    return 1;
  }
  perf::latency::Histogram histogram;
  std::thread first([&] { for (int i = 0; i < 10000; ++i) histogram.record(101); });
  std::thread second([&] { for (int i = 0; i < 10000; ++i) histogram.record(30000); });
  first.join();
  second.join();
  auto result = histogram.snapshot();
  if (result.count != 20000 || result.p50 != 200 || result.p95 != 30000 ||
      result.p99 != 30000 || result.maximum != 30000) {
    std::cerr << "Concurrent histogram lost a sample or misreported overflow\n";
    return 1;
  }
  histogram.record(std::numeric_limits<std::uint64_t>::max());
  result = histogram.snapshot();
  if (result.count != 20001 || result.maximum != std::numeric_limits<std::uint64_t>::max()) {
    std::cerr << "Histogram overflowed an extreme duration\n";
    return 1;
  }
  return 0;
}
