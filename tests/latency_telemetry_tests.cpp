#define ASOBMASHOW_ENABLE_PERF_TELEMETRY 1
#include "perf/LatencyTelemetry.h"

#include <iostream>
#include <limits>
#include <thread>

int main() {
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
