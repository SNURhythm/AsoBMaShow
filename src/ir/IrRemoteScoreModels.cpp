#include "IrRemoteScoreModels.h"

#include "../CanonicalDigest.h"
#include "../ResultContracts.h"
#include "../scene/play/GameplayGaugeTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <unordered_set>

namespace ir {
namespace {

void setDiagnostic(std::string &diagnostic, std::string_view message) noexcept {
  try {
    diagnostic.assign(message.substr(0, kMaximumIrRemoteScoreDiagnosticBytes));
  } catch (...) {
    try {
      diagnostic.clear();
    } catch (...) {
    }
  }
}

bool fail(std::string &diagnostic, std::string_view message) noexcept {
  setDiagnostic(diagnostic, message);
  return false;
}

bool hasSafeBytes(std::string_view value, std::size_t maximumBytes,
                  bool requireValue = false) noexcept {
  if ((requireValue && value.empty()) || value.size() > maximumBytes) {
    return false;
  }
  return std::ranges::none_of(value, [](unsigned char character) {
    return character < 0x20U || character == 0x7fU;
  });
}

bool validOptionalText(const std::optional<std::string> &value) noexcept {
  return !value || hasSafeBytes(*value, kMaximumIrRemoteScoreTextBytes);
}

bool hasValidHashIdentity(const IrRemoteScore &score) noexcept {
  const bool validMd5 =
      score.chartMd5.empty() ||
      canonical_digest::isCanonicalLowerHex(score.chartMd5, 32);
  const bool validSha256 =
      score.chartSha256.empty() ||
      canonical_digest::isCanonicalLowerHex(score.chartSha256, 64);
  return validMd5 && validSha256 &&
         (!score.chartMd5.empty() || !score.chartSha256.empty());
}

bool isKnownLampRank(int value) noexcept {
  constexpr std::array ranks{
      kClearTypeFailedRank,
      kClearTypeAssistedEasyClearRank,
      kClearTypeLightAssistedEasyClearRank,
      kClearTypeEasyClearRank,
      kClearTypeNormalClearRank,
      kClearTypeHardClearRank,
      kClearTypeExHardClearRank,
      kClearTypeFullComboRank,
  };
  return std::ranges::find(ranks, value) != ranks.end();
}

bool nonnegative(const std::optional<int> &value) noexcept {
  return !value || *value >= 0;
}

bool validJudgements(const IrRemoteJudgements &judgements) noexcept {
  return nonnegative(judgements.pGreat) && nonnegative(judgements.great) &&
         nonnegative(judgements.good) && nonnegative(judgements.bad) &&
         nonnegative(judgements.poor);
}

bool validTiming(const IrRemoteTimingBreakdown &timing) noexcept {
  return nonnegative(timing.earlyPGreat) && nonnegative(timing.latePGreat) &&
         nonnegative(timing.earlyGreat) && nonnegative(timing.lateGreat) &&
         nonnegative(timing.earlyGood) && nonnegative(timing.lateGood) &&
         nonnegative(timing.earlyBad) && nonnegative(timing.lateBad) &&
         nonnegative(timing.earlyPoor) && nonnegative(timing.latePoor);
}

template <typename Floating> bool validGaugeValue(Floating value) noexcept {
  return std::isfinite(value) && value >= static_cast<Floating>(0) &&
         value <= static_cast<Floating>(100);
}

bool validateScore(const IrRemoteScore &score,
                   std::string &diagnostic) noexcept {
  if (score.game != "bms-7k" && score.game != "bms-14k") {
    return fail(diagnostic, "IR remote score game is unsupported");
  }
  if (score.remoteUserId <= 0) {
    return fail(diagnostic, "IR remote score user identity is invalid");
  }
  if (!hasSafeBytes(score.remoteScoreId, kMaximumIrRemoteScoreIdBytes, true) ||
      !hasSafeBytes(score.remoteChartId, kMaximumIrRemoteScoreIdBytes, true)) {
    return fail(diagnostic, "IR remote score identity is invalid");
  }
  if (!hasValidHashIdentity(score)) {
    return fail(diagnostic, "IR remote score chart hash is invalid");
  }
  if (!hasSafeBytes(score.title, kMaximumIrRemoteScoreTextBytes) ||
      !hasSafeBytes(score.artist, kMaximumIrRemoteScoreTextBytes) ||
      !hasSafeBytes(score.service, kMaximumIrRemoteScoreTextBytes) ||
      !validOptionalText(score.difficulty) || !validOptionalText(score.level) ||
      !validOptionalText(score.random) || !validOptionalText(score.gauge) ||
      !validOptionalText(score.inputDevice) ||
      !validOptionalText(score.client)) {
    return fail(diagnostic, "IR remote score text is invalid or oversized");
  }
  if (score.levelNumber &&
      (!std::isfinite(*score.levelNumber) || *score.levelNumber < 0.0)) {
    return fail(diagnostic, "IR remote score level number is invalid");
  }
  const auto maximumScore =
      result_contract::maximumScoreForNotes(score.noteCount);
  if (!maximumScore || score.score < 0 || score.score > *maximumScore) {
    return fail(diagnostic, "IR remote score range is invalid");
  }
  if (!isKnownLampRank(score.lampRank)) {
    return fail(diagnostic, "IR remote score lamp rank is invalid");
  }
  if (score.timeAddedUnixMillis <= 0 ||
      (score.timeAchievedUnixMillis && *score.timeAchievedUnixMillis <= 0)) {
    return fail(diagnostic, "IR remote score timestamp is invalid");
  }
  if (!validJudgements(score.judgements) || !validTiming(score.timing) ||
      !nonnegative(score.fast) || !nonnegative(score.slow) ||
      !nonnegative(score.maxCombo) || !nonnegative(score.badPoints) ||
      (score.maxCombo && *score.maxCombo > score.noteCount)) {
    return fail(diagnostic, "IR remote score metric is invalid");
  }
  if (score.finalGauge && !validGaugeValue(*score.finalGauge)) {
    return fail(diagnostic, "IR remote score final gauge is invalid");
  }
  if (score.gaugeHistory.size() > kMaximumIrRemoteGaugeHistoryEntries) {
    return fail(diagnostic, "IR remote score gauge history is oversized");
  }
  if (std::ranges::any_of(score.gaugeHistory, [](const auto &value) {
        return value && !validGaugeValue(*value);
      })) {
    return fail(diagnostic, "IR remote score gauge history is invalid");
  }
  setDiagnostic(diagnostic, {});
  return true;
}

} // namespace

bool IrRemoteJudgements::complete() const noexcept {
  return pGreat.has_value() && great.has_value() && good.has_value() &&
         bad.has_value() && poor.has_value();
}

bool validateIrRemoteScore(const IrRemoteScore &score,
                           std::string &diagnostic) noexcept {
  try {
    return validateScore(score, diagnostic);
  } catch (...) {
    return fail(diagnostic, "IR remote score validation failed");
  }
}

bool validateIrUserScoreSnapshot(const IrUserScoreSnapshot &snapshot,
                                 std::string &diagnostic) noexcept {
  try {
    if (snapshot.scores.size() > kMaximumIrRemoteScoreSnapshotEntries) {
      return fail(diagnostic, "IR remote score snapshot is oversized");
    }
    std::unordered_set<std::string_view> remoteScoreIds;
    remoteScoreIds.reserve(snapshot.scores.size());
    for (const auto &score : snapshot.scores) {
      if (!validateScore(score, diagnostic)) {
        return false;
      }
      if (!remoteScoreIds.emplace(score.remoteScoreId).second) {
        return fail(diagnostic,
                    "IR remote score snapshot has duplicate identity");
      }
    }
    setDiagnostic(diagnostic, {});
    return true;
  } catch (...) {
    return fail(diagnostic, "IR remote score snapshot validation failed");
  }
}

} // namespace ir
