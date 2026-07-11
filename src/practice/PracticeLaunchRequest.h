#pragma once

#include "PracticeConfiguration.h"
#include "../bms_parser.hpp"

#include <optional>
#include <string>

namespace practice {

enum class LaunchSource {
  ChartViewer,
  NormalResult,
  PracticeResult,
  ReplayResult,
};

struct LaunchRequest {
  bms_parser::ChartMeta chartMeta;
  long long startMicros = 0;
  long long endMicros = 0;
  LaunchSource source = LaunchSource::ChartViewer;
  std::optional<int> replayId;
};

[[nodiscard]] std::optional<std::string>
validateLaunchRequest(const LaunchRequest &request);

[[nodiscard]] Configuration applyLaunchRequest(const Configuration &lastUsed,
                                               const LaunchRequest &request,
                                               long long chartEndMicros);

} // namespace practice
