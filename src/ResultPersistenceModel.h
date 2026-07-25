#pragma once

#include "ReplayData.h"

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

[[nodiscard]] inline std::string
describeChartScoreDifference(const ChartScoreWrite &expected,
                             const ChartScoreWrite &actual) {
  std::vector<std::string> differences;
  const auto opaque = [&](std::string_view name, const auto &left,
                          const auto &right) {
    if (left != right) {
      differences.emplace_back(name);
    }
  };
  const auto scalar = [&](std::string_view name, const auto &left,
                          const auto &right) {
    if (left != right) {
      differences.push_back(std::string(name) +
                            " expected=" + std::to_string(left) +
                            " actual=" + std::to_string(right));
    }
  };

  opaque("chartPath", expected.chartPath, actual.chartPath);
  opaque("chartMd5", expected.chartMd5, actual.chartMd5);
  opaque("chartSha256", expected.chartSha256, actual.chartSha256);
  opaque("chartTitle", expected.chartTitle, actual.chartTitle);
  opaque("chartArtist", expected.chartArtist, actual.chartArtist);
  scalar("longNoteMode", expected.longNoteMode, actual.longNoteMode);
  scalar("score", expected.score, actual.score);
  scalar("maxScore", expected.maxScore, actual.maxScore);
  scalar("maxCombo", expected.maxCombo, actual.maxCombo);
  scalar("comboBreak", expected.comboBreak, actual.comboBreak);
  scalar("pGreat", expected.pGreat, actual.pGreat);
  scalar("great", expected.great, actual.great);
  scalar("good", expected.good, actual.good);
  scalar("bad", expected.bad, actual.bad);
  scalar("poor", expected.poor, actual.poor);
  scalar("kPoor", expected.kPoor, actual.kPoor);
  scalar("fast", expected.fast, actual.fast);
  scalar("slow", expected.slow, actual.slow);
  const std::uint32_t expectedGaugeBits =
      std::bit_cast<std::uint32_t>(expected.finalGauge);
  const std::uint32_t actualGaugeBits =
      std::bit_cast<std::uint32_t>(actual.finalGauge);
  if (expectedGaugeBits != actualGaugeBits) {
    differences.push_back(
        "finalGauge expected=" + std::to_string(expected.finalGauge) +
        " actual=" + std::to_string(actual.finalGauge) +
        " expectedBits=" + std::to_string(expectedGaugeBits) +
        " actualBits=" + std::to_string(actualGaugeBits));
  }
  scalar("clearType", expected.clearType, actual.clearType);
  opaque("provenance", expected.provenance, actual.provenance);

  if (differences.empty()) {
    return {};
  }
  std::string result = "score payload differs: ";
  for (std::size_t index = 0; index < differences.size(); ++index) {
    if (index != 0) {
      result += "; ";
    }
    result += differences[index];
  }
  return result;
}

[[nodiscard]] bool
hasProjectableChartIdentity(const ChartScoreWrite &score) noexcept;

struct ChartJudgementTiming {
  std::array<JudgementFastSlowCount, JudgementCount> byJudgement{};

  bool operator==(const ChartJudgementTiming &) const = default;
};

struct ChartResultAttempt {
  std::string attemptId;
  ReplayData replay;
  ChartScoreWrite score;
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
