#include "../src/ReplayDBHelper.h"
#include "../src/SqliteRAII.h"
#include "../src/Utils.h"
#include "../src/targets.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#define ASSERT_EQ(expected, actual, label)                                     \
  if ((expected) != (actual)) {                                                \
    std::cerr << label << " expected " << (expected) << " actual "            \
              << (actual) << std::endl;                                       \
    return 1;                                                                 \
  }

namespace {

void setHomeEnv(const std::string &home) {
#ifdef _WIN32
  _putenv_s("HOME", home.c_str());
#else
  setenv("HOME", home.c_str(), 1);
#endif
}

void unsetHomeEnv() {
#ifdef _WIN32
  _putenv_s("HOME", "");
#else
  unsetenv("HOME");
#endif
}

class ScopedHome {
public:
  explicit ScopedHome(const std::filesystem::path &home)
      : oldHome_(std::getenv("HOME") != nullptr
                     ? std::optional<std::string>(std::getenv("HOME"))
                     : std::nullopt) {
    setHomeEnv(home.string());
  }

  ~ScopedHome() {
    if (oldHome_.has_value()) {
      setHomeEnv(*oldHome_);
    } else {
      unsetHomeEnv();
    }
  }

private:
  std::optional<std::string> oldHome_;
};

void execOrAbort(sqlite3 *db, const std::string &sql) {
  char *error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    std::cerr << "exec failed: " << (error != nullptr ? error : "") << "\n"
              << sql << std::endl;
    sqlite3_free(error);
    std::abort();
  }
}

void insertChartReplays(sqlite3 *db, int count) {
  for (int i = 1; i <= count; ++i) {
    execOrAbort(
        db,
        "INSERT INTO replays ("
        "chart_path, chart_md5, chart_sha256, chart_title, chart_artist, "
        "gauge_type, gauge_auto_shift, final_score, max_combo, final_gauge, "
        "clear_type, assist_option"
        ") VALUES ("
        "'BMS/chart.bms', 'md5', 'sha', 'Title', 'Artist', 0, 0, " +
            std::to_string(i) + ", " + std::to_string(i) +
            ", 100.0, 300, 'OFF')");
  }
}

void insertCourseReplays(sqlite3 *db, int courseId, int count) {
  for (int i = 1; i <= count; ++i) {
    execOrAbort(
        db,
        "INSERT INTO course_replays ("
        "course_id, course_name, course_group_name, constraint_json, "
        "gauge_type, gauge_profile, gauge_auto_shift, ln_mode, "
        "requested_play_option, assist_option, final_score, max_combo, "
        "final_gauge, clear_type, completed_charts, total_charts"
        ") VALUES (" +
            std::to_string(courseId) +
            ", 'Course', 'Group', '{}', 0, 0, 0, 0, 'NORMAL', 'OFF', " +
            std::to_string(i) + ", " + std::to_string(i) +
            ", 100.0, 300, 1, 1)");
  }
}

} // namespace

int main() {
#if TARGET_OS_WINDOWS
  return 0;
#endif

  const std::filesystem::path testHome =
      std::filesystem::temp_directory_path() /
      "asobmashow_replay_db_helper_tests_home";
  std::filesystem::remove_all(testHome);
  std::filesystem::create_directories(testHome);
  ScopedHome scopedHome(testHome);

  ReplayDBHelper &helper = ReplayDBHelper::GetInstance();
  {
    SqliteConnectionHandle db(helper.Connect());
    if (!db || !helper.CreateReplayTables(db.get())) {
      std::cerr << "failed to initialize replay test db" << std::endl;
      return 1;
    }
    insertChartReplays(db.get(), 105);
    insertCourseReplays(db.get(), 7, 105);
  }

  bms_parser::ChartMeta meta;
  meta.SHA256 = "sha";
  meta.MD5 = "md5";
  meta.BmsPath = testHome / Utils::GameName / "BMS" / "chart.bms";
  meta.TotalNotes = 500;

  const auto defaultChartReplays = helper.ListReplays(meta);
  ASSERT_EQ(100U, defaultChartReplays.size(), "default chart replay limit");

  const auto allChartReplays = helper.ListReplays(meta, 0);
  ASSERT_EQ(105U, allChartReplays.size(), "unlimited chart replay count");
  ASSERT_EQ(105, allChartReplays.front().id, "unlimited chart newest id");
  ASSERT_EQ(1, allChartReplays.back().id, "unlimited chart oldest id");

  const auto defaultCourseReplays = helper.ListCourseReplays(7);
  ASSERT_EQ(100U, defaultCourseReplays.size(), "default course replay limit");

  const auto allCourseReplays = helper.ListCourseReplays(7, 0);
  ASSERT_EQ(105U, allCourseReplays.size(), "unlimited course replay count");
  ASSERT_EQ(105, allCourseReplays.front().id, "unlimited course newest id");
  ASSERT_EQ(1, allCourseReplays.back().id, "unlimited course oldest id");

  std::filesystem::remove_all(testHome);
  return 0;
}
