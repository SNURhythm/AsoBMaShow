#include "IrSubmission.h"
#include "../ReplayClearMarkUtils.h"

#include <string_view>
#include <utility>

namespace ir {
namespace {

IrSubmissionBuildOutcome invalid(std::string_view diagnostic) {
  return {.diagnostic = std::string(diagnostic)};
}

IrSubmission
submissionFromResult(const result_persistence::ModernChartResult &result) {
  const auto &score = result.score;
  IrSubmission submission{
      .attemptId = result.attemptId,
      .keyMode = result.keyMode,
      .chartMd5 = score.chartMd5,
      .chartSha256 = score.chartSha256,
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
      .gaugeHistory = result.adoptedGaugeHistory,
      .finalGauge = score.finalGauge,
      .clearType = replay_clear_mark::effectiveClearRank(score),
      .playedAtUnixMillis = result.playedAtUnixMillis,
      .provenance = score.provenance,
  };
  if (!result.judgementTiming) {
    return submission;
  }

  const auto &timing = result.judgementTiming->byJudgement;
  const auto early = [&](Judgement judgement, int total) {
    return total - timing[static_cast<std::size_t>(judgement)].slow;
  };
  const auto late = [&](Judgement judgement) {
    return timing[static_cast<std::size_t>(judgement)].slow;
  };
  submission.pGreatFast = timing[PGreat].fast;
  submission.pGreatSlow = timing[PGreat].slow;
  submission.judgementTimingBreakdownAvailable = true;
  submission.earlyPGreat = early(PGreat, score.pGreat);
  submission.latePGreat = late(PGreat);
  submission.earlyGreat = early(Great, score.great);
  submission.lateGreat = late(Great);
  submission.earlyGood = early(Good, score.good);
  submission.lateGood = late(Good);
  submission.earlyBad = early(Bad, score.bad);
  submission.lateBad = late(Bad);
  submission.earlyPoor = early(Poor, score.poor);
  submission.latePoor = late(Poor);
  return submission;
}

} // namespace

IrSubmissionBuildOutcome
makeIrSubmission(const result_persistence::ModernChartResult &result) noexcept {
  try {
    std::string diagnostic;
    if (!result_persistence::validateModernChartResult(result, diagnostic)) {
      return invalid(diagnostic);
    }
    IrSubmission submission = submissionFromResult(result);
    if (!validateIrSubmission(submission, diagnostic)) {
      return invalid(diagnostic);
    }
    return {.value = std::move(submission)};
  } catch (...) {
    return invalid("canonical submission construction failed");
  }
}

} // namespace ir
