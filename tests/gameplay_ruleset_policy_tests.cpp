#include "CoursePlaySession.h"
#include "scene/play/GamePlayStartOptions.h"
#include "scene/play/GameplayRulesetPolicy.h"

#include "bms_parser.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bms_parser::ChartMeta chartMeta(GameplayRuleset ruleset) {
  bms_parser::ChartMeta meta;
  meta.MD5 = ruleset == GameplayRuleset::LR2 ? "lr2-md5" : "beatoraja-md5";
  meta.SHA256 = ruleset == GameplayRuleset::LR2 ? "lr2-sha" : "beatoraja-sha";
  meta.TotalNotes = 1000;
  meta.Total = 200.5;
  meta.HasTotal = true;
  meta.KeyMode = 7;
  meta.Rank = 1;
  return meta;
}

ScoreStageProvenance completeReplaySnapshot(const bms_parser::ChartMeta &meta) {
  ScoreStageProvenance result;
  result.chartMd5 = meta.MD5;
  result.chartSha256 = meta.SHA256;
  result.sourceJudgeRank = meta.Rank;
  result.totalNotes = meta.TotalNotes;
  result.authoredGaugeTotal = meta.Total;
  result.effectiveGaugeTotal = 199.0;
  result.candidateSelection = gameplay::CandidateSelectionMode::LR2;
  constexpr std::array contexts{
      gameplay::JudgeWindowContext::Normal,
      gameplay::JudgeWindowContext::Scratch,
      gameplay::JudgeWindowContext::LongNoteTail,
      gameplay::JudgeWindowContext::LongScratchTail,
  };
  for (const auto context : contexts) {
    result.effectiveJudgeWindows.insert(result.effectiveJudgeWindows.end(),
                                        {{context, PGreat, -12'000, 12'000},
                                         {context, Great, -30'000, 30'000},
                                         {context, Good, -60'000, 60'000},
                                         {context, Bad, -200'000, 200'000},
                                         {context, Kpoor, -1'000'000, 0}});
  }
  return result;
}

void testLr2PolicyIsCoherent() {
  const auto meta = chartMeta(GameplayRuleset::LR2);
  const auto outcome = gameplay::buildGameplayRulesetPolicy(
      meta,
      {.ruleset = GameplayRuleset::LR2,
       .gaugeProfile = GaugeProfile::Standard,
       .sourceRank = meta.Rank,
       .beatorajaCandidateSelection = gameplay::CandidateSelectionMode::Score});
  require(outcome.status == gameplay::GameplayPolicyBuildStatus::Built &&
              outcome.policy.has_value(),
          "LR2 policy builds for a valid chart");
  const auto &policy = *outcome.policy;
  require(policy.id == GameplayRuleset::LR2 &&
              policy.descriptor ==
                  RulesetDescriptor::For(GameplayRuleset::LR2) &&
              policy.judge.rules().ruleset == GameplayRuleset::LR2 &&
              policy.judge.rules().candidateSelection ==
                  gameplay::CandidateSelectionMode::LR2 &&
              policy.gauge.ruleset == GameplayRuleset::LR2 &&
              policy.gauge.compiled && policy.gauge.effectiveTotal == 200.0 &&
              policy.canonical,
          "LR2 policy descriptor, judge, candidate, gauge, and TOTAL match");
}

void testBeatorajaPolicyIsCoherent() {
  const auto meta = chartMeta(GameplayRuleset::Beatoraja);
  const auto outcome = gameplay::buildGameplayRulesetPolicy(
      meta, {.ruleset = GameplayRuleset::Beatoraja,
             .gaugeProfile = GaugeProfile::Standard,
             .sourceRank = meta.Rank,
             .beatorajaCandidateSelection =
                 gameplay::CandidateSelectionMode::Duration});
  require(outcome.status == gameplay::GameplayPolicyBuildStatus::Built &&
              outcome.policy.has_value(),
          "Beatoraja policy builds for a valid chart");
  const auto &policy = *outcome.policy;
  require(policy.id == GameplayRuleset::Beatoraja &&
              policy.descriptor ==
                  RulesetDescriptor::For(GameplayRuleset::Beatoraja) &&
              policy.judge.rules().ruleset == GameplayRuleset::Beatoraja &&
              policy.judge.rules().candidateSelection ==
                  gameplay::CandidateSelectionMode::Duration &&
              policy.gauge.ruleset == GameplayRuleset::Beatoraja &&
              policy.gauge.compiled &&
              std::abs(policy.gauge.effectiveTotal - 200.5) < 0.0001 &&
              policy.canonical,
          "Beatoraja policy cannot contain LR2 judge or gauge semantics");
}

void testInvalidInputsDoNotFallBack() {
  const auto meta = chartMeta(GameplayRuleset::LR2);
  auto future = RulesetDescriptor::For(GameplayRuleset::LR2);
  future.id = "future-ruleset";
  future.version = RulesetDescriptor::kCurrentVersion + 1;
  const auto unsupported = gameplay::buildGameplayRulesetPolicy(
      meta, {.ruleset = GameplayRuleset::LR2,
             .gaugeProfile = GaugeProfile::Standard,
             .sourceRank = meta.Rank,
             .requiredDescriptor = future});
  require(unsupported.status ==
                  gameplay::GameplayPolicyBuildStatus::UnsupportedRuleset &&
              !unsupported.policy.has_value(),
          "an unsupported replay descriptor fails without fallback");

  bms_parser::ChartMeta noNotes = meta;
  noNotes.TotalNotes = 0;
  const auto invalidChart = gameplay::buildGameplayRulesetPolicy(
      noNotes, {.ruleset = GameplayRuleset::LR2,
                .gaugeProfile = GaugeProfile::Standard,
                .sourceRank = noNotes.Rank});
  require(invalidChart.status ==
                  gameplay::GameplayPolicyBuildStatus::InvalidChart &&
              !invalidChart.policy.has_value(),
          "a chart without playable notes cannot produce a mixed fallback");

  auto incomplete = completeReplaySnapshot(meta);
  incomplete.effectiveJudgeWindows.pop_back();
  const auto invalidReplay = gameplay::buildGameplayRulesetPolicy(
      meta, {.ruleset = GameplayRuleset::LR2,
             .gaugeProfile = GaugeProfile::Standard,
             .sourceRank = meta.Rank,
             .requiredDescriptor = RulesetDescriptor::For(GameplayRuleset::LR2),
             .replaySnapshot = incomplete});
  require(invalidReplay.status ==
                  gameplay::GameplayPolicyBuildStatus::InvalidReplaySnapshot &&
              !invalidReplay.policy.has_value(),
          "an incomplete replay window snapshot fails without fallback");
}

void testValidatedReplayAndCourseConsistency() {
  const auto meta = chartMeta(GameplayRuleset::LR2);
  const auto snapshot = completeReplaySnapshot(meta);
  const auto replay = gameplay::buildGameplayRulesetPolicy(
      meta, {.ruleset = GameplayRuleset::LR2,
             .gaugeProfile = GaugeProfile::CourseDefault,
             .sourceRank = meta.Rank,
             .courseJudgement = CourseJudgementConstraint::NoGood,
             .requiredDescriptor = RulesetDescriptor::For(GameplayRuleset::LR2),
             .replaySnapshot = snapshot});
  require(replay.status == gameplay::GameplayPolicyBuildStatus::Built &&
              replay.policy.has_value(),
          "a complete bounded replay snapshot builds");
  const auto recordedPGreat = replay.policy->judge.window(PGreat);
  require(recordedPGreat.has_value() &&
              recordedPGreat->earlyMicros == -12'000 &&
              recordedPGreat->lateMicros == 12'000 &&
              replay.policy->gauge.resolvedProfile == GaugeProfile::CourseLR2 &&
              replay.policy->gauge.totalNotes == 1000 &&
              replay.policy->gauge.effectiveTotal == 199.0 &&
              !replay.policy->canonical,
          "replay windows and LR2 course gauge live in one policy");

  StartOptions replayOptions;
  replayOptions.ruleset = GameplayRuleset::LR2;
  const ScoreProvenance captured =
      captureScoreProvenanceAtPlayStart(replayOptions, meta, *replay.policy);
  require(
      captured.eligibility == ScoreEligibility::Modified,
      "a safe noncanonical replay policy remains reproducible but modified");

  CoursePlaySession session;
  session.ruleset = GameplayRuleset::LR2;
  session.rulesetDescriptor = RulesetDescriptor::For(GameplayRuleset::LR2);
  require(gameplay::courseSessionAcceptsPolicy(session, *replay.policy),
          "a course accepts a stage with its snapshotted descriptor");

  const auto otherMeta = chartMeta(GameplayRuleset::Beatoraja);
  const auto other = gameplay::buildGameplayRulesetPolicy(
      otherMeta, {.ruleset = GameplayRuleset::Beatoraja,
                  .gaugeProfile = GaugeProfile::CourseDefault,
                  .sourceRank = otherMeta.Rank});
  require(other.policy.has_value() &&
              !gameplay::courseSessionAcceptsPolicy(session, *other.policy),
          "a course rejects a stage with a different descriptor");
}

void testCanonicalReplaySnapshotStaysCanonical() {
  const auto meta = chartMeta(GameplayRuleset::LR2);
  const auto live = gameplay::buildGameplayRulesetPolicy(
      meta, {.ruleset = GameplayRuleset::LR2,
             .gaugeProfile = GaugeProfile::Standard,
             .sourceRank = meta.Rank});
  require(live.built() && live.policy->canonical,
          "the live LR2 policy is canonical");

  StartOptions options;
  options.ruleset = GameplayRuleset::LR2;
  const ScoreProvenance captured =
      captureScoreProvenanceAtPlayStart(options, meta, *live.policy);
  const auto replay = gameplay::buildGameplayRulesetPolicy(
      meta, {.ruleset = GameplayRuleset::LR2,
             .gaugeProfile = GaugeProfile::Standard,
             .sourceRank = meta.Rank,
             .requiredDescriptor = captured.ruleset,
             .replaySnapshot = captured.stages.front()});
  require(replay.built() && replay.policy->canonical &&
              replay.policy->judge.rules() == live.policy->judge.rules() &&
              replay.policy->gauge == live.policy->gauge,
          "a canonical snapshot reconstructs the exact live policy");
  const ScoreProvenance replayCapture =
      captureScoreProvenanceAtPlayStart(options, meta, *replay.policy);
  require(replayCapture.eligibility == ScoreEligibility::Verified,
          "an exact canonical replay snapshot remains verified");
}

void testReplayStartRebuildsCanonicalPolicyWithoutSnapshot() {
  const auto meta = chartMeta(GameplayRuleset::LR2);
  auto replay = std::make_shared<JudgedPlaybackData>();
  replay->chartMeta = meta;
  replay->context.ruleset = RulesetDescriptor::For(GameplayRuleset::LR2);
  StartOptions options{.replayData = replay};
  applyJudgedPlaybackContextToStartOptions(options, *replay);
  const auto outcome = buildGameplayRulesetPolicyAtPlayStart(
      options, meta, AppSettings::NotePriorityMode::Lowest);
  require(
      outcome.built() && outcome.policy->canonical &&
          outcome.policy->id == GameplayRuleset::LR2,
      "a stock replay setup rebuilds its canonical policy from chart metadata");
}

void testLegacyReplayUsesBeatorajaFallback() {
  auto meta = chartMeta(GameplayRuleset::Beatoraja);
  meta.MD5 = std::string(32, 'b');
  meta.SHA256 = std::string(64, 'a');
  auto replay = std::make_shared<JudgedPlaybackData>();
  replay->chartMeta = meta;
  replay->context.ruleset = RulesetDescriptor::Legacy();
  StartOptions options{.replayData = replay};
  applyJudgedPlaybackContextToStartOptions(options, *replay);

  const auto outcome = buildGameplayRulesetPolicyAtPlayStart(
      options, meta, AppSettings::NotePriorityMode::Lowest);
  require(options.ruleset == GameplayRuleset::Beatoraja &&
              options.requiredRulesetDescriptor ==
                  RulesetDescriptor::For(GameplayRuleset::Beatoraja) &&
              !options.replayRulesetOverride.has_value() && outcome.built() &&
              outcome.policy->id == GameplayRuleset::Beatoraja,
          "a migrated legacy replay remains playable with Beatoraja rules");

  auto legacyProvenanceWithStage = ScoreProvenance::Legacy();
  legacyProvenanceWithStage.stages = {completeReplaySnapshot(meta)};
  replay->context =
      analysis::playbackContextFrom(legacyProvenanceWithStage, meta);
  StartOptions legacyWithStage{.replayData = replay};
  applyJudgedPlaybackContextToStartOptions(legacyWithStage, *replay);
  const auto stagedLegacyOutcome = buildGameplayRulesetPolicyAtPlayStart(
      legacyWithStage, meta, AppSettings::NotePriorityMode::Lowest);
  require(!legacyWithStage.replayRulesetOverride.has_value() &&
              stagedLegacyOutcome.built() &&
              stagedLegacyOutcome.policy->canonical,
          "legacy stage data cannot override the canonical Beatoraja fallback");

  auto session = std::make_shared<CoursePlaySession>();
  session->snapshotRulesetFromReplay(*replay);
  const StartOptions courseOptions =
      makeCourseReplayStageStartOptions(session, replay);
  const auto courseOutcome = buildGameplayRulesetPolicyAtPlayStart(
      courseOptions, meta, AppSettings::NotePriorityMode::Lowest);
  require(session->ruleset == GameplayRuleset::Beatoraja &&
              session->rulesetDescriptor ==
                  RulesetDescriptor::For(GameplayRuleset::Beatoraja) &&
              courseOutcome.built() &&
              courseOutcome.policy->id == GameplayRuleset::Beatoraja,
          "a migrated legacy course replay uses the same Beatoraja fallback");
}

void testRawCourseReplayRestTimingIsPreserved() {
  CoursePlaySession session;
  session.courseReplayPlaybackData =
      std::make_shared<replay::CourseReplayPlaybackData>();
  session.courseReplayPlaybackData->restMicrosAfterStage = {1'500'000, -1};

  session.currentIndex = 0;
  require(session.restMicrosAfterCurrentStage() == 1'500'000,
          "raw course replay returns its recorded inter-stage delay");

  session.currentIndex = 1;
  require(session.restMicrosAfterCurrentStage() == 0,
          "raw course replay clamps a negative inter-stage delay");

  session.courseReplayPlaybackData.reset();
  session.replayStages.resize(1);
  session.replayStages.front().restMicrosAfterStage = 750'000;
  session.currentIndex = 0;
  require(session.restMicrosAfterCurrentStage() == 750'000,
          "freshly recorded course timing remains the fallback");
}
} // namespace

int main() {
  testLr2PolicyIsCoherent();
  testBeatorajaPolicyIsCoherent();
  testInvalidInputsDoNotFallBack();
  testValidatedReplayAndCourseConsistency();
  testCanonicalReplaySnapshotStaysCanonical();
  testReplayStartRebuildsCanonicalPolicyWithoutSnapshot();
  testLegacyReplayUsesBeatorajaFallback();
  testRawCourseReplayRestTimingIsPreserved();
  return 0;
}
