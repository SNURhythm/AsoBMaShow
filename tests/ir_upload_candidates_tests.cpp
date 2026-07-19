#include "ir/IrUploadCandidates.h"

#include "FileChecksum.h"
#include "ScoreProvenance.h"
#include "scene/play/GameplayGaugeRules.h"
#include "scene/play/GameplayJudgeRules.h"

#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

ChartMetaRecord complete7kChart(std::string path) {
  ChartMetaRecord chart;
  chart.meta.BmsPath = std::move(path);
  chart.meta.KeyMode = 7;
  chart.meta.Rank = 2;
  chart.meta.TotalNotes = 600;
  chart.meta.HasTotal = true;
  chart.meta.Total = 200.5;
  chart.meta.MD5 = repeated('b', 32);
  chart.meta.SHA256 = repeated('a', 64);
  return chart;
}

std::shared_ptr<const ScoreProvenance>
verifiedLr2Provenance(const bms_parser::ChartMeta &chart) {
  const auto judge = gameplay::compileGameplayJudgeRules(
      GameplayRuleset::LR2, chart.Rank);
  return std::make_shared<const ScoreProvenance>(makeScoreProvenance({
      .chartMeta = chart,
      .longNoteMode = 2,
      .judgeRankSource = JudgeRankSource::Chart,
      .sourceJudgeRank = chart.Rank,
      .effectiveJudgeContexts = judge.contexts,
      .totalNotes = chart.TotalNotes,
      .authoredGaugeTotal = chart.Total,
      .effectiveGaugeTotal =
          resolveEffectiveGaugeTotal(GameplayRuleset::LR2, chart),
      .candidateSelection = gameplay::CandidateSelectionMode::LR2,
      .gaugeType = GaugeType::Hard,
      .player1 = {.option = "RANDOM", .seed = 1234},
      .inputDevices = {InputDeviceCategory::Keyboard},
      .ruleset = RulesetDescriptor::For(GameplayRuleset::LR2),
  }));
}

ReplaySummary replay(int id, const ChartMetaRecord &chart) {
  ReplaySummary value;
  value.id = id;
  value.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  value.hasCanonicalAttemptFingerprint = true;
  value.chartMeta = chart.meta;
  value.provenance = verifiedLr2Provenance(chart.meta);
  return value;
}

void testProjectsOnlyCanonicalActionableAttempts() {
  const ChartMetaRecord chart = complete7kChart("library/alpha/chart.bms");
  ReplaySummary eligible = replay(11, chart);
  ReplaySummary failed = replay(12, chart);
  failed.requestedIrOutboxState = ir::IrOutboxState::FailedPermanent;
  ReplaySummary queued = replay(13, chart);
  queued.requestedIrOutboxState = ir::IrOutboxState::Pending;
  ReplaySummary uploaded = replay(14, chart);
  uploaded.hasIrReceipt = true;
  ReplaySummary hidden = replay(15, chart);
  hidden.provenance.reset();
  hidden.irRecordState = ir::IrRecordState::Eligible;
  ReplaySummary duplicateEligible = replay(16, chart);
  duplicateEligible.attemptId = "123e4567-e89b-42d3-a456-426614174001";

  const std::array replays{eligible, failed, queued, uploaded, hidden,
                           duplicateEligible};
  const std::array charts{chart};
  const auto projected = ir::projectIrUploadCandidates(replays, charts);

  expect(projected.candidates.size() == 3,
         "only eligible, failed, and duplicate eligible attempts remain");
  expect(projected.candidates[0].replayId() == eligible.id &&
             projected.candidates[1].replayId() == failed.id &&
             projected.candidates[2].replayId() == duplicateEligible.id,
         "projection preserves newest-first replay order");
  expect(projected.candidates[0].replay.chartMeta->BmsPath == chart.meta.BmsPath &&
             projected.candidates[0].replay.chartMeta->TotalNotes ==
                 chart.meta.TotalNotes,
         "projection hydrates replay chart metadata from the chart record");
  expect(projected.candidates[0].replay.maxScore == chart.meta.TotalNotes * 2,
         "projection derives max score from hydrated chart notes");
  expect(projected.candidates[0].replay.irSubmissionEligible &&
             projected.candidates[0].state == ir::IrRecordState::Eligible,
         "projection reruns the canonical resolver after hydration");
  expect(projected.candidates[1].state == ir::IrRecordState::Failed,
         "permanently failed rows remain retry candidates");

  std::unordered_set<int> selected{eligible.id, queued.id, 99999};
  ir::intersectIrUploadSelection(selected, projected.candidates);
  expect(selected == std::unordered_set<int>{eligible.id},
         "refresh retains only still-published replay IDs");
}

void testFailsClosedForAmbiguousAndInvalidHydration() {
  const ChartMetaRecord chart = complete7kChart("private/chart-path.bms");
  ChartMetaRecord duplicatePath = chart;
  ChartMetaRecord invalidNotes = complete7kChart("private/invalid.bms");
  invalidNotes.meta.TotalNotes = std::numeric_limits<int>::max();
  ChartMetaRecord ineligibleChart =
      complete7kChart("private/ineligible.bms");

  ReplaySummary missingPath = replay(21, chart);
  missingPath.chartMeta->BmsPath = "private/missing.bms";
  ReplaySummary ambiguousPath = replay(22, chart);
  ReplaySummary mismatchedHash = replay(23, chart);
  mismatchedHash.chartMeta->SHA256 = repeated('c', 64);
  ReplaySummary invalidMaxScore = replay(24, invalidNotes);
  ReplaySummary prelabelledButIneligible = replay(25, ineligibleChart);
  ScoreProvenance modified = *prelabelledButIneligible.provenance;
  modified.eligibility = ScoreEligibility::Modified;
  prelabelledButIneligible.provenance =
      std::make_shared<const ScoreProvenance>(std::move(modified));
  prelabelledButIneligible.irRecordState = ir::IrRecordState::Eligible;

  const std::array replays{missingPath, ambiguousPath, mismatchedHash,
                           invalidMaxScore, prelabelledButIneligible};
  const std::array charts{chart, duplicatePath, invalidNotes, ineligibleChart};
  const auto projected = ir::projectIrUploadCandidates(replays, charts);

  expect(projected.candidates.empty(),
         "missing, ambiguous, invalid, and ineligible rows fail closed");
  expect(projected.omittedRows == 4,
         "hydration failures and invalid score bounds are counted once each");
  expect(!projected.diagnostic.empty() &&
             projected.diagnostic.size() <= ir::kMaximumDiagnosticBytes,
         "omissions produce one bounded diagnostic");
  expect(projected.diagnostic.find("private/chart-path.bms") ==
             std::string::npos &&
             projected.diagnostic.find("RANDOM") == std::string::npos,
         "projection diagnostic excludes filesystem paths and provenance data");
}

} // namespace

int main() {
  testProjectsOnlyCanonicalActionableAttempts();
  testFailsClosedForAmbiguousAndInvalidHydration();
  return failures == 0 ? 0 : 1;
}
