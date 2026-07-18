#pragma once

#include "ReplayData.h"

#include <optional>
#include <string>

namespace result_persistence {

struct ChartScoreWrite {
  std::string chartPath;
  std::string chartMd5;
  std::string chartSha256;
  std::string chartTitle;
  std::string chartArtist;
  int longNoteMode = 0;
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
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  ScoreProvenance provenance = ScoreProvenance::Legacy();

  bool operator==(const ChartScoreWrite &) const = default;
};

[[nodiscard]] bool
hasProjectableChartIdentity(const ChartScoreWrite &score) noexcept;

struct ChartResultAttempt {
  std::string attemptId;
  ReplayData replay;
  ChartScoreWrite score;
  std::vector<float> adoptedGaugeHistory;
  std::string payloadFingerprint;
};

struct StageReceipt {
  std::string attemptId;
  int replayId = 0;
  std::string createdAt;
  bool scorePending = false;
};

[[nodiscard]] ChartScoreWrite captureChartScoreWrite(
    const bms_parser::ChartMeta &meta, const RhythmState &state,
    const ScoreProvenance &provenance, int storageLongNoteMode);

[[nodiscard]] std::optional<ChartResultAttempt> makeChartResultAttempt(
    std::string attemptId, const bms_parser::ChartMeta &meta,
    const RhythmState &state, const ScoreProvenance &provenance,
    int storageLongNoteMode, ReplayData replay, std::string &diagnostic);

[[nodiscard]] std::string payloadFingerprint(const ReplayData &replay,
                                             const ChartScoreWrite &score);

} // namespace result_persistence
