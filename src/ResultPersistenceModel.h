#pragma once

#include "ModernResult.h"
#include "ReplayData.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace result_persistence {

struct ChartResultAttempt {
  std::string attemptId;
  ReplayData replay;
  ChartScoreWrite score;
  int keyMode = 0;
  GaugeType adoptedGaugeType = GaugeType::Normal;
  std::vector<float> adoptedGaugeHistory;
  std::optional<ChartJudgementTiming> judgementTiming;
  std::string payloadFingerprint;
};

struct StageReceipt {
  std::string attemptId;
  int replayId = 0;
  std::string createdAt;
  bool scorePending = false;
};

[[nodiscard]] std::optional<ChartResultAttempt> makeChartResultAttempt(
    std::string attemptId, const bms_parser::ChartMeta &meta,
    const RhythmState &state, const ScoreProvenance &provenance,
    int storageLongNoteMode, ReplayData replay, std::string &diagnostic);

[[nodiscard]] std::string payloadFingerprint(const ReplayData &replay,
                                             const ChartScoreWrite &score);

} // namespace result_persistence
