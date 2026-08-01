#pragma once

#include "../ModernResult.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
  int pGreatFast = 0;
  int pGreatSlow = 0;
  bool judgementTimingBreakdownAvailable = false;
  int earlyPGreat = 0;
  int latePGreat = 0;
  int earlyGreat = 0;
  int lateGreat = 0;
  int earlyGood = 0;
  int lateGood = 0;
  int earlyBad = 0;
  int lateBad = 0;
  int earlyPoor = 0;
  int latePoor = 0;
  std::vector<float> gaugeHistory;
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

[[nodiscard]] bool validateIrSubmission(const IrSubmission &submission,
                                        std::string &diagnostic) noexcept;

[[nodiscard]] IrSubmissionBuildOutcome
makeIrSubmission(const result_persistence::ModernChartResult &result) noexcept;

} // namespace ir
