#include "TachiBatchManual.h"

#include "../../FileChecksum.h"
#include "../../Uuid.h"
#include "../../scene/play/GameplayGaugeRules.h"
#include "../../scene/play/GameplayJudgeRules.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>

namespace ir::tachi {
namespace {

bool isLowerHexDigest(std::string_view value, std::size_t expectedSize) {
  return value.size() == expectedSize &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

std::optional<std::string_view> lampForClearRank(int clearType) {
  switch (clearType) {
  case kClearTypeFailedRank:
    return "FAILED";
  case kClearTypeAssistedEasyClearRank:
  case kClearTypeLightAssistedEasyClearRank:
    return "ASSIST CLEAR";
  case kClearTypeEasyClearRank:
    return "EASY CLEAR";
  case kClearTypeNormalClearRank:
    return "CLEAR";
  case kClearTypeHardClearRank:
    return "HARD CLEAR";
  case kClearTypeExHardClearRank:
    return "EX HARD CLEAR";
  case kClearTypeFullComboRank:
    return "FULL COMBO";
  default:
    return std::nullopt;
  }
}

BuildDraftOutcome invalid(std::string_view diagnostic) {
  return {.status = BuildDraftStatus::Invalid,
          .reason = SubmissionEligibilityReason::InvalidSubmission,
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

BuildDraftOutcome ineligible(const SubmissionEligibilityOutcome &eligibility) {
  return {
      .status = eligibility.reason ==
                        SubmissionEligibilityReason::InvalidSubmission
                    ? BuildDraftStatus::Invalid
                    : BuildDraftStatus::Unsupported,
      .reason = eligibility.reason,
      .diagnostic = sanitizeDiagnostic(eligibility.diagnostic),
  };
}

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

std::string validationFingerprint(const IrSubmission &submission,
                                  std::string_view payload) {
  const auto &ruleset = submission.provenance.ruleset;
  std::string input = "tachi-lr2-proof-v1\n";
  input += std::to_string(ruleset.id.size()) + ":" + ruleset.id + "\n";
  input += std::to_string(ruleset.version) + "\n";
  input += std::to_string(submission.attemptId.size()) + ":" +
           submission.attemptId + "\n";
  input += std::to_string(submission.chartSha256.size()) + ":" +
           submission.chartSha256 + "\n";
  input += std::to_string(payload.size()) + ":";
  input.append(payload);
  return file_checksum::sha256(input);
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
    if (provenance.ruleset !=
        RulesetDescriptor::For(GameplayRuleset::LR2)) {
      return rejected(
          SubmissionEligibilityReason::UnsupportedRulesetRevision,
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

BuildDraftOutcome
buildBatchManualDraft(const IrSubmission &submission) noexcept {
  try {
    if (submission.keyMode != 7 && submission.keyMode != 14) {
      return ineligible(validateBokutachiEligibility(submission));
    }
    if (!uuid::isCanonicalLowerV4(submission.attemptId)) {
      return invalid("submission attempt ID is malformed");
    }
    const bool hasSha256 = !submission.chartSha256.empty();
    const bool hasMd5 = !submission.chartMd5.empty();
    if ((hasSha256 &&
         !isLowerHexDigest(submission.chartSha256, 64)) ||
        (hasMd5 && !isLowerHexDigest(submission.chartMd5, 32)) ||
        (!hasSha256 && !hasMd5)) {
      return invalid("submission chart hash is malformed");
    }

    const std::array counts{
        submission.maxCombo, submission.comboBreak, submission.pGreat,
        submission.great,    submission.good,       submission.bad,
        submission.poor,     submission.kPoor,      submission.fast,
        submission.slow,
    };
    if (std::ranges::any_of(counts, [](int value) { return value < 0; })) {
      return invalid("submission counters must not be negative");
    }
    if (submission.maxScore <= 0 || submission.score < 0 ||
        submission.score > submission.maxScore ||
        submission.maxScore % 2 != 0 ||
        submission.maxCombo > submission.maxScore / 2 ||
        submission.playedAtUnixMillis <= 0) {
      return invalid("submission score range or timestamp is invalid");
    }
    const long long expectedEx =
        static_cast<long long>(submission.pGreat) * 2LL + submission.great;
    if (expectedEx != submission.score) {
      return invalid("submission EX score disagrees with judgements");
    }
    const long long badPoints =
        static_cast<long long>(submission.bad) + submission.poor;
    if (badPoints > std::numeric_limits<int>::max()) {
      return invalid("submission BP exceeds the supported range");
    }
    if (!std::isfinite(submission.finalGauge)) {
      return invalid("submission gauge is not finite");
    }
    const auto lamp = lampForClearRank(submission.clearType);
    if (!lamp.has_value()) {
      return invalid("submission clear rank is unknown");
    }
    const auto eligibility = validateBokutachiEligibility(submission);
    if (!eligibility.eligible()) {
      return ineligible(eligibility);
    }

    const std::string &identifier =
        hasSha256 ? submission.chartSha256 : submission.chartMd5;
    nlohmann::json score = {
        {"score", submission.score},
        {"lamp", *lamp},
        {"matchType", "bmsChartHash"},
        {"identifier", identifier},
        {"timeAchieved", submission.playedAtUnixMillis},
        {"judgements",
         {{"pgreat", submission.pGreat},
          {"great", submission.great},
          {"good", submission.good},
          {"bad", submission.bad},
          {"poor", submission.poor}}},
        {"optional",
         {{"fast", submission.fast},
          {"slow", submission.slow},
          {"maxCombo", submission.maxCombo},
          {"bp", static_cast<int>(badPoints)},
          {"gauge", std::clamp(submission.finalGauge, 0.0F, 100.0F)}}},
    };
    nlohmann::json document = {
        {"meta",
         {{"game", "bms"},
          {"playtype", submission.keyMode == 7 ? "7K" : "14K"},
          {"service", "AsoBMaShow"}}},
        {"scores", nlohmann::json::array({std::move(score)})},
    };
    std::string payload = document.dump();
    if (payload.size() > kMaximumPayloadBytes) {
      return invalid("submission payload exceeds the provider size limit");
    }

    const IrRulesetProof proof{
        .rulesetId = submission.provenance.ruleset.id,
        .rulesetRevision = submission.provenance.ruleset.version,
        .validationFingerprint =
            validationFingerprint(submission, payload),
    };

    return {
        .status = BuildDraftStatus::Built,
        .reason = SubmissionEligibilityReason::Eligible,
        .draft = IrOutboxDraft{
            .providerId = std::string(kProviderId),
            .attemptId = submission.attemptId,
            .chartMd5 = submission.chartMd5,
            .chartSha256 = submission.chartSha256,
            .payloadJson = std::move(payload),
            .rulesetProof = proof,
            .createdAtUnixMillis = submission.playedAtUnixMillis,
        },
    };
  } catch (...) {
    return invalid("Tachi payload construction failed");
  }
}

} // namespace ir::tachi
