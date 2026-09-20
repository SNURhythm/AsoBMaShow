#include "CoursePlaySession.h"
#include "scene/play/GamePlayStartOptions.h"
#include "scene/play/GameplayRulesetPolicy.h"

#include "bms_parser.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

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

ScoreStageProvenance completeReplaySnapshot(
    const bms_parser::ChartMeta &meta) {
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
    result.effectiveJudgeWindows.insert(
        result.effectiveJudgeWindows.end(),
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
      meta, {.ruleset = GameplayRuleset::LR2,
             .gaugeProfile = GaugeProfile::Standard,
             .sourceRank = meta.Rank,
             .beatorajaCandidateSelection =
                 gameplay::CandidateSelectionMode::Score});
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

void testNonpositiveTotalBuildsAndReplays() {
  for (const double total : {0.0, -1.0}) {
    auto meta = chartMeta(GameplayRuleset::Beatoraja);
    meta.Total = total;
    const auto live = gameplay::buildGameplayRulesetPolicy(
        meta, {.ruleset = GameplayRuleset::Beatoraja, .sourceRank = meta.Rank});
    require(live.built() && live.policy->canonical &&
                std::abs(live.policy->gauge.effectiveTotal -
                         460.9090909090909) < 0.0001,
            "nonpositive authored TOTAL builds a canonical default gauge");
    StartOptions options;
    options.ruleset = GameplayRuleset::Beatoraja;
    const auto captured = captureScoreProvenanceAtPlayStart(
        options, meta, *live.policy);
    require(captured.stages.front().authoredGaugeTotal == total,
            "default gauge resolution preserves authored TOTAL provenance");
    const auto replay = gameplay::buildGameplayRulesetPolicy(
        meta, {.ruleset = GameplayRuleset::Beatoraja,
               .sourceRank = meta.Rank,
               .replaySnapshot = captured.stages.front()});
    require(replay.built() && replay.policy->canonical &&
                replay.policy->gauge == live.policy->gauge,
            "default TOTAL is reproducible from the captured replay policy");
  }

  std::atomic_bool cancelled{false};
  const std::string source = "#BPM 120\n#TOTAL 0\n#00111:01\n";
  bms_parser::Parser parser;
  bms_parser::Chart *parsed = nullptr;
  parser.Parse(std::vector<unsigned char>(source.begin(), source.end()),
               &parsed, false, false, cancelled);
  const std::unique_ptr<bms_parser::Chart> chart(parsed);
  require(chart && chart->Meta.TotalNotes == 1 && !chart->Meta.HasTotal,
          "the BMS parser treats TOTAL zero as unspecified");
  const auto parsedPolicy = gameplay::buildGameplayRulesetPolicy(
      chart->Meta, {.ruleset = GameplayRuleset::Beatoraja,
                    .sourceRank = chart->Meta.Rank});
  require(parsedPolicy.built() &&
              parsedPolicy.policy->gauge.effectiveTotal == 260.0,
          "a parsed TOTAL zero chart can start with the default gauge");

  for (const double total : {std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
    auto meta = chartMeta(GameplayRuleset::Beatoraja);
    meta.Total = total;
    const auto outcome = gameplay::buildGameplayRulesetPolicy(
        meta, {.ruleset = GameplayRuleset::Beatoraja});
    require(outcome.status == gameplay::GameplayPolicyBuildStatus::InvalidChart,
            "nonfinite TOTAL remains an invalid chart policy");
  }
}

void testLr2FractionalTotalStartsAndPersists() {
  for (const std::string total : {"0.001", "0.5", "0.999"}) {
    const std::string source = "#BPM 120\n#TOTAL " + total + "\n#00111:01\n";
    bms_parser::Parser parser;
    std::atomic_bool cancelled{false};
    bms_parser::Chart *parsed = nullptr;
    parser.Parse(std::vector<unsigned char>(source.begin(), source.end()),
                 &parsed, false, false, cancelled);
    const std::unique_ptr<bms_parser::Chart> chart(parsed);
    require(chart && chart->Meta.HasTotal && chart->Meta.Total > 0.0 &&
                chart->Meta.Total < 1.0,
            "BMS parsing preserves a positive fractional TOTAL");
    StartOptions options;
    options.ruleset = GameplayRuleset::LR2;
    const auto live = buildGameplayRulesetPolicyAtPlayStart(
        options, *chart, AppSettings::NotePriorityMode::Lowest);
    require(live.built() && live.policy->canonical &&
                live.policy->gauge.effectiveTotal == 0.0,
            "LR2 starts with a fractional TOTAL rounded down to zero");
    require(live.policy->gauge.delta(GaugeType::Normal, PGreat, 20.0F) == 0.0F &&
                live.policy->gauge.delta(GaugeType::Normal, Poor, 20.0F) < 0.0F,
            "zero LR2 TOTAL disables groove recovery but retains damage");
    const auto captured = captureScoreProvenanceAtPlayStart(
        options, chart->Meta, *live.policy);
    std::string error;
    const auto serialized = serializeValidatedScoreProvenance(captured, error);
    require(serialized.has_value() && error.empty(),
            "zero effective TOTAL can be saved with score and replay provenance");
    const auto restored = deserializeScoreProvenance(*serialized, error);
    require(restored.has_value() && error.empty() && *restored == captured,
            "zero effective TOTAL round-trips without losing authored TOTAL");
    auto replayData = std::make_shared<ReplayData>();
    replayData->chartMeta = chart->Meta;
    replayData->provenance = *restored;
    StartOptions replayOptions{.replayData = replayData};
    applyReplayProvenanceToStartOptions(replayOptions, *replayData);
    const auto replay = buildGameplayRulesetPolicyAtPlayStart(
        replayOptions, *chart, AppSettings::NotePriorityMode::Lowest);
    require(replay.built() && replay.policy->canonical &&
                replay.policy->gauge == live.policy->gauge,
            "saved zero TOTAL replay uses the original LR2 gauge policy");

    auto invalid = restored->stages.front();
    invalid.effectiveGaugeTotal = -1.0;
    const auto rejected = gameplay::buildGameplayRulesetPolicy(
        chart->Meta, {.ruleset = GameplayRuleset::LR2,
                      .sourceRank = chart->Meta.Rank,
                      .replaySnapshot = invalid});
    require(rejected.status == gameplay::GameplayPolicyBuildStatus::InvalidReplaySnapshot,
            "negative effective TOTAL is still rejected in replay policies");
  }
}

void testBeatorajaRejectsZeroReplayTotal() {
  auto meta = chartMeta(GameplayRuleset::Beatoraja);
  meta.MD5 = std::string(32, 'b');
  meta.SHA256 = std::string(64, 'a');
  const auto live = gameplay::buildGameplayRulesetPolicy(
      meta, {.ruleset = GameplayRuleset::Beatoraja,
             .sourceRank = meta.Rank});
  require(live.built() && live.policy->gauge.effectiveTotal == 200.5,
          "Beatoraja has positive canonical gauge recovery");
  StartOptions options;
  options.ruleset = GameplayRuleset::Beatoraja;
  auto replay = std::make_shared<ReplayData>();
  replay->chartMeta = meta;
  replay->provenance = captureScoreProvenanceAtPlayStart(
      options, meta, *live.policy);
  replay->provenance.stages.front().effectiveGaugeTotal = 0.0;
  const auto rejected = gameplay::buildGameplayRulesetPolicy(
      meta, {.ruleset = GameplayRuleset::Beatoraja,
             .sourceRank = meta.Rank,
             .replaySnapshot = replay->provenance.stages.front()});
  require(rejected.status ==
              gameplay::GameplayPolicyBuildStatus::InvalidReplaySnapshot &&
              !rejected.policy.has_value(),
          "Beatoraja rejects a zero TOTAL snapshot instead of disabling recovery");

  StartOptions replayOptions{.replayData = replay};
  applyReplayProvenanceToStartOptions(replayOptions, *replay);
  require(!replayOptions.replayRulesetOverride.has_value(),
          "Beatoraja replay ingestion rejects zero TOTAL with otherwise complete proof");
  const auto start = buildGameplayRulesetPolicyAtPlayStart(
      replayOptions, meta, AppSettings::NotePriorityMode::Lowest);
  require(start.status ==
              gameplay::GameplayPolicyBuildStatus::InvalidReplaySnapshot,
          "Beatoraja cannot start a replay with rejected zero TOTAL proof");
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
             .requiredDescriptor =
                 RulesetDescriptor::For(GameplayRuleset::LR2),
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
             .requiredDescriptor =
                 RulesetDescriptor::For(GameplayRuleset::LR2),
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
  const ScoreProvenance captured = captureScoreProvenanceAtPlayStart(
      replayOptions, meta, *replay.policy);
  require(captured.eligibility == ScoreEligibility::Modified,
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
  const ScoreProvenance captured = captureScoreProvenanceAtPlayStart(
      options, meta, *live.policy);
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
  const ScoreProvenance replayCapture = captureScoreProvenanceAtPlayStart(
      options, meta, *replay.policy);
  require(replayCapture.eligibility == ScoreEligibility::Verified,
          "an exact canonical replay snapshot remains verified");
}

void testReplayStartRequiresValidatedSnapshot() {
  const auto meta = chartMeta(GameplayRuleset::LR2);
  auto replay = std::make_shared<ReplayData>();
  replay->chartMeta = meta;
  replay->provenance.ruleset =
      RulesetDescriptor::For(GameplayRuleset::LR2);
  StartOptions options{.replayData = replay};
  applyReplayProvenanceToStartOptions(options, *replay);
  const auto outcome = buildGameplayRulesetPolicyAtPlayStart(
      options, meta, AppSettings::NotePriorityMode::Lowest);
  require(outcome.status ==
                  gameplay::GameplayPolicyBuildStatus::InvalidReplaySnapshot &&
              !outcome.policy.has_value(),
          "a replay start never falls back when its snapshot is incomplete");
}

void testLegacyReplayUsesBeatorajaFallback() {
  auto meta = chartMeta(GameplayRuleset::Beatoraja);
  meta.MD5 = std::string(32, 'b');
  meta.SHA256 = std::string(64, 'a');
  auto replay = std::make_shared<ReplayData>();
  replay->chartMeta = meta;
  replay->provenance = ScoreProvenance::Legacy();
  StartOptions options{.replayData = replay};
  applyReplayProvenanceToStartOptions(options, *replay);

  const auto outcome = buildGameplayRulesetPolicyAtPlayStart(
      options, meta, AppSettings::NotePriorityMode::Lowest);
  require(options.ruleset == GameplayRuleset::Beatoraja &&
              options.requiredRulesetDescriptor ==
                  RulesetDescriptor::For(GameplayRuleset::Beatoraja) &&
              !options.replayRulesetOverride.has_value() && outcome.built() &&
              outcome.policy->id == GameplayRuleset::Beatoraja,
          "a migrated legacy replay remains playable with Beatoraja rules");

  replay->provenance.stages = {completeReplaySnapshot(meta)};
  StartOptions legacyWithStage{.replayData = replay};
  applyReplayProvenanceToStartOptions(legacyWithStage, *replay);
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
} // namespace

int main() {
  testBeatorajaRejectsZeroReplayTotal();
  testLr2FractionalTotalStartsAndPersists();
  testNonpositiveTotalBuildsAndReplays();
  testLr2PolicyIsCoherent();
  testBeatorajaPolicyIsCoherent();
  testInvalidInputsDoNotFallBack();
  testValidatedReplayAndCourseConsistency();
  testCanonicalReplaySnapshotStaysCanonical();
  testReplayStartRequiresValidatedSnapshot();
  testLegacyReplayUsesBeatorajaFallback();
  return 0;
}
