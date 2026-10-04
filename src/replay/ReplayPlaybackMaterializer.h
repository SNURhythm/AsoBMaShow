#pragma once

#include "ReplayPlaybackDriver.h"

#include "../ModernResult.h"
#include "../scene/play/GameplayScoreState.h"

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
  std::optional<GaugeStateSnapshot> initialGaugeState;
  std::optional<GaugeStateSnapshot> finalGaugeState;
  int endingCombo = 0;
  std::shared_ptr<ReplayData> replayData;
  // Lane/time adapters cannot represent a judged detached endpoint identity.
  bool consumerIdentityCompatible = true;
  std::string diagnostic;

  [[nodiscard]] bool matched() const noexcept {
    return state == ReplayPlaybackMaterializationState::Matched;
  }
  [[nodiscard]] bool playable() const noexcept {
    return (state == ReplayPlaybackMaterializationState::Matched ||
            state == ReplayPlaybackMaterializationState::ResultMismatch) &&
           replayData != nullptr;
  }
};

struct ReplayPlaybackCarryState {
  std::optional<GaugeStateSnapshot> gauge;
  int combo = 0;
  int maximumCombo = 0;
  CourseJudgementConstraint courseJudgement = CourseJudgementConstraint::None;
  std::optional<GaugeProfile> courseGaugeProfile;
};

class ReplayPlaybackMaterializer {
public:
  [[nodiscard]] static ReplayPlaybackMaterializationOutcome materialize(
      const ReplayChartDocument &document,
      const result_persistence::ModernChartResult &savedResult,
      const ReplayJudgingSink &judge,
      std::size_t eventBudget = kDefaultReplayPlaybackEventBudget);

  // Builds a judged, in-memory track and marks differing saved facts as stale.
  // Authenticated historical provenance and the original input remain intact.
  [[nodiscard]] static ReplayPlaybackMaterializationOutcome
  materializeForConsumers(
      const ReplayChartDocument &document,
      const result_persistence::ModernChartResult &savedResult,
      const bms_parser::Chart &chart,
      std::size_t eventBudget = kDefaultReplayPlaybackEventBudget);

  [[nodiscard]] static ReplayPlaybackMaterializationOutcome
  materializeForConsumers(
      const ReplayChartDocument &document,
      const result_persistence::ModernChartResult &savedResult,
      const bms_parser::Chart &chart, const ReplayPlaybackCarryState &carry,
      std::size_t eventBudget = kDefaultReplayPlaybackEventBudget);
};

} // namespace replay
