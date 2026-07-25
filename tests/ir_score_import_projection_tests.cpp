#include "ir/IrRemoteScoreModels.h"
#include "repositories/ReplayRepository.h"
#include "repositories/ScoreRepository.h"
#include "sqlite3.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::string_view kProvider = "tachi";
constexpr std::string_view kOrigin = "https://boku.tachi.ac";

std::filesystem::path testRoot(std::string_view name) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("asobmashow-ir-score-import-" + std::string(name));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  return root;
}

ir::IrRemoteScore remoteScore(std::string id, char shaDigit, int score,
                              int lampRank) {
  return {
      .remoteUserId = 42,
      .game = "bms-7k",
      .remoteScoreId = std::move(id),
      .remoteChartId = "remote-chart",
      .chartMd5 = std::string(32, shaDigit),
      .chartSha256 = std::string(64, shaDigit),
      .title = "Remote title",
      .artist = "Remote artist",
      .service = "Bokutachi",
      .noteCount = 100,
      .score = score,
      .lampRank = lampRank,
      .timeAddedUnixMillis = 1'700'000'000'000LL + score,
  };
}

int queryInt(const std::filesystem::path &path, const std::string &query) {
  sqlite3 *database = nullptr;
  assert(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK);
  sqlite3_stmt *statement = nullptr;
  assert(sqlite3_prepare_v2(database, query.c_str(), -1, &statement, nullptr) ==
         SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_ROW);
  const int value = sqlite3_column_int(statement, 0);
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return value;
}

void testImportedSnapshotUsesNormalScoreSummaries() {
  const auto root = testRoot("summaries");
  const auto path = root / "score.db";
  ScoreRepository scores(path);
  assert(scores.EnsureSchema());

  auto scoreBest = remoteScore("score-best", 'a', 190,
                               kClearTypeNormalClearRank);
  scoreBest.maxCombo = 90;
  scoreBest.badPoints = 3;
  scoreBest.finalGauge = 88.5F;
  auto lampBest = remoteScore("lamp-best", 'a', 180,
                              kClearTypeExHardClearRank);
  const std::vector remote{scoreBest, lampBest};

  const auto applied = scores.ReplaceImportedIrScores(
      kProvider, kOrigin, 1'700'000'100'000LL, remote);
  assert(applied.status == ImportedIrScoreProjectionStatus::Applied);
  assert(applied.projectedScores == 2);
  assert(scores.ImportedIrScoresAreCurrent(
      kProvider, kOrigin, 1'700'000'100'000LL, remote.size()));

  const std::string sha(64, 'a');
  const auto clearRanks = scores.LoadBestClearRanks();
  const auto localClearRanks = scores.LoadLocalBestClearRanks();
  const auto bestScores = scores.LoadBestScores();
  for (int lnMode = 0; lnMode < 4; ++lnMode) {
    assert(clearRanks.bestRankForHash(sha, lnMode) ==
           kClearTypeExHardClearRank);
    const auto best = bestScores.bestForHash(sha, lnMode);
    assert(best.has_value() && best->score == 190 &&
           best->clearType == kClearTypeNormalClearRank);
    assert(localClearRanks.bestRankForHash(sha, lnMode) ==
           kNoClearTypeRank);
  }

  bms_parser::ChartMeta meta;
  meta.SHA256 = sha;
  meta.MD5 = std::string(32, 'a');
  meta.TotalNotes = 100;
  const auto best = scores.LoadBestScore(meta);
  assert(best.has_value() && best->score == 190 &&
         best->source == ScoreBestSource::ImportedIr);
  assert(!best->maxCombo.has_value() && !best->comboBreak.has_value() &&
         !best->badPoints.has_value() && !best->finalGauge.has_value());

  assert(queryInt(path, "SELECT COUNT(*) FROM scores WHERE score_source=1") ==
         2);
  assert(queryInt(path,
                  "SELECT COUNT(*) FROM score_sha256_clear_rank_cache WHERE "
                  "chart_sha256='" +
                      sha + "'") == 4);
}

void testRemoteLampIsAuthoritative() {
  const auto root = testRoot("authoritative-lamp");
  const auto path = root / "score.db";
  ScoreRepository scores(path);
  assert(scores.EnsureSchema());

  const std::vector remote{
      remoteScore("normal-clear", 'b', 170, kClearTypeNormalClearRank)};
  const auto applied =
      scores.ReplaceImportedIrScores(kProvider, kOrigin, 10, remote);
  assert(applied.status == ImportedIrScoreProjectionStatus::Applied);
  const auto ranks = scores.LoadBestClearRanks();
  for (int lnMode = 0; lnMode < 4; ++lnMode) {
    assert(ranks.bestRankForHash(std::string(64, 'b'), lnMode) ==
           kClearTypeNormalClearRank);
  }
}

void testReplacementIsScopedIdempotentAndDeletesStaleRows() {
  const auto root = testRoot("replacement");
  const auto path = root / "score.db";
  ScoreRepository scores(path);
  assert(scores.EnsureSchema());

  const std::vector first{
      remoteScore("first", 'c', 100, kClearTypeEasyClearRank),
      remoteScore("stale", 'd', 120, kClearTypeHardClearRank),
  };
  assert(scores.ReplaceImportedIrScores(kProvider, kOrigin, 20, first).status ==
         ImportedIrScoreProjectionStatus::Applied);

  const std::vector otherOrigin{
      remoteScore("other", 'e', 140, kClearTypeExHardClearRank)};
  assert(scores
             .ReplaceImportedIrScores(kProvider, "https://other.example", 30,
                                      otherOrigin)
             .status == ImportedIrScoreProjectionStatus::Applied);

  const std::vector replacement{
      remoteScore("first", 'c', 150, kClearTypeNormalClearRank)};
  assert(scores
             .ReplaceImportedIrScores(kProvider, kOrigin, 21, replacement)
             .status == ImportedIrScoreProjectionStatus::Applied);
  assert(queryInt(path,
                  "SELECT COUNT(*) FROM scores WHERE score_source=1 AND "
                  "source_server_origin='https://boku.tachi.ac'") == 1);
  assert(queryInt(path,
                  "SELECT COUNT(*) FROM scores WHERE score_source=1 AND "
                  "source_server_origin='https://other.example'") == 1);
  assert(!scores.ImportedIrScoresAreCurrent(kProvider, kOrigin, 20,
                                            first.size()));
  assert(scores.ImportedIrScoresAreCurrent(kProvider, kOrigin, 21,
                                            replacement.size()));

  const auto unchanged =
      scores.ReplaceImportedIrScores(kProvider, kOrigin, 21, replacement);
  assert(unchanged.status == ImportedIrScoreProjectionStatus::AlreadyCurrent);

  const std::span<const ir::IrRemoteScore> empty;
  assert(scores.ReplaceImportedIrScores(kProvider, kOrigin, 22, empty).status ==
         ImportedIrScoreProjectionStatus::Applied);
  assert(queryInt(path,
                  "SELECT COUNT(*) FROM scores WHERE score_source=1 AND "
                  "source_server_origin='https://boku.tachi.ac'") == 0);
  assert(queryInt(path,
                  "SELECT COUNT(*) FROM scores WHERE score_source=1 AND "
                  "source_server_origin='https://other.example'") == 1);
}

void testInvalidSnapshotLeavesExistingProjectionUntouched() {
  const auto root = testRoot("rollback");
  const auto path = root / "score.db";
  ScoreRepository scores(path);
  assert(scores.EnsureSchema());

  const std::vector valid{
      remoteScore("valid", 'f', 160, kClearTypeHardClearRank)};
  assert(scores.ReplaceImportedIrScores(kProvider, kOrigin, 40, valid).status ==
         ImportedIrScoreProjectionStatus::Applied);

  auto invalid = remoteScore("invalid", '1', 201,
                             kClearTypeFullComboRank);
  const std::vector invalidSnapshot{invalid};
  const auto rejected = scores.ReplaceImportedIrScores(
      kProvider, kOrigin, 41, invalidSnapshot);
  assert(rejected.status == ImportedIrScoreProjectionStatus::Invalid);
  assert(scores.ImportedIrScoresAreCurrent(kProvider, kOrigin, 40,
                                            valid.size()));
  assert(queryInt(path, "SELECT COUNT(*) FROM scores WHERE score_source=1") ==
         1);
}

void testProviderClearRemovesOnlyMatchingImportedScores() {
  const auto root = testRoot("provider-clear");
  const auto path = root / "score.db";
  ScoreRepository scores(path);
  assert(scores.EnsureSchema());

  const std::vector tachi{
      remoteScore("tachi-score", '6', 150, kClearTypeNormalClearRank)};
  const std::vector other{
      remoteScore("other-score", '7', 160, kClearTypeHardClearRank)};
  assert(scores.ReplaceImportedIrScores(kProvider, kOrigin, 50, tachi).status ==
         ImportedIrScoreProjectionStatus::Applied);
  assert(scores
             .ReplaceImportedIrScores("other", "https://other.example", 51,
                                      other)
             .status == ImportedIrScoreProjectionStatus::Applied);

  const auto cleared = scores.ClearImportedIrScores(kProvider);
  assert(cleared.status == ImportedIrScoreProjectionStatus::Applied);
  assert(queryInt(path,
                  "SELECT COUNT(*) FROM scores WHERE score_source=1 AND "
                  "source_provider_id='tachi'") == 0);
  assert(queryInt(path,
                  "SELECT COUNT(*) FROM scores WHERE score_source=1 AND "
                  "source_provider_id='other'") == 1);

  const auto unchanged = scores.ClearImportedIrScores(kProvider);
  assert(unchanged.status ==
         ImportedIrScoreProjectionStatus::AlreadyCurrent);
}

void testLocalProjectedScoreKeepsLocalSourceAndFullComboInference() {
  const auto root = testRoot("local");
  const auto path = root / "score.db";
  ScoreRepository scores(path);
  assert(scores.EnsureSchema());

  result_persistence::PendingChartScoreWrite pending;
  pending.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  pending.resultId = 1;
  pending.createdAt = "2026-07-19 12:00:00.000";
  pending.score.chartSha256 = std::string(64, '9');
  pending.score.chartMd5 = std::string(32, '9');
  pending.score.chartTitle = "Local";
  pending.score.chartArtist = "Player";
  pending.score.score = 180;
  pending.score.maxScore = 200;
  pending.score.maxCombo = 100;
  pending.score.comboBreak = 0;
  pending.score.clearType = kClearTypeNormalClearRank;
  pending.score.provenance = ScoreProvenance::Legacy();
  assert(scores.SaveProjectedScore(pending).status ==
         result_persistence::ProjectionStatus::Inserted);

  assert(queryInt(path, "SELECT score_source FROM scores LIMIT 1") == 0);
  const auto ranks = scores.LoadBestClearRanks();
  assert(ranks.bestRankForHash(std::string(64, '9'), 0) ==
         kClearTypeFullComboRank);
}

} // namespace

int main() {
  testImportedSnapshotUsesNormalScoreSummaries();
  testRemoteLampIsAuthoritative();
  testReplacementIsScopedIdempotentAndDeletesStaleRows();
  testInvalidSnapshotLeavesExistingProjectionUntouched();
  testProviderClearRemovesOnlyMatchingImportedScores();
  testLocalProjectedScoreKeepsLocalSourceAndFullComboInference();
  std::cout << "IR score import projection tests passed\n";
  return 0;
}
