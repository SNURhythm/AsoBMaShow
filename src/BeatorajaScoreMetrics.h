#pragma once

#include "ReplayData.h"

#include <cstddef>
#include <optional>
#include <vector>

struct BeatorajaResultTimingStatistics {
  bool hasTimingSamples = false;
  std::size_t timingSampleCount = 0;
  double averageMillis = 0.0;
  double standardDeviationMillis = 0.0;
  long long averageJudgeMicros = 0;
  std::vector<int> distribution = std::vector<int>(301, 0);
};

[[nodiscard]] std::optional<BeatorajaResultTimingStatistics>
beatorajaResultTimingStatistics(const ReplayData *replay, int totalNotes,
                                bms_parser::Chart *chart);
