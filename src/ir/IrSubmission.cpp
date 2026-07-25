#include "IrSubmission.h"

#include "IrOutboxModels.h"
#include "../Uuid.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace ir {
namespace {

std::string normalizedHash(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

bool isHexDigest(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

bool isKnownClearRank(int value) {
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

IrSubmissionBuildOutcome invalid(std::string_view diagnostic) {
  return {.diagnostic = sanitizeDiagnostic(diagnostic)};
}

} // namespace

IrSubmissionBuildOutcome makeIrSubmission(
    const result_persistence::PersistedChartResult &result) noexcept {
  try {
    const auto &score = result.score;
    if (!result.attemptId.has_value() ||
        !uuid::isCanonicalLowerV4(*result.attemptId)) {
      return invalid("attempt ID is not a canonical version-4 UUID");
    }
    if (result.playedAtUnixMillis <= 0) {
      return invalid("play completion time must be positive");
    }
    if (result.keyMode <= 0) {
      return invalid("chart key mode must be positive");
    }

    const std::string md5 = normalizedHash(score.chartMd5);
    const std::string sha256 = normalizedHash(score.chartSha256);
    if ((!md5.empty() && !isHexDigest(md5, 32)) ||
        (!sha256.empty() && !isHexDigest(sha256, 64)) ||
        (md5.empty() && sha256.empty())) {
      return invalid("chart hash identity is malformed");
    }
    const std::array counts{score.maxCombo, score.comboBreak, score.pGreat,
                            score.great,    score.good,       score.bad,
                            score.poor,     score.kPoor,      score.fast,
                            score.slow};
    if (std::ranges::any_of(counts, [](int value) { return value < 0; })) {
      return invalid("score counters must not be negative");
    }
    if (score.maxScore <= 0 || score.score < 0 || score.score > score.maxScore ||
        score.maxScore % 2 != 0 ||
        score.maxCombo > score.maxScore / 2) {
      return invalid("score range is inconsistent with chart notes");
    }
    const long long expectedScore =
        static_cast<long long>(score.pGreat) * 2LL + score.great;
    if (expectedScore != score.score) {
      return invalid("EX score disagrees with judgement counts");
    }
    if (!std::isfinite(score.finalGauge) || !isKnownClearRank(score.clearType)) {
      return invalid("score gauge or clear rank is invalid");
    }

    int pGreatFast = 0;
    int pGreatSlow = 0;
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
    bool judgementTimingBreakdownAvailable = false;
    if (result.judgementTiming.has_value()) {
      const auto &timing = result.judgementTiming->byJudgement;
      std::array<int, JudgementCount> judgementTotals{};
      judgementTotals[PGreat] = score.pGreat;
      judgementTotals[Great] = score.great;
      judgementTotals[Good] = score.good;
      judgementTotals[Bad] = score.bad;
      judgementTotals[Kpoor] = score.kPoor;
      judgementTotals[Poor] = score.poor;

      std::int64_t capturedFast = 0;
      std::int64_t capturedSlow = 0;
      for (int index = 0; index < JudgementCount; ++index) {
        const auto judgement = static_cast<Judgement>(index);
        const auto &count = timing[static_cast<std::size_t>(index)];
        if (count.fast < 0 || count.slow < 0) {
          return invalid("captured judgement timing must not be negative");
        }
        if (judgement == Kpoor || judgement == None) {
          if (count.fast != 0 || count.slow != 0) {
            return invalid("KPOOR and NONE cannot have captured timing");
          }
          continue;
        }
        if (static_cast<std::int64_t>(count.fast) + count.slow >
            judgementTotals[static_cast<std::size_t>(index)]) {
          return invalid("captured judgement timing exceeds result totals");
        }
        capturedFast += count.fast;
        capturedSlow += count.slow;
      }
      if (capturedFast != score.fast || capturedSlow != score.slow) {
        return invalid(
            "captured judgement timing disagrees with aggregate timing");
      }

      const auto early = [&](Judgement judgement) {
        return judgementTotals[static_cast<std::size_t>(judgement)] -
               timing[static_cast<std::size_t>(judgement)].slow;
      };
      const auto late = [&](Judgement judgement) {
        return timing[static_cast<std::size_t>(judgement)].slow;
      };
      pGreatFast = timing[PGreat].fast;
      pGreatSlow = timing[PGreat].slow;
      earlyPGreat = early(PGreat);
      latePGreat = late(PGreat);
      earlyGreat = early(Great);
      lateGreat = late(Great);
      earlyGood = early(Good);
      lateGood = late(Good);
      earlyBad = early(Bad);
      lateBad = late(Bad);
      earlyPoor = early(Poor);
      latePoor = late(Poor);
      judgementTimingBreakdownAvailable = true;
    }

    std::vector<float> gaugeHistory = result.adoptedGaugeHistory;
    if (std::ranges::any_of(gaugeHistory,
                            [](float value) { return !std::isfinite(value); })) {
      return invalid("adopted gauge history is not finite");
    }

    return {.value = IrSubmission{
                .attemptId = *result.attemptId,
                .keyMode = result.keyMode,
                .chartMd5 = md5,
                .chartSha256 = sha256,
                .score = score.score,
                .maxScore = score.maxScore,
                .maxCombo = score.maxCombo,
                .comboBreak = score.comboBreak,
                .pGreat = score.pGreat,
                .great = score.great,
                .good = score.good,
                .bad = score.bad,
                .poor = score.poor,
                .kPoor = score.kPoor,
                .fast = score.fast,
                .slow = score.slow,
                .pGreatFast = pGreatFast,
                .pGreatSlow = pGreatSlow,
                .judgementTimingBreakdownAvailable =
                    judgementTimingBreakdownAvailable,
                .earlyPGreat = earlyPGreat,
                .latePGreat = latePGreat,
                .earlyGreat = earlyGreat,
                .lateGreat = lateGreat,
                .earlyGood = earlyGood,
                .lateGood = lateGood,
                .earlyBad = earlyBad,
                .lateBad = lateBad,
                .earlyPoor = earlyPoor,
                .latePoor = latePoor,
                .gaugeHistory = std::move(gaugeHistory),
                .finalGauge = score.finalGauge,
                .clearType = score.clearType,
                .playedAtUnixMillis = result.playedAtUnixMillis,
                .provenance = score.provenance,
            }};
  } catch (...) {
    return invalid("canonical submission construction failed");
  }
}

} // namespace ir
