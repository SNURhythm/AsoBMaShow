#pragma once

#include "../ResultPersistenceModel.h"
#include "ReplayPlaybackData.h"

#include <optional>
#include <string>

namespace replay::setup_authority {

enum class Source {
  CapturedAttempt,
  AsoExtension,
  Stock,
};

enum class Status {
  Resolved,
  Conflict,
  Invalid,
};

struct Outcome {
  Status status = Status::Invalid;
  std::optional<ChartPlaybackSetup> setup;
  std::string field;
  std::string diagnostic;

  [[nodiscard]] bool resolved() const noexcept {
    return status == Status::Resolved && setup.has_value();
  }
};

[[nodiscard]] Outcome resolveForResult(
    ChartPlaybackSetup setup,
    const result_persistence::ChartScoreWrite &score, int expectedKeyMode,
    Source source, bool carriedStartingGaugeAllowed);

} // namespace replay::setup_authority
