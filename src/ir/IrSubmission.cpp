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

bool replayEventMutatesGauge(const ReplayEvent &event) {
  switch (event.action) {
  case ReplayEventAction::Mine:
  case ReplayEventAction::Gauge:
    return true;
  case ReplayEventAction::Press:
  case ReplayEventAction::Release:
  case ReplayEventAction::Miss:
    return event.judgement != None;
  }
  return false;
}

} // namespace

IrSubmissionBuildOutcome makeIrSubmission(
    const result_persistence::ChartResultAttempt &attempt,
    std::int64_t playedAtUnixMillis) noexcept {
  try {
    const auto &score = attempt.score;
    const auto &meta = attempt.replay.chartMeta;
    if (!uuid::isCanonicalLowerV4(attempt.attemptId)) {
      return invalid("attempt ID is not a canonical version-4 UUID");
    }
    if (playedAtUnixMillis <= 0) {
      return invalid("play completion time must be positive");
    }
    if (meta.KeyMode <= 0) {
      return invalid("chart key mode must be positive");
    }

    const std::string md5 = normalizedHash(score.chartMd5);
    const std::string sha256 = normalizedHash(score.chartSha256);
    if ((!md5.empty() && !isHexDigest(md5, 32)) ||
        (!sha256.empty() && !isHexDigest(sha256, 64)) ||
        (md5.empty() && sha256.empty())) {
      return invalid("chart hash identity is malformed");
    }
    const std::string replayMd5 = normalizedHash(meta.MD5);
    const std::string replaySha256 = normalizedHash(meta.SHA256);
    if ((!replayMd5.empty() && replayMd5 != md5) ||
        (!replaySha256.empty() && replaySha256 != sha256)) {
      return invalid("score and replay chart identities disagree");
    }

    const std::array counts{score.maxCombo, score.comboBreak, score.pGreat,
                            score.great,    score.good,       score.bad,
                            score.poor,     score.kPoor,      score.fast,
                            score.slow};
    if (std::ranges::any_of(counts, [](int value) { return value < 0; })) {
      return invalid("score counters must not be negative");
    }
    if (score.maxScore <= 0 || score.score < 0 || score.score > score.maxScore ||
        meta.TotalNotes <= 0 || meta.TotalNotes > std::numeric_limits<int>::max() / 2 ||
        score.maxScore != meta.TotalNotes * 2 ||
        score.maxCombo > meta.TotalNotes) {
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

    std::optional<GaugeType> adoptedGaugeType;
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
    std::int64_t observedReplayScore = 0;
    for (const ReplayEvent &event : attempt.replay.events) {
      const std::int64_t eventScore = event.score;
      const std::int64_t scoreDelta = eventScore - observedReplayScore;
      observedReplayScore = std::max(observedReplayScore, eventScore);
      if (!replayEventMutatesGauge(event)) {
        continue;
      }
      if (!std::isfinite(event.gauge)) {
        return invalid("replay gauge history is not finite");
      }
      adoptedGaugeType = event.gaugeType;
      const bool authenticJudgementEvent =
          event.action == ReplayEventAction::Press ||
          event.action == ReplayEventAction::Release ||
          event.action == ReplayEventAction::Miss;
      if (authenticJudgementEvent && event.judgement == PGreat &&
          scoreDelta == 2) {
        if (event.diffMicros < 0) {
          ++pGreatFast;
        } else if (event.diffMicros > 0) {
          ++pGreatSlow;
        }
        if (event.diffMicros <= 0) {
          ++earlyPGreat;
        } else {
          ++latePGreat;
        }
      } else if (authenticJudgementEvent && event.judgement == Great &&
                 scoreDelta == 1) {
        if (event.diffMicros <= 0) {
          ++earlyGreat;
        } else {
          ++lateGreat;
        }
      } else if (authenticJudgementEvent && event.judgement == Good) {
        if (event.diffMicros <= 0) {
          ++earlyGood;
        } else {
          ++lateGood;
        }
      } else if (authenticJudgementEvent && event.judgement == Bad) {
        if (event.diffMicros <= 0) {
          ++earlyBad;
        } else {
          ++lateBad;
        }
      } else if (authenticJudgementEvent && event.judgement == Poor) {
        if (event.diffMicros <= 0) {
          ++earlyPoor;
        } else {
          ++latePoor;
        }
      }
    }

    std::vector<float> gaugeHistory;
    if (!attempt.adoptedGaugeHistory.empty()) {
      if (std::ranges::any_of(attempt.adoptedGaugeHistory, [](float value) {
            return !std::isfinite(value);
          })) {
        return invalid("adopted gauge history is not finite");
      }
      gaugeHistory = attempt.adoptedGaugeHistory;
    } else if (adoptedGaugeType) {
      gaugeHistory.reserve(attempt.replay.events.size());
      for (const ReplayEvent &event : attempt.replay.events) {
        if (replayEventMutatesGauge(event) &&
            event.gaugeType == *adoptedGaugeType) {
          gaugeHistory.push_back(event.gauge);
        }
      }
    }
    if (pGreatFast > score.fast || pGreatSlow > score.slow) {
      return invalid("PGREAT timing exceeds aggregate timing counts");
    }
    const bool judgementTimingBreakdownAvailable =
        earlyPGreat + latePGreat == score.pGreat &&
        earlyGreat + lateGreat == score.great &&
        earlyGood + lateGood == score.good &&
        earlyBad + lateBad == score.bad &&
        earlyPoor + latePoor == score.poor;

    return {.value = IrSubmission{
                .attemptId = attempt.attemptId,
                .keyMode = meta.KeyMode,
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
                .playedAtUnixMillis = playedAtUnixMillis,
                .provenance = score.provenance,
            }};
  } catch (...) {
    return invalid("canonical submission construction failed");
  }
}

} // namespace ir
