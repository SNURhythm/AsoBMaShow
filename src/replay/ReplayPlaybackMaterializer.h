#pragma once

#include "ReplayPlaybackDriver.h"

#include "../ModernResult.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

struct ReplayData;
namespace bms_parser {
class Chart;
}

namespace replay {

struct ReplayJudgingSink {
  std::function<bool(std::int64_t, std::string &)> advanceTo;
  std::function<bool(const InputTransition &, std::string &)> applyInput;
  std::function<bool(std::span<const InputTransition>, std::string &)>
      applyInputBatch;
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
  std::optional<result_persistence::ModernChartResult> judgedResult;
  std::shared_ptr<ReplayData> replayData;
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

  // Builds the single judged, in-memory compatibility track used by Watch,
  // Retry Same, G-Battle, practice ghost, and video export. It is returned
  // only when independently materialized facts agree with the saved result.
  [[nodiscard]] static ReplayPlaybackMaterializationOutcome
  materializeForConsumers(
      const ReplayChartDocument &document, ReplaySetupSource source,
      const result_persistence::ModernChartResult &savedResult,
      const bms_parser::Chart &chart,
      std::size_t eventBudget = kDefaultReplayPlaybackEventBudget);
};

} // namespace replay
