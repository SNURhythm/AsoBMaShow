#pragma once

#include "ReplayPlaybackDriver.h"

#include "../ModernResult.h"

#include <functional>
#include <optional>
#include <string>

namespace replay {

struct ReplayJudgingSink {
  std::function<bool(std::int64_t, std::string &)> advanceTo;
  std::function<bool(const InputTransition &, std::string &)> applyInput;
  std::function<std::optional<result_persistence::ModernChartResult>(
      std::string &)>
      finish;
};

enum class ReplayPlaybackMaterializationState {
  Matched,
  ResultMismatch,
  InvalidReplay,
  WorkLimitExceeded,
  JudgingFailed,
};

struct ReplayPlaybackMaterializationOutcome {
  ReplayPlaybackMaterializationState state =
      ReplayPlaybackMaterializationState::InvalidReplay;
  std::optional<result_persistence::ResultFactAgreement> agreement;
  std::string diagnostic;

  [[nodiscard]] bool matched() const noexcept {
    return state == ReplayPlaybackMaterializationState::Matched;
  }
};

class ReplayPlaybackMaterializer {
public:
  [[nodiscard]] static ReplayPlaybackMaterializationOutcome materialize(
      const ReplayChartDocument &document, ReplaySetupSource source,
      const result_persistence::ModernChartResult &savedResult,
      const ReplayJudgingSink &judge,
      std::size_t eventBudget = kDefaultReplayPlaybackEventBudget);
};

} // namespace replay
