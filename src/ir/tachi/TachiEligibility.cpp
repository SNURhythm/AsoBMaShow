#include "TachiEligibility.h"

#include "../../BmsMetadataText.h"
#include "../../ResultContracts.h"
#include "../../Uuid.h"
#include "../../scene/play/GameplayGaugeRules.h"
#include "../../scene/play/GameplayJudgeRules.h"

#include <array>
#include <cmath>
#include <ranges>

namespace ir::tachi {
namespace {

SubmissionEligibilityOutcome eligible() {
  return {.reason = SubmissionEligibilityReason::Eligible};
}

SubmissionEligibilityOutcome rejected(SubmissionEligibilityReason reason,
                                      std::string_view diagnostic) {
  return {.reason = reason, .diagnostic = sanitizeDiagnostic(diagnostic)};
}

bool chartHashMatches(std::string_view submitted, std::string_view recorded) {
  return submitted.empty() ? recorded.empty() : submitted == recorded;
}

bool canonicalJudgeWindows(const ScoreStageProvenance &stage, int rank) {
  const auto expected =
      gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, rank);
  if (stage.effectiveJudgeWindows.size() != 20) {
    return false;
  }
  constexpr std::array contexts{
      gameplay::JudgeWindowContext::Normal,
      gameplay::JudgeWindowContext::Scratch,
      gameplay::JudgeWindowContext::LongNoteTail,
      gameplay::JudgeWindowContext::LongScratchTail,
  };
  for (std::size_t contextIndex = 0; contextIndex < contexts.size();
       ++contextIndex) {
    for (const auto &window : expected.contexts[contextIndex].windows) {
      const auto found = std::ranges::find_if(
          stage.effectiveJudgeWindows, [&](const auto &candidate) {
            return candidate.context == contexts[contextIndex] &&
                   candidate.judgement == window.judgement;
          });
      if (found == stage.effectiveJudgeWindows.end() ||
          found->earlyMicros != window.earlyMicros ||
          found->lateMicros != window.lateMicros) {
        return false;
      }
    }
  }
  return true;
}

} // namespace

SubmissionEligibilityOutcome
validateBokutachiEligibility(const IrSubmission &submission) noexcept {
  try {
    if (submission.keyMode != 7 && submission.keyMode != 14) {
      return rejected(SubmissionEligibilityReason::UnsupportedKeyMode,
                      "Bokutachi supports BMS 7K and 14K only");
    }

    const auto &provenance = submission.provenance;
    std::string provenanceError;
    if (!serializeValidatedScoreProvenance(provenance, provenanceError)) {
      return rejected(SubmissionEligibilityReason::InvalidSubmission,
                      provenanceError);
    }
    if (provenance.stages.size() > 1 ||
        gaugeProfileIsCourse(provenance.gaugeProfile)) {
      return rejected(SubmissionEligibilityReason::CourseResult,
                      "Course results cannot be submitted to Bokutachi.");
    }
    if (provenance.stages.empty()) {
      return rejected(SubmissionEligibilityReason::InvalidSubmission,
                      "Score provenance has no chart stage.");
    }
    if (provenance.ruleset.id == "beatoraja") {
      return rejected(SubmissionEligibilityReason::RulesetMismatch,
                      "Beatoraja ruleset scores cannot be submitted.");
    }
    if (provenance.ruleset.version <= 0 ||
        provenance.ruleset.id == "legacy-unknown" ||
        provenance.eligibility == ScoreEligibility::LegacyUnverified) {
      return rejected(SubmissionEligibilityReason::UnverifiedProvenance,
                      "Legacy scores cannot be verified for submission.");
    }
    if (provenance.ruleset.id != "lr2") {
      return rejected(SubmissionEligibilityReason::RulesetMismatch,
                      "Only LR2 ruleset scores can be submitted.");
    }
    if (provenance.ruleset != RulesetDescriptor::For(GameplayRuleset::LR2)) {
      return rejected(SubmissionEligibilityReason::UnsupportedRulesetRevision,
                      "This ruleset revision is not supported by Bokutachi.");
    }

    const auto &stage = provenance.stages.front();
    if (!chartHashMatches(submission.chartSha256, stage.chartSha256) ||
        !chartHashMatches(submission.chartMd5, stage.chartMd5)) {
      return rejected(SubmissionEligibilityReason::UnverifiedProvenance,
                      "Score provenance does not match this chart.");
    }
    if (stage.judgeRankSource != JudgeRankSource::Chart ||
        !stage.sourceJudgeRank.has_value() || *stage.sourceJudgeRank < 0 ||
        *stage.sourceJudgeRank > 4) {
      return rejected(SubmissionEligibilityReason::UnverifiedProvenance,
                      "Chart judge rank could not be verified.");
    }
    if (!canonicalJudgeWindows(stage, *stage.sourceJudgeRank)) {
      return rejected(SubmissionEligibilityReason::ModifiedJudgePolicy,
                      "Modified judge windows cannot be submitted.");
    }
    if (stage.candidateSelection != gameplay::CandidateSelectionMode::LR2) {
      return rejected(SubmissionEligibilityReason::ModifiedJudgePolicy,
                      "Modified judge policy cannot be submitted.");
    }
    if (submission.maxScore <= 0 || submission.maxScore % 2 != 0 ||
        stage.totalNotes != submission.maxScore / 2) {
      return rejected(SubmissionEligibilityReason::ModifiedGaugeTotal,
                      "Modified gauge TOTAL cannot be submitted.");
    }
    bms_parser::ChartMeta meta;
    meta.KeyMode = submission.keyMode;
    meta.TotalNotes = stage.totalNotes;
    if (stage.authoredGaugeTotal.has_value()) {
      meta.HasTotal = true;
      meta.Total = *stage.authoredGaugeTotal;
    }
    const double canonicalTotal =
        resolveEffectiveGaugeTotal(GameplayRuleset::LR2, meta);
    if (!std::isfinite(stage.effectiveGaugeTotal) ||
        stage.effectiveGaugeTotal != canonicalTotal) {
      return rejected(SubmissionEligibilityReason::ModifiedGaugeTotal,
                      "Modified gauge TOTAL cannot be submitted.");
    }
    if (provenance.autoPlay || provenance.practice ||
        assist_options::isEnabled(provenance.assistOption) ||
        !provenance.playback.neutral() ||
        provenance.judgeWindowScalePercent != 100 ||
        provenance.startingGaugePercent.has_value()) {
      return rejected(SubmissionEligibilityReason::ModifiedAttempt,
                      "Modified attempts cannot be submitted.");
    }
    if (provenance.eligibility != ScoreEligibility::Verified) {
      return rejected(SubmissionEligibilityReason::UnverifiedProvenance,
                      "Score provenance is not verified.");
    }
    return eligible();
  } catch (...) {
    return rejected(SubmissionEligibilityReason::InvalidSubmission,
                    "Score provenance validation failed.");
  }
}

bool isReplayEligibleForBokutachi(
    std::string_view attemptId, bool hasCanonicalAttemptFingerprint,
    const bms_parser::ChartMeta &meta,
    const ScoreProvenance &provenance) noexcept {
  const auto maximumScore =
      result_contract::maximumScoreForNotes(meta.TotalNotes);
  if (!uuid::isCanonicalLowerV4(attemptId) ||
      !hasCanonicalAttemptFingerprint || meta.TotalNotes <= 0 ||
      !maximumScore) {
    return false;
  }

  IrSubmission probe;
  probe.attemptId = std::string(attemptId);
  probe.keyMode = meta.KeyMode;
  probe.chartMd5 = asobmshow::bms_metadata::normalizedHash(meta.MD5);
  probe.chartSha256 = asobmshow::bms_metadata::normalizedHash(meta.SHA256);
  probe.maxScore = *maximumScore;
  probe.provenance = provenance;
  return validateBokutachiEligibility(probe).eligible();
}

bool shouldShowReplayUploadMarker(
    std::string_view attemptId, bool hasCanonicalAttemptFingerprint,
    const bms_parser::ChartMeta &meta, const ScoreProvenance &provenance,
    std::optional<IrOutboxState> outboxState) noexcept {
  return outboxState != IrOutboxState::Succeeded &&
         isReplayEligibleForBokutachi(attemptId,
                                      hasCanonicalAttemptFingerprint, meta,
                                      provenance);
}

} // namespace ir::tachi
