#pragma once

#include "../ReplayData.h"
#include "../bms_parser.hpp"
#include "PracticeConfiguration.h"

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

struct ParsedLaunchApplication {
  Configuration configuration;
  std::optional<std::string> issue;

  [[nodiscard]] bool applied() const noexcept { return !issue.has_value(); }
};

[[nodiscard]] bms_parser::ChartMeta
mergeReplayLaunchChartMeta(const bms_parser::ChartMeta &authoritative,
                           const ReplayData &replay);

[[nodiscard]] std::optional<std::string>
validateLaunchRequest(const LaunchRequest &request);

[[nodiscard]] Configuration applyLaunchRequest(const Configuration &lastUsed,
                                               const LaunchRequest &request,
                                               long long chartEndMicros);

[[nodiscard]] ParsedLaunchApplication applyLaunchRequestForParsedChart(
    const Configuration &lastUsed, const LaunchRequest &request,
    const bms_parser::ChartMeta &parsedChartMeta, long long chartEndMicros);

} // namespace practice
