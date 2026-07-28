#include "repositories/ReplayRepository.h"

#include "repositories/SqliteRAII.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-modern-course-repository-" + std::to_string(stamp));
    assert(std::filesystem::create_directories(path));
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

SqliteConnectionHandle openDatabase(const std::filesystem::path &path) {
  sqlite3 *raw = nullptr;
  assert(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
  return SqliteConnectionHandle(raw);
}

void exec(sqlite3 *database, const std::string &sql) {
  char *error = nullptr;
  if (sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error) !=
      SQLITE_OK) {
    std::cerr << (error != nullptr ? error : "SQLite failure") << '\n';
    sqlite3_free(error);
    std::abort();
  }
}

int queryInt(sqlite3 *database, const std::string &sql) {
  SqliteStatementHandle statement;
  assert(prepareSqliteStatement(database, sql, statement) == SQLITE_OK);
  assert(sqlite3_step(statement.get()) == SQLITE_ROW);
  return sqlite3_column_int(statement.get(), 0);
}

bool tableExists(sqlite3 *database, std::string_view table) {
  SqliteStatementHandle statement;
  assert(prepareSqliteStatement(
             database,
             "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
             statement) == SQLITE_OK);
  assert(sqlite3_bind_text(statement.get(), 1, table.data(),
                           static_cast<int>(table.size()), SQLITE_TRANSIENT) ==
         SQLITE_OK);
  return sqlite3_step(statement.get()) == SQLITE_ROW;
}

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

std::string attemptId(int suffix) {
  char value[37]{};
  std::snprintf(value, sizeof(value), "123e4567-e89b-42d3-a456-426614175%03d",
                suffix);
  return value;
}

#ifdef ASOBMASHOW_HAS_MODERN_COURSE_REPOSITORY

result_persistence::ModernCourseStageResult stage(int index, char hash,
                                                   int maximumCombo,
                                                   float finalGauge) {
  result_persistence::ModernCourseStageResult value;
  value.stageIndex = index;
  value.score.chartPath = "library/course-stage.bms";
  value.score.chartMd5 = repeated(hash, 32);
  value.score.chartSha256 = repeated(hash, 64);
  value.score.chartTitle = "Stage";
  value.score.chartArtist = "Artist";
  value.score.longNoteMode = 1;
  value.score.score = 7;
  value.score.maxScore = 10;
  value.score.maxCombo = maximumCombo;
  value.score.comboBreak = 1;
  value.score.pGreat = 3;
  value.score.great = 1;
  value.score.good = 1;
  value.score.finalGauge = finalGauge;
  value.score.clearType = kClearTypeHardClearRank;
  value.score.provenance = ScoreProvenance::Legacy();
  value.keyMode = index == 0 ? 7 : 14;
  value.adoptedGaugeType = GaugeType::Hard;
  value.adoptedGaugeHistory = {80.0F, finalGauge};
  return value;
}

result_persistence::ModernCourseResult result(int suffix, bool partial = true) {
  result_persistence::ModernCourseResultCapture capture{
      .attemptId = attemptId(suffix),
      .courseKey = "course:v1:" + repeated('c', 64),
      .legacyCourseId = 17,
      .courseName = "Repository Course",
      .courseGroupName = "Tests",
      .constraintJson = "[\"CLASS\"]",
      .requestedPlayOption = "NORMAL",
      .assistOption = "OFF",
      .initialGaugeType = GaugeType::Hard,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::None,
      .gaugeAutoShiftLowerBound = GaugeType::AssistedEasy,
      .longNoteMode = 1,
      .clearType = kClearTypeHardClearRank,
      .stages = {stage(0, 'a', 4, 76.0F),
                 stage(1, 'b', 8, 62.5F)},
      .entryFacts = {{.totalNotes = 5, .playLengthMicros = 1'000'000},
                     {.totalNotes = 5, .playLengthMicros = 2'000'000}},
      .playedAtUnixMillis = 1'700'000'100'000LL + suffix,
  };
  if (partial) {
    capture.entryFacts.push_back(
        {.totalNotes = 5, .playLengthMicros = 3'000'000});
  }
  std::string diagnostic;
  auto captured =
      result_persistence::captureModernCourseResult(capture, diagnostic);
  assert(captured.has_value());
  return *captured;
}

ModernReplayFileAttachment
attachment(const ModernReplayPathReservation &reservation, char hash = 'd') {
  return {.identity = reservation.identity,
          .metadata = {.relativePath = reservation.identity.relativePath,
                       .sha256 = repeated(hash, 64),
                       .compressedSize = 321,
                       .codecVersion =
                           replay::BeatorajaReplayCodec::kCodecVersion}};
}

replay::CoursePathInput coursePathInput(
    const result_persistence::ModernCourseResult &completed) {
  return {
      .stageSha256 = {completed.stages[0].score.chartSha256,
                      completed.stages[1].score.chartSha256},
      .longNoteMode = completed.longNoteMode,
      .beatorajaConstraintIds = {4},
  };
}

ModernReplayPathReservation reserve(ReplayRepository &repository,
                                    const result_persistence::ModernCourseResult
                                        &completed) {
  const auto input = coursePathInput(completed);
  std::string diagnostic;
  const auto stem = replay::courseStem(input, diagnostic);
  assert(stem.has_value());
  const auto reserved = repository.ReserveModernReplayPath(
      completed.attemptId, *stem, completed.playedAtUnixMillis);
  assert(reserved.status == ModernReplayReservationStatus::Reserved &&
         reserved.reservation.has_value());
  return *reserved.reservation;
}

void testAtomicCourseStageExactRetryAndStrictRead() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  auto database = openDatabase(databasePath);
  assert(queryInt(database.get(), "PRAGMA user_version") == 14);
  for (const std::string_view table : {"modern_course_results",
                                       "modern_course_stages",
                                       "modern_course_entries"}) {
    assert(tableExists(database.get(), table));
  }
  for (const std::string_view table :
       {"replays", "replay_events", "replay_touch_samples",
        "replay_lane_cover_events", "course_replays",
        "course_replay_stages"}) {
    assert(!tableExists(database.get(), table));
  }

  const auto completed = result(1);
  const auto reservation = reserve(repository, completed);
  const auto file = attachment(reservation);
  const auto staged = repository.StageModernCourseResult(
      completed, file, coursePathInput(completed));
  assert(staged.status == ModernCourseStageStatus::Staged && staged.receipt &&
         staged.receipt->resultId > 0);
  assert(queryInt(database.get(), "SELECT COUNT(*) FROM modern_course_results") ==
             1 &&
         queryInt(database.get(), "SELECT COUNT(*) FROM modern_course_stages") ==
             completed.completedCharts &&
         queryInt(database.get(), "SELECT COUNT(*) FROM modern_course_entries") ==
             completed.totalCharts &&
         queryInt(database.get(), "SELECT COUNT(*) FROM modern_replay_files") ==
             1 &&
         queryInt(database.get(),
                  "SELECT COUNT(*) FROM modern_replay_file_reservations") == 0);
  for (const std::string_view table :
       {"replays", "replay_events", "replay_touch_samples",
        "replay_lane_cover_events", "course_replays",
        "course_replay_stages"}) {
    assert(!tableExists(database.get(), table));
  }

  const auto loaded =
      repository.LoadModernCourseResultByAttempt(completed.attemptId);
  assert(loaded.status == ModernCourseResultReadStatus::Loaded &&
         loaded.record && loaded.record->result.resultId ==
                              staged.receipt->resultId &&
         loaded.record->result.stages == completed.stages &&
         loaded.record->result.entryFacts == completed.entryFacts &&
         loaded.record->replayFile &&
         loaded.record->replayFile->identity == file.identity &&
         loaded.record->replayFile->metadata == file.metadata);

  const auto retried = repository.StageModernCourseResult(
      completed, file, coursePathInput(completed));
  assert(retried.status == ModernCourseStageStatus::AlreadyStaged &&
         retried.receipt == staged.receipt);
  auto conflicting = file;
  conflicting.metadata.sha256 = repeated('e', 64);
  assert(repository
             .StageModernCourseResult(completed, conflicting,
                                      coursePathInput(completed))
             .status ==
         ModernCourseStageStatus::IntegrityConflict);

  exec(database.get(),
       "UPDATE modern_course_stages SET score=score+1 WHERE stage_index=1");
  assert(repository.LoadModernCourseResult(staged.receipt->resultId).status ==
         ModernCourseResultReadStatus::Invalid);
}

void testResultOnlyHistoryAndRollbackPreserveReservation() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  auto database = openDatabase(databasePath);

  auto older = result(2);
  auto newer = result(3, false);
  newer.playedAtUnixMillis = older.playedAtUnixMillis + 10;
  newer.resultFingerprint =
      result_persistence::modernResultFingerprint(newer);
  assert(repository.StageModernCourseResult(older, std::nullopt).status ==
         ModernCourseStageStatus::Staged);
  assert(repository.StageModernCourseResult(newer, std::nullopt).status ==
         ModernCourseStageStatus::Staged);
  const auto loaded =
      repository.LoadModernCourseResultByAttempt(older.attemptId);
  assert(loaded.status == ModernCourseResultReadStatus::Loaded &&
         loaded.record && !loaded.record->replayFile);
  const auto history = repository.ListModernCourseResults(older.courseKey, 2);
  assert(history.status == ModernCourseHistoryReadStatus::Loaded &&
         history.records.size() == 2 &&
         history.records[0].result.attemptId == newer.attemptId &&
         history.records[1].result.attemptId == older.attemptId);

  const auto failedResult = result(4);
  const auto reservation = reserve(repository, failedResult);
  exec(database.get(),
       "CREATE TRIGGER fail_course_stage BEFORE INSERT ON "
       "modern_course_stages BEGIN SELECT RAISE(ABORT,'injected'); END");
  assert(repository
             .StageModernCourseResult(failedResult, attachment(reservation),
                                      coursePathInput(failedResult))
             .status == ModernCourseStageStatus::StorageFailure);
  assert(queryInt(database.get(),
                  "SELECT COUNT(*) FROM modern_course_results WHERE "
                  "attempt_id='" +
                      failedResult.attemptId + "'") == 0 &&
         queryInt(database.get(),
                  "SELECT COUNT(*) FROM modern_replay_file_reservations "
                  "WHERE attempt_id='" +
                      failedResult.attemptId + "'") == 1);
}

void testReplayFileOwnerCheckRejectsBothAndNeither() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  const auto completed = result(5);
  const auto reservation = reserve(repository, completed);
  assert(repository.StageModernCourseResult(
                       completed, attachment(reservation),
                       coursePathInput(completed))
             .status == ModernCourseStageStatus::Staged);
  auto database = openDatabase(databasePath);
  char *error = nullptr;
  const std::string neither =
      "INSERT INTO modern_replay_files(modern_chart_result_id,"
      "modern_course_result_id,stem,history_index,relative_path,"
      "content_sha256,compressed_size,codec_version) VALUES(NULL,NULL,'" +
      repeated('f', 64) + "',9,'replay/invalid.brd','" + repeated('e', 64) +
      "',1,3)";
  assert(sqlite3_exec(database.get(), neither.c_str(), nullptr, nullptr,
                      &error) != SQLITE_OK);
  sqlite3_free(error);
  error = nullptr;
  const int courseId = queryInt(
      database.get(), "SELECT id FROM modern_course_results LIMIT 1");
  const std::string both =
      "INSERT INTO modern_replay_files(modern_chart_result_id,"
      "modern_course_result_id,stem,history_index,relative_path,"
      "content_sha256,compressed_size,codec_version) VALUES(1," +
      std::to_string(courseId) + ",'" + repeated('f', 64) +
      "',10,'replay/invalid2.brd','" + repeated('e', 64) + "',1,3)";
  assert(sqlite3_exec(database.get(), both.c_str(), nullptr, nullptr, &error) !=
         SQLITE_OK);
  sqlite3_free(error);
}

void testCourseScoreSourcesPageWithoutReplayOwnership() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  const auto first = result(6, false);
  const auto reservation = reserve(repository, first);
  const auto firstStage = repository.StageModernCourseResult(
      first, attachment(reservation), coursePathInput(first));
  const auto second = result(7, false);
  const auto third = result(8, false);
  const auto secondStage =
      repository.StageModernCourseResult(second, std::nullopt);
  const auto thirdStage =
      repository.StageModernCourseResult(third, std::nullopt);
  assert(firstStage.receipt && secondStage.receipt && thirdStage.receipt);

  auto database = openDatabase(databasePath);
  exec(database.get(), "UPDATE modern_replay_files SET stem='" +
                           repeated('e', 64) +
                           "' WHERE "
                           "modern_course_result_id=" +
                           std::to_string(firstStage.receipt->resultId));

  const auto firstPage = repository.ListModernCourseScoreSourcesAfter(0, 2);
  assert(firstPage.status == ModernCourseScoreSourceBatchStatus::Loaded &&
         firstPage.entries.size() == 2 && firstPage.hasMore &&
         firstPage.entries[0].status ==
             ModernCourseScoreSourceEntryStatus::Loaded &&
         firstPage.entries[0].source &&
         firstPage.entries[0].source->result.attemptId == first.attemptId &&
         !firstPage.entries[0].source->createdAt.empty() &&
         firstPage.entries[1].source &&
         firstPage.entries[1].source->result.attemptId == second.attemptId);
  const auto secondPage = repository.ListModernCourseScoreSourcesAfter(
      firstPage.entries.back().resultId, 2);
  assert(secondPage.status == ModernCourseScoreSourceBatchStatus::Loaded &&
         secondPage.entries.size() == 1 && !secondPage.hasMore &&
         secondPage.entries.front().source &&
         secondPage.entries.front().source->result.attemptId ==
             third.attemptId);
}

#endif

} // namespace

int main() {
#ifdef ASOBMASHOW_HAS_MODERN_COURSE_REPOSITORY
  testAtomicCourseStageExactRetryAndStrictRead();
  testResultOnlyHistoryAndRollbackPreserveReservation();
  testReplayFileOwnerCheckRejectsBothAndNeither();
  testCourseScoreSourcesPageWithoutReplayOwnership();
#else
  std::cerr << "FAIL: modern course repository contract is not implemented\n";
  return 1;
#endif
  std::cout << "modern course repository tests passed\n";
  return 0;
}
