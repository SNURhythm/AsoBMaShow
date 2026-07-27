#pragma once

#include "ReplayFileActionService.h"

#include "../ResultRecordSummary.h"

#include <optional>

namespace replay {

struct ReplayFileActionSelection {
  std::optional<ReplayFileActionRequest> request;
  bool shareVisible = false;
  bool deleteVisible = false;
  bool enabled = false;
};

[[nodiscard]] ReplayFileActionSelection
replayFileActionSelection(const ResultRecordSummary &summary,
                          bool interactive) noexcept;

} // namespace replay
