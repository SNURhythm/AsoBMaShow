#include "ir/IrRemoteScoreModels.h"
#include "repositories/ReplayRepository.h"
#include "repositories/ScoreRepository.h"
#include "repositories/ScoreCacheQueries.h"
#include "sqlite3.h"
#include "kombo/KomboStore.h"
#include "goals/ChartGoal.h"
#include "ScoreHistoryTime.h"
#include "Utils.h"
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

void testGaugeRecommendationSummaries() {
  const auto root = testRoot("gauge-goals");
  const auto path = root / "score.db";
  ScoreRepository scores(path);
  assert(scores.EnsureSchema());
  result_persistence::PendingChartScoreWrite pending;
  pending.attemptId = "123e4567-e89b-42d3-a456-426614175000";
  pending.replayId = 1;
  pending.createdAt = "2026-10-28 12:00:00.000";
  pending.score.chartSha256 = std::string(64, 'a');
  pending.score.chartMd5 = std::string(32, 'a');
  pending.score.maxScore = 200;
  pending.score.score = 150;
  pending.score.comboBreak = 5;
  pending.score.clearType = kClearTypeFailedRank;
  ScoreProvenanceBuildInput input;
  input.chartMeta.SHA256 = pending.score.chartSha256;
  input.chartMeta.MD5 = pending.score.chartMd5;
  input.chartMeta.TotalNotes = input.totalNotes = 100;
  input.judgeRankSource = JudgeRankSource::Chart;
  input.sourceJudgeRank = 2;
  input.ruleset = RulesetDescriptor::For(GameplayRuleset::LR2);
  input.candidateSelection = gameplay::CandidateSelectionMode::LR2;
  for (auto &context : input.effectiveJudgeContexts) {
    int i = 0;
    for (auto judgement : {PGreat,Great,Good,Bad,Kpoor})
      context.windows[i++] = {.judgement=judgement, .earlyMicros=-10000, .lateMicros=10000};
  }
  input.effectiveGaugeTotal = 300;
  pending.score.provenance = makeScoreProvenance(input);
  const auto save = [&] {
    const auto outcome = scores.SaveProjectedScore(pending);
    if (outcome.status != result_persistence::ProjectionStatus::Inserted)
      std::cerr << outcome.diagnostic << '\n';
    assert(outcome.status == result_persistence::ProjectionStatus::Inserted);
  };
  auto &evidence = pending.gaugeEvidence;
  evidence.recorded = true;
  evidence.passedNotes = 100;
  evidence.survivalMask = 0x1f8;
  evidence.values = {55,75,74.9F,0,0,100,0,0,0};
  evidence.deathPassedNotes = {0,0,0,90,89,-1,0,0,0};
  save();
  assert(queryInt(path, "SELECT COUNT(*) FROM goal_gauge_best") == 5);
  assert(queryInt(path, "SELECT combo_break FROM goal_combo_best") == 5);
  assert(queryInt(path, "SELECT COUNT(*) FROM goal_gauge_best WHERE gap<=CASE WHEN target<=300 THEN 5 ELSE 0.1 END") == 3);
  // Better evidence replaces only that gauge; worse attempts do not regress it.
  pending.attemptId.back() = '1'; pending.score.comboBreak = 1; evidence.values[2] = 78; evidence.deathPassedNotes[3] = 95;
  save();
  pending.attemptId.back() = '2'; pending.score.comboBreak = 2; evidence.values[2] = 60; evidence.deathPassedNotes[3] = 80;
  save();
  assert(queryInt(path, "SELECT gauge_value FROM goal_gauge_best WHERE target=300") == 78);
  assert(queryInt(path, "SELECT death_notes FROM goal_gauge_best WHERE target=400") == 95);
  assert(queryInt(path, "SELECT combo_break FROM goal_combo_best") == 1);
  // A partial play cannot claim final groove values, whole-chart survival, or near FC.
  pending.score.chartSha256 = std::string(64, 'b'); pending.attemptId.back() = '3';
  pending.score.comboBreak = 0; evidence.passedNotes = 99; evidence.values[2] = 100; evidence.deathPassedNotes[4] = -1;
  save();
  assert(queryInt(path, "SELECT COUNT(*) FROM goal_gauge_best WHERE chart_sha256 LIKE 'b%'") == 1);
  assert(queryInt(path, "SELECT COUNT(*) FROM goal_combo_best WHERE chart_sha256 LIKE 'b%'") == 0);
  // Modified, unknown, and course evidence cannot suggest a normal chart lamp.
  pending.score.chartSha256 = std::string(64, 'c'); pending.attemptId.back() = '4';
  evidence.passedNotes = 100; pending.score.provenance.practice = true;
  pending.score.provenance.eligibility = ScoreEligibility::Modified;
  save();
  pending.attemptId.back() = '5'; pending.score.provenance = makeScoreProvenance(input);
  pending.score.provenance.gaugeProfile = GaugeProfile::Course7Keys;
  save();
  pending.attemptId.back() = '6'; pending.score.provenance = makeScoreProvenance(input);
  const auto recorded = evidence; evidence = {};
  save();
  assert(queryInt(path, "SELECT COUNT(*) FROM goal_gauge_best WHERE chart_sha256 LIKE 'c%'") == 0);
  assert(queryInt(path, "SELECT COUNT(*) FROM goal_combo_best WHERE chart_sha256 LIKE 'c%'") == 0);
  // PMS clear borders differ from standard BMS; use the recorded rules/profile.
  pending.attemptId.back() = '7'; pending.score.chartSha256 = std::string(64, 'd'); evidence = recorded;
  input.ruleset = RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  input.candidateSelection = gameplay::CandidateSelectionMode::Lowest;
  input.gaugeProfile = GaugeProfile::Standard9Keys;
  pending.score.provenance = makeScoreProvenance(input);
  save();
  assert(queryInt(path, "SELECT border FROM goal_gauge_best WHERE chart_sha256 LIKE 'd%' AND target=300") == 85);
  // A summary failure rolls the score insert back with it.
  sqlite3 *blocked = nullptr;
  assert(sqlite3_open(path.string().c_str(), &blocked) == SQLITE_OK);
  assert(sqlite3_exec(blocked, "CREATE TRIGGER reject_gauge_summary BEFORE INSERT ON goal_gauge_best BEGIN SELECT RAISE(ABORT,'test failure'); END", nullptr,nullptr,nullptr)==SQLITE_OK);
  pending.attemptId.back() = '8'; pending.score.chartSha256 = std::string(64, 'e');
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::StorageFailure);
  assert(queryInt(path, "SELECT COUNT(*) FROM scores") == 8);
  assert(queryInt(path, "SELECT COUNT(*) FROM goal_combo_best WHERE chart_sha256 LIKE 'e%'") == 0);
  assert(sqlite3_exec(blocked,"DROP TRIGGER reject_gauge_summary",nullptr,nullptr,nullptr)==SQLITE_OK);
  sqlite3_close(blocked);
  // The preceding build's current-version profiles gain an empty combo summary.
  scores.Shutdown();
  sqlite3 *db = nullptr;
  assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
  assert(sqlite3_exec(db, "DROP TABLE goal_combo_best", nullptr, nullptr, nullptr)==SQLITE_OK);
  sqlite3_close(db);
  assert(scores.EnsureSchema());
  assert(queryInt(path, "SELECT COUNT(*) FROM goal_combo_best") == 0);
  assert(queryInt(path, "SELECT COUNT(*) FROM scores") == 8);
  // Pre-feature profiles start with empty summaries; existing scores stay intact.
  scores.Shutdown();
  assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
  assert(sqlite3_exec(db, "DROP TABLE goal_gauge_best; PRAGMA user_version=14;", nullptr, nullptr, nullptr)==SQLITE_OK);
  sqlite3_close(db);
  assert(scores.EnsureSchema());
  assert(queryInt(path, "SELECT COUNT(*) FROM goal_gauge_best") == 0);
  assert(queryInt(path, "SELECT COUNT(*) FROM pragma_table_info('scores') WHERE name='gauge_evidence'") == 0);
  assert(queryInt(path, "SELECT COUNT(*) FROM scores") == 8);
  assert(queryInt(path, "PRAGMA user_version") == ScoreRepository::kCurrentSchemaVersion);
  ScoreRepository other(root / "other.db");
  assert(other.EnsureSchema());
  assert(queryInt(root / "other.db", "SELECT COUNT(*) FROM goal_gauge_best") == 0);
  // Version 15 backfills completed local plays without requiring gauge payloads.
  input.ruleset = RulesetDescriptor::For(GameplayRuleset::LR2);
  input.candidateSelection = gameplay::CandidateSelectionMode::LR2;
  input.gaugeProfile = GaugeProfile::Standard;
  const auto historicalProvenance = makeScoreProvenance(input);
  int historyAttempt = 20;
  const auto historical = [&](char hash, int breaks, int passed, int mode = 0,
                              const ScoreProvenance *provenance = nullptr) {
    pending.attemptId = "123e4567-e89b-42d3-a456-4266141750" + std::to_string(historyAttempt++);
    pending.score.chartSha256 = std::string(64, hash);
    pending.score.longNoteMode = mode;
    pending.score.comboBreak = breaks;
    pending.score.pGreat = 75; pending.score.great = 0;
    pending.score.good = passed - 75 - breaks;
    pending.score.bad = breaks; pending.score.poor = 0; pending.score.kPoor = 7;
    pending.score.provenance = provenance ? *provenance : historicalProvenance;
    pending.gaugeEvidence = {};
    save();
  };
  historical('f', 4, 100);
  historical('f', 2, 100); // The minimum comes from any play, not just best EX.
  historical('f', 3, 100, 2); // LN scopes remain independent.
  historical('f', 4, 100, 3); // A better existing summary must survive.
  historical('1', 1, 99); // Empty POORs cannot make an incomplete play look complete.
  historical('2', 1, 100);
  historical('3', 1, 100);
  historical('4', 1, 100);
  historical('5', 3, 100);
  input.ruleset = RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  input.candidateSelection = gameplay::CandidateSelectionMode::Lowest;
  input.gaugeProfile = GaugeProfile::Standard9Keys;
  const auto pmsProvenance = makeScoreProvenance(input);
  historical('6', 1, 100, 0, &pmsProvenance); // PMS BAD did not consume the last note.
  historical('7', 1, 101, 0, &pmsProvenance); // 100 consumed notes plus one PMS BAD.
  scores.Shutdown();
  assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
  const auto exec = [&](const char *sql) { assert(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)==SQLITE_OK); };
  exec("INSERT INTO goal_combo_best SELECT chart_sha256,ln_mode,1 FROM scores WHERE chart_sha256 LIKE 'f%' AND ln_mode=3;"
       "UPDATE scores SET eligibility=1 WHERE chart_sha256 LIKE '2%';"
       "UPDATE scores SET score_source=1 WHERE chart_sha256 LIKE '3%';"
       "UPDATE scores SET provenance_json='invalid' WHERE chart_sha256 LIKE '4%';"
       "UPDATE scores SET ruleset_version=4,provenance_json=json_set(provenance_json,'$.ruleset.version',4) WHERE chart_sha256 LIKE '5%';"
       "DROP TABLE IF EXISTS score_v15_migration_state; PRAGMA user_version=14;"
       "CREATE TRIGGER reject_combo_backfill BEFORE INSERT ON goal_combo_best BEGIN SELECT RAISE(ABORT,'test failure'); END");
  sqlite3_close(db);
  assert(!scores.EnsureSchema());
  assert(queryInt(path, "PRAGMA user_version") == 14);
  assert(queryInt(path, "SELECT COUNT(*) FROM sqlite_master WHERE name='score_v15_migration_state'") == 0);
  assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
  exec("DROP TRIGGER reject_combo_backfill");
  sqlite3_close(db);
  assert(scores.EnsureSchema());
  assert(queryInt(path, "SELECT combo_break FROM goal_combo_best WHERE chart_sha256 LIKE 'f%' AND ln_mode=0") == 2);
  assert(queryInt(path, "SELECT combo_break FROM goal_combo_best WHERE chart_sha256 LIKE 'f%' AND ln_mode=2") == 3);
  assert(queryInt(path, "SELECT combo_break FROM goal_combo_best WHERE chart_sha256 LIKE 'f%' AND ln_mode=3") == 1);
  assert(queryInt(path, "SELECT combo_break FROM goal_combo_best WHERE chart_sha256 LIKE '5%'") == 3);
  assert(queryInt(path, "SELECT combo_break FROM goal_combo_best WHERE chart_sha256 LIKE '7%'") == 1);
  assert(queryInt(path, "SELECT COUNT(*) FROM goal_combo_best WHERE substr(chart_sha256,1,1) IN ('1','2','3','4','6')") == 0);
  assert(queryInt(path, "SELECT COUNT(*) FROM scores") == 19);
  assert(queryInt(path, "PRAGMA user_version") == 15);
  // Reopening does not re-read history once the migration completed.
  scores.Shutdown();
  assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
  exec("CREATE TRIGGER no_repeat_backfill BEFORE INSERT ON goal_combo_best BEGIN SELECT RAISE(ABORT,'must not repeat'); END");
  sqlite3_close(db);
  assert(scores.EnsureSchema());

}

void testKomboProjectionIsAtomicAndUsesEffectiveLamp() {
  const auto root = testRoot("kombo");
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
  kombo::State state;
  kombo::Config config;
  config.dailyGoal = 10;
  config.assignments = {{.id="a", .name="First FC", .levels={{"https://table","0","sl0"}, {"https://other","1","st1"}},
                         .eligibleLamp=-1, .targetLamp=600, .points=20}};
  kombo::configure(state, config, kombo::today());
  kombo::save(db, state);
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
  state = kombo::load(db);
  assert(state.days.rbegin()->second.points == 10); // Effective Full Combo, counted once across two tables.
  assert(state.currentKombo == 1 && state.savings == 0);
  assert(!scores.TakeKomboProgress("different-attempt"));
  const auto animation = scores.TakeKomboProgress(pending.attemptId);
  assert(animation && animation->assignments.size() == 1 && animation->assignments[0].completedRound());
  assert(!scores.TakeKomboProgress(pending.attemptId));
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::AlreadyPresent);
  assert(kombo::load(db).savings == 0);
  // A failed kombo write must roll back the score so recovery can retry both.
  assert(sqlite3_exec(db, "CREATE TRIGGER reject_kombo BEFORE UPDATE ON kombo_state BEGIN SELECT RAISE(ABORT,'test storage failure'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
  pending.attemptId = "123e4567-e89b-42d3-a456-426614174001";
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::StorageFailure);
  assert(queryInt(path, "SELECT COUNT(*) FROM scores") == 1);
  assert(!scores.TakeKomboProgress(pending.attemptId));
  assert(sqlite3_exec(db, "DROP TRIGGER reject_kombo", nullptr, nullptr, nullptr) == SQLITE_OK);
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  assert(kombo::load(db).savings == 0);
  // Historical MD5-only records still exclude a chart from NO PLAY tasks.
  auto legacyState = kombo::load(db);
  legacyState.days.clear(); legacyState.configurations.clear();
  legacyState.savings = 0; legacyState.currentKombo = 0;
  kombo::configure(legacyState, config, kombo::today());
  kombo::save(db, legacyState);
  assert(sqlite3_exec(db, "UPDATE scores SET chart_sha256=''", nullptr, nullptr, nullptr) == SQLITE_OK);
  pending.attemptId = "123e4567-e89b-42d3-a456-426614174002";
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  assert(kombo::load(db).days.rbegin()->second.points == 0);

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
    kombo::save(db, {});
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
    kombo::State scenario;
    kombo::Config eligibility;
    eligibility.assignments = {
        {.id="exact", .name="Existing lamp", .levels={{"https://table","0","sl0"}},
         .eligibleLamp=test.expected, .targetLamp=0, .points=1},
        {.id="unplayed", .name="No play", .levels={{"https://table","0","sl0"}},
         .eligibleLamp=-1, .targetLamp=0, .points=1}};
    kombo::configure(scenario, eligibility, kombo::localDay(now / 1000));
    kombo::save(db, scenario);
    write(test.mode, 300, 0);
    const auto progress = kombo::load(db).days.rbegin()->second.assignments;
    assert(progress[0].completedRounds == 1);
    assert(progress[1].completedRounds == (test.expected == -1 ? 1 : 0));
  }

  struct ScoreCase { int mode, previousMode, score, futureScore, expectedRank; };
  for (const auto test : std::vector<ScoreCase>{
           {2, 2, 21, 24, 21}, {2, 1, 21, -1, -1}, {1, 0, 18, -1, 18},
           {3, 3, 24, 27, 24}, {0, 1, 21, -1, -1}, {2, 2, 0, -1, 0}}) {
    kombo::save(db, {});
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
    assert(kombo::scoreRank(best ? best->score : 0, best ? best->maxScore : 0) == test.expectedRank);
    if (test.futureScore >= 0) write(test.mode, test.futureScore, 1000);
    kombo::State scenario;
    kombo::Config eligibility;
    eligibility.assignments = {
        {.id="exact-score", .name="Previous score", .levels={{"https://table","0",""}},
         .eligibleScoreRank=test.expectedRank, .targetLamp=0, .points=1},
        {.id="no-score", .name="No score", .levels={{"https://table","0",""}},
         .eligibleScoreRank=-1, .targetLamp=0, .points=1}};
    kombo::configure(scenario, eligibility, kombo::localDay(now / 1000));
    kombo::save(db, scenario);
    write(test.mode, 27, 0); // Same timestamp; exclude this attempt and later-dated records.
    const auto progress = kombo::load(db).days.rbegin()->second.assignments;
    assert(progress[0].completedRounds == 1);
    assert(progress[1].completedRounds == (test.expectedRank == -1 ? 1 : 0));
  }
  int importScenario = 0;
  for (const auto test : std::vector<ScoreCase>{
           {2, -1, 0, -1, 21}, {3, -1, 0, -1, 21},
           {1, 1, 110, -1, 21}, {2, 2, 180, -1, 24}}) {
    kombo::save(db, {});
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
    auto imported = remoteScore("kombo-wildcard", '9', 160, 300);
    assert(scores.ReplaceImportedIrScores(kProvider, kOrigin, 100 + importScenario++, std::span(&imported, 1)).status ==
           ImportedIrScoreProjectionStatus::Applied);
    const auto best = scores.LoadBestScores().bestForHash(pending.score.chartSha256, test.mode);
    assert(best && kombo::scoreRank(best->score, best->maxScore) == test.expectedRank);
    kombo::State scenario;
    kombo::Config eligibility;
    eligibility.assignments = {
        {.id="import-score", .name="Imported best", .levels={{"https://table","0",""}},
         .eligibleScoreRank=test.expectedRank, .targetLamp=0, .points=1},
        {.id="import-no-score", .name="No score", .levels={{"https://table","0",""}},
         .eligibleScoreRank=-1, .targetLamp=0, .points=1}};
    kombo::configure(scenario, eligibility, kombo::localDay(now / 1000));
    kombo::save(db, scenario);
    pending.attemptId = "123e4567-e89b-42d3-a456-426614174041";
    pending.score.longNoteMode = test.mode; pending.score.score = 200;
    assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
    const auto progress = kombo::load(db).days.rbegin()->second.assignments;
    assert(progress[0].completedRounds == 1 && progress[1].completedRounds == 0);
  }
  pending.score.maxScore = 200;

  // Separate saved attempts retain eligibility and combine score/lamp targets.
  kombo::save(db, {});
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
  kombo::State promotion;
  kombo::Config promotionConfig;
  promotionConfig.assignments = {{.id="promote", .name="EASY to HARD", .levels={{"https://table","0",""}},
                                 .eligibleLamp=200, .targetLamp=400, .scoreNumerator=9,
                                 .scoreDenominator=10, .points=20}};
  kombo::configure(promotion, promotionConfig, kombo::localDay(promotionTime / 1000));
  kombo::save(db, promotion);
  savePromotion(1, 300, 190);
  assert(kombo::load(db).days.rbegin()->second.points == 0);
  savePromotion(2, 400, 100);
  const auto promoted = kombo::load(db);
  assert(promoted.days.rbegin()->second.points == 10);
  const auto &achievement = promoted.days.rbegin()->second.assignments[0].chartProgress.at(pending.score.chartSha256);
  assert(achievement.lamp == 400 && achievement.score == 190);
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::AlreadyPresent);
  assert(kombo::load(db).days.rbegin()->second.points == 10);
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
  goal.longNoteMode = 3; // A chart without LN has shared goal scope.
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
  goal.id = 0;
  goal.chart.SHA256 = std::string(64, 'b');
  goal.chart.TotalLongNotes = 1;
  goal.longNoteMode = 1; assert(scores.SaveChartGoal(goal, error));
  goal.id = 0; goal.longNoteMode = 2; assert(scores.SaveChartGoal(goal, error));
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
  goal.id = 0; assert(scores.SaveChartGoal(goal, error));
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

void testCannotSetAlreadyAchievedGoal() {
  const auto root = testRoot("goal-already-achieved");
  ScoreRepository scores(root / "score.db");
  assert(scores.EnsureSchema());
  const std::vector remote{remoteScore("best-score", 'd', 190, 200),
                           remoteScore("best-lamp", 'd', 160, 500)};
  assert(scores.ReplaceImportedIrScores(kProvider, kOrigin, 1, remote).status ==
         ImportedIrScoreProjectionStatus::Applied);
  chart_goals::Goal goal;
  goal.chart.SHA256 = std::string(64, 'd'); goal.chart.MD5 = std::string(32, 'd');
  std::string error;
  goal.target = {.lamp=500};
  assert(!scores.SaveChartGoal(goal, error) && error == "goal_already_achieved");
  goal.target = {.scoreNumerator=24};
  assert(!scores.SaveChartGoal(goal, error) && error == "goal_already_achieved");
  goal.target = {.lamp=500, .scoreNumerator=24};
  assert(!scores.SaveChartGoal(goal, error) && error == "goal_already_achieved");
  assert(!scores.LoadChartGoal(goal.chart, 1, error) && error.empty());
  goal.target = {.lamp=600, .scoreNumerator=24}; // Only score is met.
  assert(scores.SaveChartGoal(goal, error));
  auto existing = scores.LoadChartGoal(goal.chart, 1, error);
  assert(existing && !existing->progress.achieved);
  goal.target = {.lamp=500, .scoreNumerator=24};
  assert(!scores.SaveChartGoal(goal, error) && error == "goal_already_achieved");
  const auto preserved = scores.LoadChartGoal(goal.chart, 1, error);
  assert(preserved && preserved->goal.target.lamp == 600);
  goal.target = {.lamp=500, .scoreNumerator=27}; // Only lamp is met.
  assert(scores.SaveChartGoal(goal, error));
  assert(!scores.LoadChartGoal(goal.chart, 1, error)->progress.achieved);
}

void testGoalIgnoresReplacedChartAtSamePath() {
  const auto root = testRoot("goal-replaced-chart");
  ScoreRepository scores(root / "score.db");
  assert(scores.EnsureSchema());
  const auto chartPath = root / "same.bms";
  result_persistence::PendingChartScoreWrite pending;
  pending.attemptId = "123e4567-e89b-42d3-a456-426614174040";
  pending.replayId = 1; pending.createdAt = "2026-07-19 12:00:00.000";
  pending.score.chartPath = Utils::GetStoragePathUtf8RelativeToDocuments(chartPath, "BMS/");
  pending.score.chartSha256 = std::string(64, 'a');
  pending.score.chartMd5 = std::string(32, 'a');
  pending.score.score = 200; pending.score.maxScore = 200;
  pending.score.comboBreak = 1; pending.score.clearType = 300;
  pending.score.provenance = ScoreProvenance::Legacy();
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  chart_goals::Goal goal;
  goal.chart.BmsPath = chartPath;
  goal.chart.SHA256 = std::string(64, 'b'); goal.chart.MD5 = std::string(32, 'b');
  goal.target = {.scoreNumerator=24};
  std::string error;
  const auto preview = scores.PreviewChartGoal(goal, error);
  assert(preview && error.empty() && !preview->progress.bestScore && !preview->progress.bestClear);
  assert(scores.SaveChartGoal(goal, error));
  auto stored = scores.LoadChartGoal(goal.chart, 1, error);
  assert(stored && !stored->progress.achieved && stored->goal.achievedAtUnixMillis == 0);
  pending.attemptId.back() = '1';
  pending.score.chartSha256 = goal.chart.SHA256; pending.score.chartMd5 = goal.chart.MD5;
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  assert(scores.TakeChartGoalAchievements(pending.attemptId).size() == 1);
  assert(scores.LoadChartGoal(goal.chart, 1, error)->goal.achievedAtUnixMillis > 0);
  goal.chart.BmsPath = root / "moved.bms";
  const auto moved = scores.PreviewChartGoal(goal, error);
  assert(moved && moved->progress.bestScore && moved->progress.bestScore->score == 200);
}

void testGoalAchievementIsSavedOnceAndCelebrationIsProfileScoped() {
  const auto root = testRoot("goal-achievement");
  ScoreRepository scores(root / "score.db");
  assert(scores.EnsureSchema());
  std::string error;
  kombo::Config routine;
  routine.dailyGoal = 20;
  kombo::DayId effectiveDay = 0;
  assert(scores.ConfigureKombo(routine, effectiveDay, error));
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
  sqlite3 *blocked = nullptr;
  assert(sqlite3_open((root / "score.db").string().c_str(), &blocked) == SQLITE_OK);
  assert(sqlite3_exec(blocked, "CREATE TRIGGER reject_goal_credit BEFORE INSERT ON chart_goals BEGIN SELECT RAISE(ABORT,'goal failure'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::StorageFailure);
  assert(queryInt(root / "score.db", "SELECT COUNT(*) FROM scores") == 1);
  assert(scores.LoadKombo(error)->days.at(effectiveDay).points == 0);
  assert(sqlite3_exec(blocked, "DROP TRIGGER reject_goal_credit; CREATE TRIGGER reject_goal_kombo BEFORE UPDATE ON kombo_state BEGIN SELECT RAISE(ABORT,'kombo failure'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::StorageFailure);
  assert(scores.LoadChartGoal(goal.chart, 2, error)->goal.achievedAtUnixMillis == 0);
  assert(sqlite3_exec(blocked, "DROP TRIGGER reject_goal_kombo", nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(blocked);
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  assert(scores.LoadKombo(error)->days.at(effectiveDay).points == 10);
  auto reached = scores.LoadChartGoal(goal.chart, 2, error);
  assert(reached->progress.achieved && reached->goal.achievedAtUnixMillis >= setAt);
  assert(reached->goal.achievedAtUnixMillis / 1000 == (setAt + 5000) / 1000);
  assert(scores.TakeChartGoalAchievements("other-attempt").empty());
  const auto celebration = scores.TakeChartGoalAchievements(pending.attemptId);
  assert(celebration.size() == 1 && celebration.front().longNoteMode == 2);
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::AlreadyPresent);
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
  assert(scores.LoadKombo(error)->days.at(effectiveDay).points == 10);
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
  assert(scores.LoadKombo(error)->days.at(effectiveDay).points == 20);
  assert(scores.BindDatabasePath(root / "other.db", error));
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
  assert(scores.BindDatabasePath(root / "score.db", error));
  assert(scores.LoadChartGoal(goal.chart, 2, error)->goal.achievedAtUnixMillis > 0);
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
}

void testLegacyGoalsAfterSelectorAttachesScores() {
  for (bool editFirst : {false, true}) {
    const auto root = testRoot(editFirst ? "legacy-goal-edit" : "legacy-goal-create");
    const auto path = root / "scores.db";
    ScoreRepository scores(path);
    assert(scores.EnsureSchema());
    std::string error;
    chart_goals::Goal original;
    original.chart.SHA256 = std::string(64, 'e');
    original.target.lamp = 400;
    assert(scores.SaveChartGoal(original, error));
    const auto originalId = original.id;
    const auto originalDate = original.setAtUnixMillis;
    sqlite3 *db = nullptr;
    assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "CREATE TABLE legacy_goals(sha256 TEXT NOT NULL,ln_mode INTEGER NOT NULL,body TEXT NOT NULL,PRIMARY KEY(sha256,ln_mode));"
        "INSERT INTO legacy_goals(rowid,sha256,ln_mode,body) SELECT rowid,sha256,ln_mode,body FROM chart_goals;"
        "DROP TABLE chart_goals; ALTER TABLE legacy_goals RENAME TO chart_goals;",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    // The live selector installs the score trigger through an attached alias,
    // after the score repository has already initialized its own connection.
    sqlite3 *charts = nullptr;
    assert(sqlite3_open(":memory:", &charts) == SQLITE_OK);
    assert(!score_cache_queries::prepareScoreQueryDatabase(charts, path));
    auto added = original; added.id = 0; added.target = {.scoreNumerator=24};
    auto &first = editFirst ? original : added;
    const bool saved = scores.SaveChartGoal(first, error);
    if (!saved) std::cerr << "Legacy goal migration: " << error << '\n';
    assert(saved);
    assert(scores.SaveChartGoal(editFirst ? added : original, error));
    const auto goals = scores.LoadChartGoalsForChart(original.chart, error);
    assert(goals && goals->size() == 2 && error.empty());
    assert(original.id == originalId && original.setAtUnixMillis == originalDate);
    assert(added.id != originalId);
    original.target.lamp = 500;
    assert(scores.SaveChartGoal(original, error));
    assert(scores.LoadChartGoal(original.chart, 0, error, originalId)->goal.target.lamp == 500);
    assert(scores.LoadChartGoal(added.chart, 0, error, added.id)->goal.target.scoreNumerator == 24);
    sqlite3_close(charts);
  }
}

void testIndependentChartGoals() {
  const auto root = testRoot("multiple-chart-goals");
  ScoreRepository scores(root / "scores.db");
  assert(scores.EnsureSchema());
  std::string error;
  chart_goals::Goal lamp;
  lamp.chart.SHA256 = std::string(64, 'd'); lamp.chart.MD5 = std::string(32, 'd');
  lamp.chart.TotalLongNotes = 1; lamp.longNoteMode = 2;
  lamp.target = {.lamp=500};
  auto score = lamp; score.target = {.scoreNumerator=24};
  assert(scores.SaveChartGoal(lamp, error) && scores.SaveChartGoal(score, error));
  assert(lamp.id > 0 && score.id != lamp.id);
  assert(scores.LoadChartGoalsForChart(lamp.chart, error)->size() == 2);
  result_persistence::PendingChartScoreWrite pending;
  pending.attemptId = "123e4567-e89b-42d3-a456-426614174070";
  pending.replayId = 1; pending.createdAt = scoreHistoryTime(score.setAtUnixMillis + 1000);
  pending.score.chartSha256 = lamp.chart.SHA256; pending.score.chartMd5 = lamp.chart.MD5;
  pending.score.longNoteMode = 2; pending.score.score = 180; pending.score.maxScore = 200;
  pending.score.clearType = 300; pending.score.comboBreak = 1;
  pending.score.provenance = ScoreProvenance::Legacy();
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  auto achieved = scores.TakeChartGoalAchievements(pending.attemptId);
  assert(achieved.size() == 1 && achieved.front().id == score.id);
  assert(!scores.LoadChartGoalIndex(error)->allAchieved(lamp.chart.SHA256));
  assert(scores.LoadChartGoalIndex(error)->activeHashes().size() == 1);
  assert(scores.LoadChartGoal(score.chart, 2, error, score.id)->goal.achievedAtUnixMillis > 0);
  assert(scores.LoadChartGoal(lamp.chart, 2, error, lamp.id)->goal.achievedAtUnixMillis == 0);
  pending.attemptId.back() = '1'; pending.score.clearType = 500;
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  achieved = scores.TakeChartGoalAchievements(pending.attemptId);
  assert(achieved.size() == 1 && achieved.front().id == lamp.id);
  assert(scores.LoadChartGoalIndex(error)->allAchieved(lamp.chart.SHA256));
  const auto scoreDate = scores.LoadChartGoal(score.chart, 2, error, score.id)->goal.achievedAtUnixMillis;
  lamp.target.lamp = 600;
  assert(scores.SaveChartGoal(lamp, error));
  assert(!scores.LoadChartGoalIndex(error)->allAchieved(lamp.chart.SHA256));
  assert(scores.LoadChartGoal(score.chart, 2, error, score.id)->goal.achievedAtUnixMillis == scoreDate);
  assert(scores.RemoveChartGoal(lamp.chart, 2, error, lamp.id));
  assert(scores.LoadChartGoalsForChart(lamp.chart, error)->size() == 1);
  assert(scores.LoadChartGoalIndex(error)->allAchieved(lamp.chart.SHA256));
  assert(!scores.SaveChartGoal(lamp, error));
  assert(scores.LoadChartGoalsForChart(score.chart, error)->front().goal.id == score.id);
  // One play may complete two independent targets in the same chart/LN scope.
  kombo::Config routine;
  routine.dailyGoal = 20;
  kombo::DayId effectiveDay = 0;
  assert(scores.ConfigureKombo(routine, effectiveDay, error));
  lamp.id = 0;
  score.target.scoreNumerator = 27;
  assert(scores.SaveChartGoal(lamp, error) && scores.SaveChartGoal(score, error));
  pending.attemptId.back() = '2'; pending.score.clearType = 600; pending.score.score = 200;
  pending.createdAt = scoreHistoryTime(std::max(lamp.setAtUnixMillis, score.setAtUnixMillis) + 1000);
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  achieved = scores.TakeChartGoalAchievements(pending.attemptId);
  assert(achieved.size() == 2 && achieved[0].id != achieved[1].id);
  assert(scores.LoadKombo(error)->days.at(effectiveDay).points == 20);
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::AlreadyPresent);
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
  assert(scores.LoadKombo(error)->days.at(effectiveDay).points == 20);
  pending.attemptId.back() = '3';
  assert(scores.SaveProjectedScore(pending).status == result_persistence::ProjectionStatus::Inserted);
  assert(scores.TakeChartGoalAchievements(pending.attemptId).empty());
  assert(scores.LoadKombo(error)->days.at(effectiveDay).points == 20);
}

void testFolderGoalPersistenceAndPlayCredit() {
  const auto root=testRoot("folder-goals");
  bms_parser::ChartMeta chart;
  chart.BmsPath=root/"charts"/"a.bms"; chart.Folder=chart.BmsPath.parent_path();
  chart.SHA256=std::string(64,'e'); chart.MD5=std::string(32,'e'); chart.TotalNotes=100;
  sqlite3 *db=nullptr;
  assert(sqlite3_open((root/"chart.db").string().c_str(),&db)==SQLITE_OK);
  const std::string fixture="CREATE TABLE chart_meta(path TEXT,sha256 TEXT,md5 TEXT,ln_mode INTEGER,total_long_notes INTEGER,total_backspin_notes INTEGER,total_notes INTEGER,source_priority INTEGER,source_archive_size INTEGER);"
      "CREATE TABLE difficulty_tables(id INTEGER,source_url TEXT);"
      "CREATE TABLE difficulty_table_entries(table_id INTEGER,level TEXT,sha256 TEXT,md5 TEXT);"
      "INSERT INTO difficulty_tables VALUES(1,'https://folder-test');"
      "INSERT INTO chart_meta(path,sha256,md5,ln_mode,total_long_notes,total_backspin_notes,total_notes) VALUES('/charts/a','"+chart.SHA256+"','"+chart.MD5+"',0,0,0,100);"
      "INSERT INTO difficulty_table_entries VALUES(1,'1','"+chart.SHA256+"',''),(1,'1','missing','');";
  assert(sqlite3_exec(db,fixture.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);
  sqlite3_close(db);
  ScoreRepository scores(root/"score.db"); scores.SetChartDatabasePath(root/"chart.db");
  assert(scores.EnsureSchema());
  folder_goals::Goal goal{.source={.kind=folder_goals::SourceKind::Level,
      .identity="https://folder-test",.level="1",.name="Test 1"},
      .target={.type=folder_goals::Type::Lamp,.lamp=200,.percent=10000}};
  std::string error;
  assert(scores.SaveFolderGoal(goal,error));
  auto rows=scores.LoadFolderGoals(error);
  assert(rows && rows->size()==1 && rows->front().progress.available==1 && rows->front().progress.missing==1);
  const auto first=rows->front().goal;
  assert(scores.SaveFolderGoal(first,error));
  assert(scores.LoadFolderGoals(error)->front().goal.setAt==first.setAt);
  result_persistence::PendingChartScoreWrite pending;
  pending.attemptId="123e4567-e89b-42d3-a456-426614174060";
  pending.replayId=1; pending.createdAt="2026-10-29 12:00:00.000";
  pending.score.chartSha256=chart.SHA256; pending.score.chartMd5=chart.MD5;
  pending.score.chartPath=chart.BmsPath.string(); pending.score.score=100; pending.score.maxScore=200;
  pending.score.clearType=200; pending.score.comboBreak=1; pending.score.provenance=ScoreProvenance::Legacy();
  assert(scores.SaveProjectedScore(pending).status==result_persistence::ProjectionStatus::Inserted);
  assert(scores.TakeFolderGoalAchievements(pending.attemptId).size()==1);
  assert(scores.TakeFolderGoalAchievements(pending.attemptId).empty());
  assert(scores.LoadFolderGoals(error)->front().progress.achieved);
  pending.attemptId.back()='1';
  assert(scores.SaveProjectedScore(pending).status==result_persistence::ProjectionStatus::Inserted);
  assert(scores.TakeFolderGoalAchievements(pending.attemptId).empty());
  // Adding an available, unplayed member drops progress, but never resets its first achievement.
  assert(sqlite3_open((root/"chart.db").string().c_str(),&db)==SQLITE_OK);
  const auto added="INSERT INTO chart_meta(path,sha256,md5,ln_mode,total_long_notes,total_backspin_notes,total_notes) VALUES('/charts/b','"+std::string(64,'f')+"','"+std::string(32,'f')+"',0,0,0,100);"
      "UPDATE difficulty_table_entries SET sha256='"+std::string(64,'f')+"' WHERE sha256='missing';"
      "UPDATE difficulty_tables SET id=2; UPDATE difficulty_table_entries SET table_id=2;";
  assert(sqlite3_exec(db,added.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);
  auto changed=scores.LoadFolderGoals(error);
  assert(changed && changed->front().progress.available==2 && changed->front().progress.missing==0 && !changed->front().progress.achieved);
  assert(changed->front().goal.achievedAt>0);
  // Imports can complete a new EX goal without local play credit.
  auto ex=goal; ex.target={.type=folder_goals::Type::AverageEx,.percent=6000};
  assert(scores.SaveFolderGoal(ex,error));
  auto imported=remoteScore("folder-import",'f',200,300);
  assert(scores.ReplaceImportedIrScores(kProvider,kOrigin,1,std::span(&imported,1)).status==ImportedIrScoreProjectionStatus::Applied);
  auto importedGoals=scores.LoadFolderGoals(error);
  assert(importedGoals && importedGoals->size()==2);
  for(const auto &g:*importedGoals) assert(g.progress.achieved && g.goal.achievedAt>0);
  pending.attemptId.back()='2';pending.score.score=110;
  assert(scores.SaveProjectedScore(pending).status==result_persistence::ProjectionStatus::Inserted);
  assert(scores.TakeFolderGoalAchievements(pending.attemptId).empty());
  assert(scores.SaveProjectedScore(pending).status==result_persistence::ProjectionStatus::AlreadyPresent);
  assert(scores.TakeFolderGoalAchievements(pending.attemptId).empty());
  // Removing the source keeps both goals without transferring them to another table.
  assert(sqlite3_exec(db,"DELETE FROM difficulty_tables;DELETE FROM difficulty_table_entries",nullptr,nullptr,nullptr)==SQLITE_OK);
  assert(!scores.LoadFolderGoals(error)->front().progress.sourceAvailable);
  sqlite3_close(db);
  assert(scores.RemoveFolderGoal(ex,error));
  scores.SetDatabasePath(root/"other.db"); assert(scores.EnsureSchema());
  assert(scores.LoadFolderGoals(error)->empty());
  scores.SetDatabasePath(root/"score.db");
  assert(scores.RemoveFolderGoal(goal,error));
  assert(scores.LoadFolderGoals(error)->empty());
}

int main() {
  testLegacyGoalsAfterSelectorAttachesScores();
  testIndependentChartGoals();
  testFolderGoalPersistenceAndPlayCredit();
  testGaugeRecommendationSummaries();
  testCannotSetAlreadyAchievedGoal();
  testGoalIgnoresReplacedChartAtSamePath();
  testGoalAchievementIsSavedOnceAndCelebrationIsProfileScoped();
  testPersonalGoalsUseProfileAndIndependentBestRecords();
  testKomboProjectionIsAtomicAndUsesEffectiveLamp();
  testImportedSnapshotUsesNormalScoreSummaries();
  testRemoteLampIsAuthoritative();
  testReplacementIsScopedIdempotentAndDeletesStaleRows();
  testInvalidSnapshotLeavesExistingProjectionUntouched();
  testProviderClearRemovesOnlyMatchingImportedScores();
  testLocalProjectedScoreKeepsLocalSourceAndFullComboInference();
  std::cout << "IR score import projection tests passed\n";
  return 0;
}
