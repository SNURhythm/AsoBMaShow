#pragma once

#include "../ResultPersistenceModel.h"

#include <cstdint>
#include <optional>
#include <string>

namespace ir {

struct IrSubmission {
  std::string attemptId;
  int keyMode = 0;
  std::string chartMd5;
  std::string chartSha256;
  int score = 0;
  int maxScore = 0;
  int maxCombo = 0;
  int comboBreak = 0;
  int pGreat = 0;
  int great = 0;
  int good = 0;
  int bad = 0;
  int poor = 0;
  int kPoor = 0;
  int fast = 0;
  int slow = 0;
  float finalGauge = 0.0F;
  int clearType = kClearTypeFailedRank;
  std::int64_t playedAtUnixMillis = 0;
  ScoreProvenance provenance = ScoreProvenance::Legacy();

  bool operator==(const IrSubmission &) const = default;
};

struct IrSubmissionBuildOutcome {
  std::optional<IrSubmission> value;
  std::string diagnostic;
};

[[nodiscard]] IrSubmissionBuildOutcome makeIrSubmission(
    const result_persistence::ChartResultAttempt &attempt,
    std::int64_t playedAtUnixMillis) noexcept;

} // namespace ir
