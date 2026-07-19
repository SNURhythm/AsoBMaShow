#include "ir/IrScoreHistoryProjection.h"

#include "scene/play/GameplayGaugeTypes.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
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

ChartMetaRecord chartRecord(std::string title, std::string path,
                            char shaDigit, char md5Digit) {
  ChartMetaRecord record;
  record.meta.Title = std::move(title);
  record.meta.BmsPath = std::move(path);
  if (shaDigit != '\0') {
    record.meta.SHA256 = std::string(64, shaDigit);
  }
  if (md5Digit != '\0') {
    record.meta.MD5 = std::string(32, md5Digit);
  }
  return record;
}

std::vector<std::string>
pathsFor(const std::vector<ChartMetaRecord> &records) {
  std::vector<std::string> paths;
  paths.reserve(records.size());
  for (const auto &record : records) {
    paths.push_back(record.meta.BmsPath.string());
  }
  return paths;
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

void testMd5OnlyFallbackAllowsMultipleRowsForOneRemoteSha() {
  ScoreClearRankCache clearRanks;
  ScoreBestCache bestScores;
  auto strongerLamp =
      remoteScore('d', 'e', 170, kClearTypeFullComboRank);
  strongerLamp.remoteScoreId = "same-sha-stronger-lamp";
  auto strongerScore =
      remoteScore('d', 'e', 190, kClearTypeHardClearRank);
  strongerScore.remoteScoreId = "same-sha-stronger-score";
  ir::projectIrRemoteScores(std::vector{strongerLamp, strongerScore},
                            clearRanks, bestScores);

  bms_parser::ChartMeta md5Only;
  md5Only.MD5 = std::string(32, 'e');
  assert(clearRanks.bestRankFor(md5Only) == kClearTypeFullComboRank);
  const auto best = bestScores.bestFor(md5Only);
  assert(best.has_value() && best->score == 190 &&
         best->clearType == kClearTypeHardClearRank &&
         best->source == ScoreBestSource::ImportedIr);
}

void testMd5OnlyFallbackRejectsConflictingRemoteShaIdentities() {
  ScoreClearRankCache clearRanks;
  ScoreBestCache bestScores;
  clearRanks.rankBySha256[""].ranks[0] = kClearTypeEasyClearRank;
  bestScores.scoreBySha256[""].snapshots[0] =
      ScoreBestSnapshot{.score = 120,
                        .maxScore = 200,
                        .clearType = kClearTypeEasyClearRank,
                        .createdAt = "2023-01-01 00:00:00"};

  auto compatible = remoteScore('a', 'f', 170, kClearTypeHardClearRank);
  compatible.remoteScoreId = "compatible-sha";
  auto conflicting =
      remoteScore('c', 'f', 190, kClearTypeFullComboRank);
  conflicting.remoteScoreId = "conflicting-sha";
  ir::projectIrRemoteScores(std::vector{compatible, conflicting}, clearRanks,
                            bestScores);

  bms_parser::ChartMeta md5Only;
  md5Only.MD5 = std::string(32, 'f');
  assert(clearRanks.bestRankFor(md5Only) == kClearTypeEasyClearRank);
  const auto localOnly = bestScores.bestFor(md5Only);
  assert(localOnly.has_value() && localOnly->score == 120 &&
         localOnly->source == ScoreBestSource::Local);

  bms_parser::ChartMeta knownSha = md5Only;
  knownSha.SHA256 = std::string(64, 'a');
  assert(clearRanks.bestRankFor(knownSha) == kClearTypeHardClearRank);
  const auto compatibleOnly = bestScores.bestFor(knownSha);
  assert(compatibleOnly.has_value() && compatibleOnly->score == 170 &&
         compatibleOnly->clearType == kClearTypeHardClearRank &&
         compatibleOnly->source == ScoreBestSource::ImportedIr);
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

void testProjectedMembershipUsesDisplayedClearAndScoreRanks() {
  ScoreClearRankCache clearRanks;
  ScoreBestCache bestScores;
  ir::projectIrRemoteScores(std::vector{remoteScore()}, clearRanks,
                            bestScores);
  const std::vector base{
      chartRecord("Remote", "remote.bms", 'a', 'b'),
      chartRecord("No play", "no-play.bms", 'c', 'd'),
  };

  ChartMetaQuery query;
  query.clearMarkFilter = true;
  query.clearMarkRank = kClearTypeHardClearRank;
  assert(ir::chartMetaQueryUsesProjectedScores(query));
  auto records = base;
  ir::applyProjectedScoreQuery(query, clearRanks, bestScores, records);
  assert(pathsFor(records) == std::vector<std::string>{"remote.bms"});

  query.clearMarkRank = kClearTypeNormalClearRank;
  query.clearMarkOrAbove = true;
  records = base;
  ir::applyProjectedScoreQuery(query, clearRanks, bestScores, records);
  assert(pathsFor(records) == std::vector<std::string>{"remote.bms"});

  query = {};
  query.scoreRank = "AAA";
  records = base;
  ir::applyProjectedScoreQuery(query, clearRanks, bestScores, records);
  assert(pathsFor(records) == std::vector<std::string>{"remote.bms"});

  query.scoreRank = "AA";
  query.scoreRankOrAbove = true;
  records = base;
  ir::applyProjectedScoreQuery(query, clearRanks, bestScores, records);
  assert(pathsFor(records) == std::vector<std::string>{"remote.bms"});

  query.scoreRank = "MAX";
  query.scoreRankOrAbove = false;
  query.scoreRankOrBelow = true;
  records = base;
  ir::applyProjectedScoreQuery(query, clearRanks, bestScores, records);
  assert(pathsFor(records) == std::vector<std::string>{"remote.bms"});
}

void testProjectedClearAndScoreSortMatchDisplayedValues() {
  ScoreClearRankCache clearRanks;
  ScoreBestCache bestScores;
  ir::projectIrRemoteScores(std::vector{remoteScore()}, clearRanks,
                            bestScores);
  const std::string localSha(64, 'c');
  clearRanks.rankBySha256[localSha].ranks[0] = kClearTypeFullComboRank;
  bestScores.scoreBySha256[localSha].snapshots[0] = ScoreBestSnapshot{
      .score = 195,
      .maxScore = 200,
      .maxCombo = 90,
      .clearType = kClearTypeFullComboRank,
  };
  const std::vector base{
      chartRecord("No play", "no-play.bms", 'e', 'f'),
      chartRecord("Remote", "remote.bms", 'a', 'b'),
      chartRecord("Local", "local.bms", 'c', 'd'),
  };

  ChartMetaQuery query;
  query.sortCriterion = ChartRecordSortCriterion::ClearMark;
  query.sortDirection = ChartRecordSortDirection::Descending;
  auto records = base;
  ir::applyProjectedScoreQuery(query, clearRanks, bestScores, records);
  assert(pathsFor(records) ==
         std::vector<std::string>({"local.bms", "remote.bms",
                                   "no-play.bms"}));

  query.sortCriterion = ChartRecordSortCriterion::Score;
  records = base;
  ir::applyProjectedScoreQuery(query, clearRanks, bestScores, records);
  assert(pathsFor(records) ==
         std::vector<std::string>({"local.bms", "remote.bms",
                                   "no-play.bms"}));

  query.sortDirection = ChartRecordSortDirection::Ascending;
  records = base;
  ir::applyProjectedScoreQuery(query, clearRanks, bestScores, records);
  assert(pathsFor(records) ==
         std::vector<std::string>({"remote.bms", "local.bms",
                                   "no-play.bms"}));
}

void testProjectedMd5FallbackMatchesConflictSafeRowLookup() {
  ScoreClearRankCache clearRanks;
  ScoreBestCache bestScores;
  ir::projectIrRemoteScores(std::vector{remoteScore('a', 'b')}, clearRanks,
                            bestScores);
  ChartMetaQuery hardQuery;
  hardQuery.clearMarkFilter = true;
  hardQuery.clearMarkRank = kClearTypeHardClearRank;

  std::vector records{chartRecord("MD5 only", "md5-only.bms", '\0', 'b')};
  ir::applyProjectedScoreQuery(hardQuery, clearRanks, bestScores, records);
  assert(pathsFor(records) == std::vector<std::string>{"md5-only.bms"});

  auto collision = remoteScore('c', 'b');
  collision.remoteScoreId = "collision";
  ir::projectIrRemoteScores(std::vector{collision}, clearRanks, bestScores);
  records = {chartRecord("Ambiguous MD5", "ambiguous.bms", '\0', 'b')};
  ir::applyProjectedScoreQuery(hardQuery, clearRanks, bestScores, records);
  assert(records.empty());
}

void testProjectedQueryKeepsNonScoreConstraintsAndStablePathLookup() {
  ChartMetaQuery query;
  query.keyword = "artist";
  query.tableId = 3;
  query.tableLevel = "12";
  query.bpmMin = 120.0;
  query.favoritesOnly = true;
  query.clearMarkFilter = true;
  query.clearMarkRank = kClearTypeHardClearRank;
  query.scoreRank = "AAA";
  query.sortCriterion = ChartRecordSortCriterion::Score;
  query.limit = 32;
  query.offset = 64;
  const auto baseQuery = ir::chartMetaQueryWithoutProjectedScoreCriteria(query);
  assert(baseQuery.keyword == query.keyword &&
         baseQuery.tableId == query.tableId &&
         baseQuery.tableLevel == query.tableLevel &&
         baseQuery.bpmMin == query.bpmMin && baseQuery.favoritesOnly &&
         !baseQuery.clearMarkFilter && !baseQuery.scoreRank.has_value() &&
         baseQuery.sortCriterion == ChartRecordSortCriterion::Default &&
         baseQuery.limit == 0 && baseQuery.offset == 0);

  ChartMetaQuery courseDefault;
  courseDefault.courseId = 7;
  assert(!ir::chartMetaQueryUsesProjectedScores(courseDefault));
  ChartMetaQuery titleSort;
  titleSort.sortCriterion = ChartRecordSortCriterion::Title;
  assert(!ir::chartMetaQueryUsesProjectedScores(titleSort));

  const std::vector owned{
      chartRecord("Third", "third.bms", 'c', 'c'),
      chartRecord("First", "first.bms", 'a', 'a'),
      chartRecord("Second", "second.bms", 'b', 'b'),
  };
  assert(ir::findProjectedChartPathIndex(owned, "first.bms") == 1);
  assert(ir::findProjectedChartPathIndex(owned, "third.bms") == 0);
  assert(ir::findProjectedChartPathIndex(owned, "missing.bms") == -1);
}

} // namespace

int main() {
  testShaProjectionPopulatesEveryLnBucketWithoutInventingMetrics();
  testMd5FallbackIsLookupOnlyAndCannotOverrideConflictingSha();
  testMd5OnlyFallbackAllowsMultipleRowsForOneRemoteSha();
  testMd5OnlyFallbackRejectsConflictingRemoteShaIdentities();
  testScoreComparisonUsesScoreThenLampThenAchievedOrAddedTime();
  testProjectionCannotLowerOrRewriteLocalEvidence();
  testProjectedMembershipUsesDisplayedClearAndScoreRanks();
  testProjectedClearAndScoreSortMatchDisplayedValues();
  testProjectedMd5FallbackMatchesConflictSafeRowLookup();
  testProjectedQueryKeepsNonScoreConstraintsAndStablePathLookup();
  std::cout << "IR score history projection tests passed\n";
  return 0;
}
