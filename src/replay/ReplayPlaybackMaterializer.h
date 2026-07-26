#pragma once

#include "ReplayPlaybackData.h"
#include "../scene/play/GameplayRulesetPolicy.h"
#include "../scene/play/GameplaySimulation.h"

#include <optional>
#include <string>
#include <vector>

namespace bms_parser {
class Chart;
}

namespace replay {

struct ReplayMaterializationSeed {
  int carriedCombo = 0;
  int carriedMaxCombo = 0;
};

struct MaterializedReplay {
  std::vector<gameplay::GameplayReplayEvent> judgedEvents;
  gameplay::GameplayAttemptSnapshot attempt;
  std::vector<float> gaugeHistory;
};

struct MaterializeOutcome {
  enum class Status { Materialized, LegacyTrack, Invalid, CapacityExceeded };

  Status status = Status::Invalid;
  std::optional<MaterializedReplay> value;
  std::string diagnostic;

  [[nodiscard]] bool materialized() const noexcept {
    return status == Status::Materialized && value.has_value();
  }
};

// Derived playback annotations for video, ghosts, pacemakers, and G-Battle.
// The result must never be persisted or used to construct an IR submission.
[[nodiscard]] MaterializeOutcome materializeReplay(
    const ReplayPlaybackData &, const bms_parser::Chart &,
    const gameplay::GameplayRulesetPolicy &,
    ReplayMaterializationSeed seed = {});

} // namespace replay
