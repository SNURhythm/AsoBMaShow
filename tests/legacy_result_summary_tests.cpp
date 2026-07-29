#include "repositories/ReplayRepository.h"
#include "repositories/SqliteRAII.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-legacy-summary-" + std::to_string(stamp));
    assert(std::filesystem::create_directories(path));
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }

  std::filesystem::path path;
};

void exec(sqlite3 *database, std::string_view sql) {
  char *error = nullptr;
  const std::string statement(sql);
  assert(sqlite3_exec(database, statement.c_str(), nullptr, nullptr, &error) ==
         SQLITE_OK);
  sqlite3_free(error);
}

SqliteConnectionHandle openDatabase(const std::filesystem::path &path) {
  sqlite3 *raw = nullptr;
  assert(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
  return SqliteConnectionHandle(raw);
}

void seedSummaries(const std::filesystem::path &path) {
  auto database = openDatabase(path);
  constexpr std::string_view provenance =
      "{\"schemaVersion\":1,\"ruleset\":{\"version\":0},\"stages\":[],"
      "\"eligibility\":\"legacy-unverified\"}";
  exec(database.get(),
       "INSERT INTO legacy_chart_result_summaries(legacy_replay_id,"
       "chart_path,chart_md5,chart_sha256,chart_title,chart_artist,"
       "long_note_mode,final_score,max_combo,final_gauge,clear_type,created_at,"
       "ruleset_version,eligibility,provenance_json,partial) VALUES"
       "(10,'BMS/sha.bms','11111111111111111111111111111111',"
       "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
       "'SHA chart','Artist',1,1000,100,50.5,300,'2026-07-01 00:00:00',0,2,'" +
           std::string(provenance) +
           "',0),"
           "(20,'BMS/md5.bms','22222222222222222222222222222222',NULL,"
           "'MD5 chart','Artist',NULL,NULL,200,NULL,NULL,"
           "'2026-07-02 00:00:00',NULL,NULL,NULL,1),"
           "(30,'path/chart.bms',NULL,NULL,'Path chart','Artist',1,3000,"
           "300,70.0,400,'2026-07-03 00:00:00',NULL,NULL,NULL,1),"
           "(31,'Documents/BMS/path/chart.bms',NULL,NULL,'Legacy path chart',"
           "'Artist',1,3100,310,71.0,400,'2026-07-04 00:00:00',NULL,NULL,"
           "NULL,1)");
  exec(database.get(),
       "INSERT INTO legacy_course_result_summaries(legacy_course_replay_id,"
       "legacy_course_id,course_key,course_name,course_group_name,"
       "constraint_json,final_score,max_combo,final_gauge,clear_type,"
       "completed_charts,total_charts,created_at,ruleset_version,eligibility,"
       "provenance_json,partial) VALUES"
       "(40,7,'course-key','Course A','Group','{}',4000,400,72.0,300,2,2,"
       "'2026-07-05 00:00:00',NULL,NULL,NULL,1),"
       "(41,8,'course-key','Course B','Group','{}',4100,410,73.0,400,3,3,"
       "'2026-07-06 00:00:00',NULL,NULL,NULL,1),"
       "(42,9,NULL,'Legacy ID Course','Group','{}',NULL,NULL,NULL,NULL,1,2,"
       "'2026-07-07 00:00:00',NULL,NULL,NULL,1)");
}

void testChartLookupAndNullableFacts() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  ReplayRepository repository(path);
  assert(repository.EnsureSchema());
  repository.Shutdown();
  seedSummaries(path);

  bms_parser::ChartMeta sha;
  sha.SHA256 = std::string(64, 'a');
  auto rows = repository.ListLegacyChartSummaries(sha, 10);
  assert(rows.size() == 1 && rows.front().legacyReplayId == 10);
  assert(rows.front().finalScore == 1000);
  assert(rows.front().maxCombo == 100);
  assert(rows.front().createdAt == "2026-07-01 00:00:00");
  assert(!rows.front().partial);

  bms_parser::ChartMeta md5;
  md5.MD5 = std::string(32, '2');
  rows = repository.ListLegacyChartSummaries(md5, 10);
  assert(rows.size() == 1 && rows.front().legacyReplayId == 20);
  assert(!rows.front().finalScore && !rows.front().longNoteMode);
  assert(rows.front().partial);

  bms_parser::ChartMeta pathOnly;
  pathOnly.BmsPath = "path/chart.bms";
  rows = repository.ListLegacyChartSummaries(pathOnly, 1);
  assert(rows.size() == 1 && rows.front().legacyReplayId == 31);
  rows = repository.ListLegacyChartSummaries(pathOnly, 10);
  assert(rows.size() == 2 && rows[0].legacyReplayId == 31 &&
         rows[1].legacyReplayId == 30);
  assert(repository.ListLegacyChartSummaries(pathOnly, 0).empty());
  assert(repository
             .ListLegacyChartSummaries(pathOnly,
                                       kMaximumLegacyResultSummaryRows + 1)
             .empty());
}

void testCourseLookupAndLimit() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  ReplayRepository repository(path);
  assert(repository.EnsureSchema());
  repository.Shutdown();
  seedSummaries(path);

  auto rows = repository.ListLegacyCourseSummaries(
      {.courseKey = "course-key", .legacyCourseId = 0}, 1);
  assert(rows.size() == 1 && rows.front().legacyCourseReplayId == 41);
  rows = repository.ListLegacyCourseSummaries(
      {.courseKey = "course-key", .legacyCourseId = 0}, 10);
  assert(rows.size() == 2 && rows[0].legacyCourseReplayId == 41 &&
         rows[1].legacyCourseReplayId == 40);
  rows = repository.ListLegacyCourseSummaries(
      {.courseKey = {}, .legacyCourseId = 9}, 10);
  assert(rows.size() == 1 && rows.front().legacyCourseReplayId == 42 &&
         rows.front().completedCharts == 1 && rows.front().totalCharts == 2 &&
         rows.front().partial);
}

} // namespace

int main() {
  testChartLookupAndNullableFacts();
  testCourseLookupAndLimit();
  std::cout << "legacy result summary tests passed\n";
  return 0;
}
