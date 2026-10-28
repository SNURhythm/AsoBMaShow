#include "ir/IrRemoteScoreModels.h"
#include "repositories/ReplayRepository.h"
#include "repositories/ScoreRepository.h"
#include "sqlite3.h"
#include "streak/StreakStore.h"
#include "goals/ChartGoal.h"
#include "ScoreHistoryTime.h"
#include <ctime>

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
  pending.replayId = 1;
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

void testStreakProjectionIsAtomicAndUsesEffectiveLamp() {
  const auto root = testRoot("streak");
  const auto path = root / "score.db";
  const auto chartPath = root / "chart.db";
  ScoreRepository scores(path);
  scores.SetChartDatabasePath(chartPath);
  assert(scores.EnsureSchema());
  sqlite3 *db = nullptr;
  assert(sqlite3_open(chartPath.string().c_str(), &db) == SQLITE_OK);
  assert(sqlite3_exec(db,
      "CREATE TABLE difficulty_tables(id INTEGER,source_url TEXT,symbol TEXT);"
      "CREATE TABLE difficulty_table_entries(table_id INTEGER,level TEXT,md5 TEXT,sha256 TEXT,sort_order INTEGER);"
      "INSERT INTO difficulty_tables VALUES(1,'https://table','sl'),(2,'https://other','st');"
      "INSERT INTO difficulty_table_entries VALUES(1,'0','99999999999999999999999999999999','',0),"
      "(2,'1','99999999999999999999999999999999','',0);", nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(db);
  assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
  streak::State state;
  streak::Config config;
  config.dailyGoal = 10;
  config.assignments = {{.id="a", .name="First FC", .levels={{"https://table","0","sl0"}, {"https://other","1","st1"}},
                         .eligibleLamp=-1, .targetLamp=600, .points=20}};
  streak::configure(state, config, streak::today());
  streak::save(db, state);
  result_persistence::PendingChartScoreWrite pending;
  pending.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  pending.replayId = 1;
  pending.createdAt = scoreHistoryTime(static_cast<std::int64_t>(std::time(nullptr)) * 1000);
  pending.score.chartSha256 = std::string(64, '9');
  pending.score.chartMd5 = std::string(32, '9');
  pending.score.score = 180;
  pending.score.maxScore = 200;
  pending.score.comboBreak = 0;
  pending.score.clearType = kClearTypeNormalClearRank;
  pending.score.provenance = ScoreProvenance::Legacy();
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  state = streak::load(db);
  assert(state.days.rbegin()->second.points == 10); // Effective Full Combo, counted once across two tables.
  assert(state.currentStreak == 1 && state.savings == 0);
  assert(!scores.TakeStreakProgress("different-attempt"));
  const auto animation = scores.TakeStreakProgress(pending.attemptId);
  assert(animation && animation->assignments.size() == 1 && animation->assignments[0].completedRound());
  assert(!scores.TakeStreakProgress(pending.attemptId));
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::AlreadyPresent);
  assert(streak::load(db).savings == 0);
  // A failed streak write must roll back the score so recovery can retry both.
  assert(sqlite3_exec(db, "CREATE TRIGGER reject_streak BEFORE UPDATE ON streak_state BEGIN SELECT RAISE(ABORT,'test storage failure'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
  pending.attemptId = "123e4567-e89b-42d3-a456-426614174001";
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::StorageFailure);
  assert(queryInt(path, "SELECT COUNT(*) FROM scores") == 1);
  assert(!scores.TakeStreakProgress(pending.attemptId));
  assert(sqlite3_exec(db, "DROP TRIGGER reject_streak", nullptr, nullptr, nullptr) == SQLITE_OK);
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  assert(streak::load(db).savings == 0);
  // Historical MD5-only records still exclude a chart from NO PLAY tasks.
  auto legacyState = streak::load(db);
  legacyState.days.clear(); legacyState.configurations.clear();
  legacyState.savings = 0; legacyState.currentStreak = 0;
  streak::configure(legacyState, config, streak::today());
  streak::save(db, legacyState);
  assert(sqlite3_exec(db, "UPDATE scores SET chart_sha256=''", nullptr, nullptr, nullptr) == SQLITE_OK);
  pending.attemptId = "123e4567-e89b-42d3-a456-426614174002";
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  assert(streak::load(db).days.rbegin()->second.points == 0);

  struct LampCase {
    int mode, classic, specific, unknown, futureMode, futureLamp, expected;
  };
  // -1 means no stored record. Mode-specific FAILED must override a better
  // classic/unknown lamp; future records must not change a recovered play.
  for (const auto test : std::vector<LampCase>{
           {2, 400, -1, -1, -1, -1, 400},
           {3, 400, -1, -1, -1, -1, 400},
           {2, 400, -1, 600, -1, -1, 400},
           {2, 400, 0, 600, -1, -1, 0},
           {3, 400, 200, 600, -1, -1, 200},
           {2, -1, -1, 300, -1, -1, 300},
           {0, 400, -1, -1, -1, -1, -1},
           {2, 400, -1, -1, 2, 0, 400},
           {2, -1, -1, -1, 1, 400, -1}}) {
    streak::save(db, {});
    assert(sqlite3_exec(db, "DELETE FROM scores; DELETE FROM score_sha256_clear_rank_cache",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    const auto now = static_cast<std::int64_t>(std::time(nullptr)) * 1000;
    int attempt = 0;
    const auto write = [&](int mode, int lamp, int offset) {
      pending.attemptId = "123e4567-e89b-42d3-a456-42661417401" + std::to_string(attempt++);
      pending.createdAt = scoreHistoryTime(now + offset);
      pending.score.longNoteMode = mode;
      pending.score.clearType = lamp;
      pending.score.comboBreak = 1;
      assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
    };
    if (test.classic >= 0) write(1, test.classic, 0);
    if (test.specific >= 0) write(test.mode, test.specific, 0);
    if (test.unknown >= 0) write(0, test.unknown, 0);
    assert(scores.LoadBestClearRanks().bestRankForHash(pending.score.chartSha256, test.mode) == test.expected);
    if (test.futureMode >= 0) write(test.futureMode, test.futureLamp, 1000);
    streak::State scenario;
    streak::Config eligibility;
    eligibility.assignments = {
        {.id="exact", .name="Existing lamp", .levels={{"https://table","0","sl0"}},
         .eligibleLamp=test.expected, .targetLamp=0, .points=1},
        {.id="unplayed", .name="No play", .levels={{"https://table","0","sl0"}},
         .eligibleLamp=-1, .targetLamp=0, .points=1}};
    streak::configure(scenario, eligibility, streak::localDay(now / 1000));
    streak::save(db, scenario);
    write(test.mode, 300, 0);
    const auto progress = streak::load(db).days.rbegin()->second.assignments;
    assert(progress[0].completedRounds == 1);
    assert(progress[1].completedRounds == (test.expected == -1 ? 1 : 0));
  }

  struct ScoreCase { int mode, previousMode, score, futureScore, expectedRank; };
  for (const auto test : std::vector<ScoreCase>{
           {2, 2, 21, 24, 21}, {2, 1, 21, -1, -1}, {1, 0, 18, -1, 18},
           {3, 3, 24, 27, 24}, {0, 1, 21, -1, -1}, {2, 2, 0, -1, 0}}) {
    streak::save(db, {});
    assert(sqlite3_exec(db, "DELETE FROM scores; DELETE FROM score_sha256_clear_rank_cache; "
                           "DELETE FROM score_sha256_best_score_cache", nullptr, nullptr, nullptr) == SQLITE_OK);
    const auto now = static_cast<std::int64_t>(std::time(nullptr)) * 1000;
    int attempt = 0;
    const auto write = [&](int mode, int exScore, int offset) {
      pending.attemptId = "123e4567-e89b-42d3-a456-42661417403" + std::to_string(attempt++);
      pending.createdAt = scoreHistoryTime(now + offset);
      pending.score.longNoteMode = mode;
      pending.score.clearType = 300; pending.score.comboBreak = 1;
      pending.score.score = exScore; pending.score.maxScore = 27;
      assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
    };
    write(test.previousMode, test.score, 0);
    const auto best = scores.LoadBestScores().bestForHash(pending.score.chartSha256, test.mode);
    assert(streak::scoreRank(best ? best->score : 0, best ? best->maxScore : 0) == test.expectedRank);
    if (test.futureScore >= 0) write(test.mode, test.futureScore, 1000);
    streak::State scenario;
    streak::Config eligibility;
    eligibility.assignments = {
        {.id="exact-score", .name="Previous score", .levels={{"https://table","0",""}},
         .eligibleScoreRank=test.expectedRank, .targetLamp=0, .points=1},
        {.id="no-score", .name="No score", .levels={{"https://table","0",""}},
         .eligibleScoreRank=-1, .targetLamp=0, .points=1}};
    streak::configure(scenario, eligibility, streak::localDay(now / 1000));
    streak::save(db, scenario);
    write(test.mode, 27, 0); // Same timestamp; exclude this attempt and later-dated records.
    const auto progress = streak::load(db).days.rbegin()->second.assignments;
    assert(progress[0].completedRounds == 1);
    assert(progress[1].completedRounds == (test.expectedRank == -1 ? 1 : 0));
  }
  int importScenario = 0;
  for (const auto test : std::vector<ScoreCase>{
           {2, -1, 0, -1, 21}, {3, -1, 0, -1, 21},
           {1, 1, 110, -1, 21}, {2, 2, 180, -1, 24}}) {
    streak::save(db, {});
    assert(sqlite3_exec(db, "DELETE FROM scores; DELETE FROM score_sha256_clear_rank_cache; "
                           "DELETE FROM score_sha256_best_score_cache", nullptr, nullptr, nullptr) == SQLITE_OK);
    const auto now = static_cast<std::int64_t>(std::time(nullptr)) * 1000;
    pending.score.maxScore = 200; pending.score.comboBreak = 1; pending.score.clearType = 300;
    pending.createdAt = scoreHistoryTime(now);
    pending.attemptId = "123e4567-e89b-42d3-a456-426614174040";
    if (test.previousMode >= 0) {
      pending.score.longNoteMode = test.previousMode; pending.score.score = test.score;
      assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
    }
    auto imported = remoteScore("streak-wildcard", '9', 160, 300);
    assert(scores.ReplaceImportedIrScores(kProvider, kOrigin, 100 + importScenario++, std::span(&imported, 1)).status ==
           ImportedIrScoreProjectionStatus::Applied);
    const auto best = scores.LoadBestScores().bestForHash(pending.score.chartSha256, test.mode);
    assert(best && streak::scoreRank(best->score, best->maxScore) == test.expectedRank);
    streak::State scenario;
    streak::Config eligibility;
    eligibility.assignments = {
        {.id="import-score", .name="Imported best", .levels={{"https://table","0",""}},
         .eligibleScoreRank=test.expectedRank, .targetLamp=0, .points=1},
        {.id="import-no-score", .name="No score", .levels={{"https://table","0",""}},
         .eligibleScoreRank=-1, .targetLamp=0, .points=1}};
    streak::configure(scenario, eligibility, streak::localDay(now / 1000));
    streak::save(db, scenario);
    pending.attemptId = "123e4567-e89b-42d3-a456-426614174041";
    pending.score.longNoteMode = test.mode; pending.score.score = 200;
    assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
    const auto progress = streak::load(db).days.rbegin()->second.assignments;
    assert(progress[0].completedRounds == 1 && progress[1].completedRounds == 0);
  }
  pending.score.maxScore = 200;

  // Separate saved attempts retain eligibility and combine score/lamp targets.
  streak::save(db, {});
  assert(sqlite3_exec(db, "DELETE FROM scores; DELETE FROM score_sha256_clear_rank_cache",
                      nullptr, nullptr, nullptr) == SQLITE_OK);
  const auto promotionTime = static_cast<std::int64_t>(std::time(nullptr)) * 1000;
  const auto savePromotion = [&](int sequence, int lamp, int exScore) {
    pending.attemptId = "123e4567-e89b-42d3-a456-42661417402" + std::to_string(sequence);
    pending.createdAt = scoreHistoryTime(promotionTime + sequence);
    pending.score.longNoteMode = 2;
    pending.score.clearType = lamp;
    pending.score.comboBreak = 1;
    pending.score.score = exScore;
    assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  };
  savePromotion(0, 200, 100);
  streak::State promotion;
  streak::Config promotionConfig;
  promotionConfig.assignments = {{.id="promote", .name="EASY to HARD", .levels={{"https://table","0",""}},
                                 .eligibleLamp=200, .targetLamp=400, .scoreNumerator=9,
                                 .scoreDenominator=10, .points=20}};
  streak::configure(promotion, promotionConfig, streak::localDay(promotionTime / 1000));
  streak::save(db, promotion);
  savePromotion(1, 300, 190);
  assert(streak::load(db).days.rbegin()->second.points == 0);
  savePromotion(2, 400, 100);
  const auto promoted = streak::load(db);
  assert(promoted.days.rbegin()->second.points == 20);
  const auto &achievement = promoted.days.rbegin()->second.assignments[0].chartProgress.at(pending.score.chartSha256);
  assert(achievement.lamp == 400 && achievement.score == 190);
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::AlreadyPresent);
  assert(streak::load(db).days.rbegin()->second.points == 20);
  sqlite3_close(db);
}

} // namespace

void testPersonalGoalsUseProfileAndIndependentBestRecords() {
  const auto root = testRoot("personal-goals");
  const auto path = root / "score.db";
  ScoreRepository scores(path);
  assert(scores.EnsureSchema());
  std::string error;
  chart_goals::Goal goal;
  goal.chart.SHA256 = std::string(64, 'A');
  goal.chart.Title = "Personal target";
  goal.longNoteMode = 3; // A chart without LN has one shared goal.
  goal.target = {.lamp=500, .scoreNumerator=24};
  assert(scores.SaveChartGoal(goal, error));
  auto saved = scores.LoadChartGoal(goal.chart, 1, error);
  assert(saved && error.empty() && saved->goal.longNoteMode == 0);
  assert(!saved->progress.achieved);
  assert(saved->goal.setAtUnixMillis > 0 && saved->goal.achievedAtUnixMillis == 0);
  const auto setAt = saved->goal.setAtUnixMillis;
  const std::vector remote{remoteScore("goal-score", 'a', 190, 200),
                           remoteScore("goal-lamp", 'a', 160, 500)};
  assert(scores.ReplaceImportedIrScores(kProvider, kOrigin, 1, remote).status ==
         ImportedIrScoreProjectionStatus::Applied);
  auto draft = goal;
  draft.target = {}; // Best records must also work before choosing any target.
  const auto preview = scores.PreviewChartGoal(draft, error);
  assert(preview && error.empty() && preview->progress.bestScore->score == 190);
  assert(preview->progress.bestClear->clearType == 500);
  saved = scores.LoadChartGoal(goal.chart, 3, error);
  assert(saved && saved->progress.achieved);
  assert(saved->goal.setAtUnixMillis == setAt && saved->goal.achievedAtUnixMillis >= setAt);
  const auto achievedIndex = scores.LoadChartGoalIndex(error);
  assert(achievedIndex && achievedIndex->allAchieved(goal.chart.SHA256));
  assert(achievedIndex->activeHashes().empty());
  const auto achievedAt = saved->goal.achievedAtUnixMillis;
  assert(scores.SaveChartGoal(saved->goal, error));
  assert(scores.LoadChartGoal(goal.chart, 3, error)->goal.achievedAtUnixMillis == achievedAt);
  assert(saved->progress.bestScore->score == 190);
  assert(saved->progress.bestClear->clearType == 500);
  assert(!scores.LoadBestClearScore(saved->goal.chart)); // Replay-facing query stays local.
  auto page = scores.LoadChartGoals(0, error);
  assert(page && page->goals.size() == 1 && page->goals[0].progress.achieved);
  assert(scores.ClearImportedIrScores(kProvider).status == ImportedIrScoreProjectionStatus::Applied);
  assert(!scores.LoadChartGoal(goal.chart, 1, error)->progress.achieved);
  assert(scores.LoadChartGoalIndex(error)->activeHashes().size() == 1);
  assert(scores.LoadChartGoal(goal.chart, 1, error)->goal.achievedAtUnixMillis == achievedAt);
  goal.chart.SHA256 = std::string(64, 'b');
  goal.chart.TotalLongNotes = 1;
  goal.longNoteMode = 1; assert(scores.SaveChartGoal(goal, error));
  goal.longNoteMode = 2; assert(scores.SaveChartGoal(goal, error));
  assert(scores.LoadChartGoals(0, error)->goals.size() == 3);
  auto index = scores.LoadChartGoalIndex(error);
  assert(index && error.empty() && index->hashes().size() == 2);
  assert(index->modeFor(goal.chart.SHA256, 2) == 2 && index->modeFor(goal.chart.SHA256, 3) == 1);
  assert(!scores.LoadChartGoal(goal.chart, 3, error) && error.empty());
  goal.chart.LnMode = 2; // Authored CN collapses selections to CN.
  assert(scores.LoadChartGoal(goal.chart, 3, error));
  assert(scores.BindDatabasePath(root / "other.db", error));
  assert(scores.LoadChartGoals(0, error)->goals.empty());
  assert(scores.LoadChartGoalIndex(error)->modes.empty());
  assert(scores.SaveChartGoal(goal, error));
  assert(scores.LoadChartGoals(0, error)->goals.size() == 1);
  assert(scores.BindDatabasePath(path, error));
  assert(scores.LoadChartGoals(0, error)->goals.size() == 3);
  assert(scores.RemoveChartGoal(goal.chart, 3, error));
  assert(scores.LoadChartGoals(0, error)->goals.size() == 2);
  sqlite3 *db = nullptr;
  assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
  assert(sqlite3_exec(db, "CREATE TRIGGER reject_goal BEFORE INSERT ON chart_goals BEGIN SELECT RAISE(ABORT,'test failure'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
  assert(!scores.SaveChartGoal(goal, error) && !error.empty());
  assert(scores.LoadChartGoals(0, error)->goals.size() == 2);
  sqlite3_close(db);
}

void testGoalAchievementIsSavedOnceAndCelebrationIsProfileScoped() {
  const auto root = testRoot("goal-achievement");
  ScoreRepository scores(root / "score.db");
  assert(scores.EnsureSchema());
  std::string error;
  chart_goals::Goal goal;
  goal.chart.SHA256 = std::string(64, 'c');
  goal.chart.MD5 = std::string(32, 'c');
  goal.chart.Title = "AAA target";
  goal.chart.TotalLongNotes = 1; goal.longNoteMode = 2;
  goal.target = {.scoreNumerator=24};
  assert(scores.SaveChartGoal(goal, error));
  const auto setAt = scores.LoadChartGoal(goal.chart, 2, error)->goal.setAtUnixMillis;
  result_persistence::PendingChartScoreWrite pending;
  pending.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  pending.replayId = 1;
  pending.createdAt = scoreHistoryTime(setAt + 5000);
  pending.score.chartSha256 = goal.chart.SHA256;
  pending.score.chartMd5 = goal.chart.MD5;
  pending.score.longNoteMode = 2;
  pending.score.score = 1777; pending.score.maxScore = 2000;
  pending.score.comboBreak = 1; pending.score.clearType = 300;
  pending.score.provenance = ScoreProvenance::Legacy();
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
  assert(scores.LoadChartGoal(goal.chart, 2, error)->goal.achievedAtUnixMillis == 0);
  pending.attemptId.back() = '1'; pending.score.score = 1778;
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  const auto reached = scores.LoadChartGoal(goal.chart, 2, error);
  assert(reached->progress.achieved && reached->goal.achievedAtUnixMillis >= setAt);
  assert(reached->goal.achievedAtUnixMillis / 1000 == (setAt + 5000) / 1000);
  assert(scores.TakeChartGoalAchievements("other-attempt").empty());
  const auto celebration = scores.TakeChartGoalAchievements(pending.attemptId);
  assert(celebration.size() == 1 && celebration.front().longNoteMode == 2);
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::AlreadyPresent);
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
  pending.attemptId.back() = '2'; pending.score.score = 1900;
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
  assert(scores.LoadChartGoal(goal.chart, 2, error)->goal.achievedAtUnixMillis == reached->goal.achievedAtUnixMillis);
  // Legacy achievements have no trustworthy dates; neither a no-op save nor
  // a later play may invent a first-achievement date or a fresh celebration.
  sqlite3 *db = nullptr;
  assert(sqlite3_open((root / "score.db").string().c_str(), &db) == SQLITE_OK);
  assert(sqlite3_exec(db, "UPDATE chart_goals SET body=json_remove(body,'$.setAt','$.achievedAt')", nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(db);
  assert(scores.SaveChartGoal(reached->goal, error));
  const auto legacy = scores.LoadChartGoal(goal.chart, 2, error);
  assert(legacy->progress.achieved && legacy->goal.setAtUnixMillis == 0 && legacy->goal.achievedAtUnixMillis == 0);
  pending.attemptId.back() = '4'; pending.score.score = 1800;
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
  assert(scores.LoadChartGoal(goal.chart, 2, error)->goal.achievedAtUnixMillis == 0);
  // A changed target starts a new goal timeline, while no-op edits preserve it.
  goal.target = {.scoreNumerator=27};
  assert(scores.SaveChartGoal(goal, error));
  assert(scores.LoadChartGoal(goal.chart, 2, error)->goal.achievedAtUnixMillis == 0);
  pending.attemptId.back() = '3'; pending.score.score = 2000;
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  assert(scores.BindDatabasePath(root / "other.db", error));
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
  assert(scores.BindDatabasePath(root / "score.db", error));
  assert(scores.LoadChartGoal(goal.chart, 2, error)->goal.achievedAtUnixMillis > 0);
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
}

int main() {
  testGoalAchievementIsSavedOnceAndCelebrationIsProfileScoped();
  testPersonalGoalsUseProfileAndIndependentBestRecords();
  testStreakProjectionIsAtomicAndUsesEffectiveLamp();
  testImportedSnapshotUsesNormalScoreSummaries();
  testRemoteLampIsAuthoritative();
  testReplacementIsScopedIdempotentAndDeletesStaleRows();
  testInvalidSnapshotLeavesExistingProjectionUntouched();
  testProviderClearRemovesOnlyMatchingImportedScores();
  testLocalProjectedScoreKeepsLocalSourceAndFullComboInference();
  std::cout << "IR score import projection tests passed\n";
  return 0;
}
