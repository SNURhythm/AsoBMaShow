#include "ir/IrScoreHistoryProjection.h"

#include "scene/play/GameplayGaugeTypes.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr char kShaDigit = 'a';

ir::IrRemoteScore remoteScore(char shaDigit = kShaDigit,
                              char md5Digit = 'b', int score = 180,
                              int lampRank = kClearTypeHardClearRank) {
  return {
      .remoteUserId = 42,
      .game = "bms-7k",
      .remoteScoreId = "remote-score",
      .remoteChartId = "remote-chart",
      .chartMd5 = std::string(32, md5Digit),
      .chartSha256 = std::string(64, shaDigit),
      .title = "Remote title",
      .artist = "Remote artist",
      .service = "Bokutachi",
      .noteCount = 100,
      .score = score,
      .lampRank = lampRank,
      .timeAddedUnixMillis = 1'700'000'000'000LL,
  };
}

void testMd5FallbackIsLookupOnlyAndCannotOverrideConflictingSha() {
  ScoreClearRankCache clearRanks;
  ScoreBestCache bestScores;
  const std::vector remote{remoteScore('a', 'b')};
  ir::projectIrRemoteScores(remote, clearRanks, bestScores);

  bms_parser::ChartMeta md5Only;
  md5Only.MD5 = std::string(32, 'b');
  assert(clearRanks.bestRankFor(md5Only) == kClearTypeHardClearRank);
  const auto fallback = bestScores.bestFor(md5Only);
  assert(fallback.has_value() && fallback->score == 180);

  bms_parser::ChartMeta conflicting;
  conflicting.MD5 = md5Only.MD5;
  conflicting.SHA256 = std::string(64, 'c');
  assert(clearRanks.bestRankFor(conflicting) == kNoClearTypeRank);
  assert(!bestScores.bestFor(conflicting).has_value());

  clearRanks.rankBySha256[conflicting.SHA256].ranks[0] =
      kClearTypeEasyClearRank;
  bestScores.scoreBySha256[conflicting.SHA256].snapshots[0] =
      ScoreBestSnapshot{.score = 120,
                        .maxScore = 200,
                        .clearType = kClearTypeEasyClearRank,
                        .createdAt = "2023-01-01 00:00:00"};
  assert(clearRanks.bestRankFor(conflicting) == kClearTypeEasyClearRank);
  const auto local = bestScores.bestFor(conflicting);
  assert(local.has_value() && local->score == 120 &&
         local->source == ScoreBestSource::Local);
}

void testShaProjectionPopulatesEveryLnBucketWithoutInventingMetrics() {
  ScoreClearRankCache clearRanks;
  ScoreBestCache bestScores;
  const std::vector remote{remoteScore()};

  ir::projectIrRemoteScores(remote, clearRanks, bestScores);

  const std::string sha(64, kShaDigit);
  for (int lnMode = 0; lnMode < 4; ++lnMode) {
    assert(clearRanks.bestRankForHash(sha, lnMode) ==
           kClearTypeHardClearRank);
    const auto best = bestScores.bestForHash(sha, lnMode);
    assert(best.has_value());
    assert(best->score == 180);
    assert(best->maxScore == 200);
    assert(best->clearType == kClearTypeHardClearRank);
    assert(best->source == ScoreBestSource::ImportedIr);
    assert(!best->maxCombo.has_value());
    assert(!best->comboBreak.has_value());
    assert(!best->badPoints.has_value());
    assert(!best->finalGauge.has_value());
    assert(!best->createdAt.has_value());
  }
}

void testScoreComparisonUsesScoreThenLampThenAchievedOrAddedTime() {
  ScoreClearRankCache clearRanks;
  ScoreBestCache bestScores;
  auto achievedEarly = remoteScore('d', 'e', 190, kClearTypeHardClearRank);
  achievedEarly.remoteScoreId = "achieved-early";
  achievedEarly.timeAchievedUnixMillis = 2'000;
  achievedEarly.timeAddedUnixMillis = 10'000;
  achievedEarly.maxCombo = 70;

  auto addedLater = achievedEarly;
  addedLater.remoteScoreId = "added-later";
  addedLater.timeAchievedUnixMillis.reset();
  addedLater.timeAddedUnixMillis = 3'000;
  addedLater.maxCombo = 80;
  addedLater.badPoints = 4;
  addedLater.finalGauge = 88.5F;

  auto strongerLamp = addedLater;
  strongerLamp.remoteScoreId = "stronger-lamp";
  strongerLamp.lampRank = kClearTypeFullComboRank;
  strongerLamp.timeAddedUnixMillis = 1'000;
  strongerLamp.maxCombo = 90;

  auto higherScore = strongerLamp;
  higherScore.remoteScoreId = "higher-score";
  higherScore.score = 191;
  higherScore.lampRank = kClearTypeEasyClearRank;
  higherScore.timeAddedUnixMillis = 500;
  higherScore.maxCombo = 91;

  const std::vector remote{achievedEarly, addedLater, strongerLamp,
                           higherScore};
  ir::projectIrRemoteScores(remote, clearRanks, bestScores);

  const std::string sha(64, 'd');
  const auto best = bestScores.bestForHash(sha, 2);
  assert(best.has_value() && best->score == 191 &&
         best->clearType == kClearTypeEasyClearRank && best->maxCombo == 91);
  assert(clearRanks.bestRankForHash(sha, 2) == kClearTypeFullComboRank);

  ScoreBestCache tiedScores;
  ir::projectIrRemoteScores(std::span{remote}.first(2), clearRanks,
                            tiedScores);
  const auto timeWinner = tiedScores.bestForHash(sha, 3);
  assert(timeWinner.has_value() && timeWinner->maxCombo == 80 &&
         timeWinner->badPoints == 4 && timeWinner->finalGauge == 88.5F &&
         !timeWinner->createdAt.has_value());
}

void testProjectionCannotLowerOrRewriteLocalEvidence() {
  ScoreClearRankCache clearRanks;
  ScoreBestCache bestScores;
  const std::string sha(64, 'a');
  clearRanks.rankBySha256[sha].ranks[0] = kClearTypeFullComboRank;
  clearRanks.rankByCourseKey["course:v1:local"].wildcardRank =
      kClearTypeHardClearRank;
  bestScores.scoreBySha256[sha].snapshots[0] = ScoreBestSnapshot{
      .score = 195,
      .maxScore = 200,
      .maxCombo = 95,
      .comboBreak = 1,
      .badPoints = 2,
      .finalGauge = 90.0F,
      .clearType = kClearTypeFullComboRank,
      .createdAt = "2026-01-01 00:00:00",
  };
  int localPlayCount = 7;
  bool localReplayAvailable = true;
  bool localRulesetEligible = true;
  int localSubmissionState = 3;

  const std::vector remote{remoteScore()};
  ir::projectIrRemoteScores(remote, clearRanks, bestScores);

  const auto best = bestScores.bestForHash(sha, 0);
  assert(best.has_value() && best->score == 195 &&
         best->source == ScoreBestSource::Local);
  assert(clearRanks.bestRankForHash(sha, 0) == kClearTypeFullComboRank);
  assert(clearRanks.rankBySha256.at(sha).ranks[0] ==
         kClearTypeFullComboRank);
  assert(bestScores.scoreBySha256.at(sha).snapshots[0]->score == 195);
  assert(clearRanks.rankByCourseKey.at("course:v1:local").wildcardRank ==
         kClearTypeHardClearRank);
  assert(localPlayCount == 7 && localReplayAvailable &&
         localRulesetEligible && localSubmissionState == 3);
}

} // namespace

int main() {
  testShaProjectionPopulatesEveryLnBucketWithoutInventingMetrics();
  testMd5FallbackIsLookupOnlyAndCannotOverrideConflictingSha();
  testScoreComparisonUsesScoreThenLampThenAchievedOrAddedTime();
  testProjectionCannotLowerOrRewriteLocalEvidence();
  std::cout << "IR score history projection tests passed\n";
  return 0;
}
