#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ir {

inline constexpr std::size_t kMaximumIrRemoteScoreSnapshotEntries = 50'000;
inline constexpr std::size_t kMaximumIrRemoteScoreIdBytes = 512;
inline constexpr std::size_t kMaximumIrRemoteScoreTextBytes = 2 * 1024;
inline constexpr std::size_t kMaximumIrRemoteGaugeHistoryEntries = 100'000;
inline constexpr std::size_t kMaximumIrRemoteScoreDiagnosticBytes = 512;

enum class IrUserScoreSnapshotStatus {
  Succeeded,
  AuthenticationRequired,
  TransientFailure,
  MalformedResponse,
  OversizedResponse,
  Unsupported,
  Cancelled,
};

struct IrRemoteJudgements {
  std::optional<int> pGreat;
  std::optional<int> great;
  std::optional<int> good;
  std::optional<int> bad;
  std::optional<int> poor;

  [[nodiscard]] bool complete() const noexcept;
};

struct IrRemoteTimingBreakdown {
  std::optional<int> earlyPGreat;
  std::optional<int> latePGreat;
  std::optional<int> earlyGreat;
  std::optional<int> lateGreat;
  std::optional<int> earlyGood;
  std::optional<int> lateGood;
  std::optional<int> earlyBad;
  std::optional<int> lateBad;
  std::optional<int> earlyPoor;
  std::optional<int> latePoor;
};

struct IrRemoteScore {
  std::int64_t remoteUserId = 0;
  std::string game;
  std::string remoteScoreId;
  std::string remoteChartId;
  std::string chartMd5;
  std::string chartSha256;
  std::string title;
  std::string artist;
  std::string service;
  std::optional<std::string> difficulty;
  std::optional<std::string> level;
  std::optional<double> levelNumber;
  int noteCount = 0;
  int score = 0;
  int lampRank = 0;
  std::optional<std::int64_t> timeAchievedUnixMillis;
  std::int64_t timeAddedUnixMillis = 0;
  IrRemoteJudgements judgements;
  IrRemoteTimingBreakdown timing;
  std::optional<int> fast;
  std::optional<int> slow;
  std::optional<int> maxCombo;
  std::optional<int> badPoints;
  std::optional<float> finalGauge;
  std::vector<std::optional<float>> gaugeHistory;
  std::optional<std::string> random;
  std::optional<std::string> gauge;
  std::optional<std::string> inputDevice;
  std::optional<std::string> client;
};

struct IrUserScoreSnapshot {
  std::vector<IrRemoteScore> scores;
};

struct IrUserScoreSnapshotOutcome {
  IrUserScoreSnapshotStatus status =
      IrUserScoreSnapshotStatus::MalformedResponse;
  std::optional<IrUserScoreSnapshot> snapshot;
  std::string code;
  std::string diagnostic;
};

[[nodiscard]] bool validateIrRemoteScore(const IrRemoteScore &score,
                                         std::string &diagnostic) noexcept;

[[nodiscard]] bool
validateIrUserScoreSnapshot(const IrUserScoreSnapshot &snapshot,
                            std::string &diagnostic) noexcept;

} // namespace ir
