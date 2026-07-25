#pragma once

#include "ReplayData.h"
#include "ResultPersistenceModel.h"

#include <optional>
#include <string>

namespace legacy_result_persistence {

struct LegacyChartResultAttempt {
  std::string attemptId;
  ReplayData replay;
  result_persistence::ChartScoreWrite score;
  std::vector<float> adoptedGaugeHistory;
  std::optional<result_persistence::ChartJudgementTiming> judgementTiming;
  std::string payloadFingerprint;
};

[[nodiscard]] std::optional<LegacyChartResultAttempt>
makeLegacyChartResultAttempt(std::string attemptId,
                             const bms_parser::ChartMeta &meta,
                             const RhythmState &state,
                             const ScoreProvenance &provenance,
                             int storageLongNoteMode, ReplayData replay,
                             std::string &diagnostic);

[[nodiscard]] std::string
legacyPayloadFingerprint(const ReplayData &replay,
                         const result_persistence::ChartScoreWrite &score);

} // namespace legacy_result_persistence
