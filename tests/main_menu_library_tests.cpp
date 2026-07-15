#include "../src/scene/MainMenuLibrary.h"
#include "../src/repositories/ChartRepositoryInternal.h"
#include "../src/LongNoteModeUtils.h"
#include "RepositorySqliteTestSupport.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>

#define ASSERT_EQ(expected, actual, label)                                     \
  if ((expected) != (actual)) {                                                \
    std::cerr << label << " expected " << (expected) << " actual "            \
              << (actual) << std::endl;                                       \
    return 1;                                                                 \
  }

namespace {

void execOrAbort(sqlite3 *db, const std::string &sql) {
  char *error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    std::cerr << "exec failed: " << (error != nullptr ? error : "") << "\n"
              << sql << std::endl;
    sqlite3_free(error);
    std::abort();
  }
}

int folderRankForLn(const chart_library::FolderClearDataByLongNoteMode &data,
                    const std::string &folderKey) {
  const auto &ranks = data.clearRanks[long_note_mode::kLnValue];
  const auto it = ranks.find(folderKey);
  return it == ranks.end() ? kNoClearTypeRank : it->second;
}

int folderRankForMode(
    const chart_library::FolderClearDataByLongNoteMode &data,
    const std::string &folderKey, int longNoteMode) {
  const auto &ranks =
      data.clearRanks[long_note_mode::normalizeValue(longNoteMode)];
  const auto it = ranks.find(folderKey);
  return it == ranks.end() ? kNoClearTypeRank : it->second;
}

int folderClearCountForLn(
    const chart_library::FolderClearDataByLongNoteMode &data,
    const std::string &folderKey, int clearRank) {
  const auto &countsByFolder = data.clearMarkCounts[long_note_mode::kLnValue];
  const auto folderIt = countsByFolder.find(folderKey);
  if (folderIt == countsByFolder.end()) {
    return 0;
  }
  const auto countIt = folderIt->second.find(clearRank);
  return countIt == folderIt->second.end() ? 0 : countIt->second;
}

} // namespace

std::size_t TransparentStringHash::operator()(std::string_view value) const
    noexcept {
  return std::hash<std::string_view>{}(value);
}

int ScoreClearRankCache::bestRankForStoredKey(std::string_view sha256,
                                              int longNoteMode) const {
  const auto it = rankBySha256.find(sha256);
  if (it == rankBySha256.end()) {
    return kNoClearTypeRank;
  }
  const int mode = long_note_mode::normalizeValue(longNoteMode);
  const int rank = it->second.ranks[static_cast<std::size_t>(mode)];
  if (rank != kNoClearTypeRank || mode == long_note_mode::kUnknownValue) {
    return rank;
  }
  const int classicLongNoteRank =
      it->second.ranks[long_note_mode::kLnValue];
  return classicLongNoteRank != kNoClearTypeRank
             ? classicLongNoteRank
             : it->second.ranks[long_note_mode::kUnknownValue];
}

int CourseScoreRankByLongNoteMode::bestRankForMode(int lnMode) const {
  const int mode = long_note_mode::normalizeValue(lnMode);
  return std::max(ranks[static_cast<std::size_t>(mode)], wildcardRank);
}

int ScoreClearRankCache::bestCourseRankFor(std::string_view courseKey,
                                           int legacyCourseId,
                                           int lnMode) const {
  int rank = kNoClearTypeRank;
  const auto keyIt = rankByCourseKey.find(courseKey);
  if (keyIt != rankByCourseKey.end()) {
    rank = keyIt->second.bestRankForMode(lnMode);
  }
  const auto idIt = rankByLegacyCourseId.find(legacyCourseId);
  if (idIt != rankByLegacyCourseId.end()) {
    rank = std::max(rank, idIt->second.bestRankForMode(lnMode));
  }
  return rank;
}

int scoreLongNoteModeForClearLamp(int chartLongNoteMode, int totalLongNotes,
                                  int totalBackSpinNotes,
                                  int selectedLongNoteMode) {
  if (std::max(0, totalLongNotes) + std::max(0, totalBackSpinNotes) <= 0) {
    return 0;
  }
  const int forcedLongNoteMode =
      long_note_mode::normalizeValue(chartLongNoteMode);
  if (forcedLongNoteMode > 0) {
    return forcedLongNoteMode;
  }
  return long_note_mode::normalizeValue(selectedLongNoteMode);
}

int main() {
  sqlite3 *db = nullptr;
  if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
    std::cerr << "open failed" << std::endl;
    return 1;
  }

  execOrAbort(db,
              "CREATE TABLE chart_meta ("
              "path TEXT PRIMARY KEY,"
              "md5 TEXT NOT NULL,"
              "sha256 TEXT NOT NULL,"
              "ln_mode INTEGER NOT NULL DEFAULT 0,"
              "total_long_notes INTEGER NOT NULL DEFAULT 0,"
              "total_backspin_notes INTEGER NOT NULL DEFAULT 0,"
              "source_priority INTEGER,"
              "source_archive_size INTEGER"
              ")");
  execOrAbort(db,
              "CREATE TABLE difficulty_table_entries ("
              "table_id INTEGER NOT NULL,"
              "level TEXT NOT NULL,"
              "sha256 TEXT NOT NULL,"
              "md5 TEXT NOT NULL"
              ")");
  execOrAbort(db,
              "CREATE TABLE difficulty_courses ("
              "id INTEGER PRIMARY KEY,"
              "course_key TEXT NOT NULL,"
              "name TEXT NOT NULL,"
              "table_id INTEGER NOT NULL,"
              "group_name TEXT NOT NULL"
              ")");
  execOrAbort(db,
              "CREATE TABLE difficulty_course_entries ("
              "course_id INTEGER NOT NULL,"
              "sha256 TEXT NOT NULL,"
              "md5 TEXT NOT NULL,"
              "sort_order INTEGER NOT NULL"
              ")");

  execOrAbort(db,
              "INSERT INTO chart_meta(path, md5, sha256) "
              "VALUES('charts/md5-only.bms', 'md5-local', 'sha-local')");
  execOrAbort(db,
              "INSERT INTO chart_meta(path, md5, sha256, ln_mode, "
              "total_long_notes) VALUES('charts/forced-cn.bms', "
              "'md5-forced-cn', 'sha-forced-cn', 2, 1)");
  execOrAbort(db,
              "INSERT INTO difficulty_table_entries(table_id, level, sha256, "
              "md5) VALUES(1, '12', '', 'md5-local'),"
              "(2, '13', 'sha-forced-cn', 'md5-forced-cn')");
  execOrAbort(db,
              "INSERT INTO difficulty_courses(id, course_key, name, table_id, "
              "group_name) VALUES(10, "
              "'course:v1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
              "aaaaaaaaaaaaaa', 'Current renamed course', 1, 'Courses'),"
              "(20, 'course:v1:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
              "bbbbbbbbbbbbbbbbbb', 'Blank fallback course', 1, 'Courses'),"
              "(30, 'course:v1:cccccccccccccccccccccccccccccccccccccccccccccc"
              "cccccccccccccccccc', 'Mismatching key course', 1, 'Courses')");
  execOrAbort(db,
              "INSERT INTO difficulty_course_entries(course_id, sha256, md5, "
              "sort_order) VALUES(10, '', 'md5-local', 1),"
              "(20, '', 'md5-local', 1),(30, '', 'md5-local', 1)");

  ScoreClearRankCache scoreRanks;
  scoreRanks.rankBySha256["sha-local"].ranks[0] = kClearTypeHardClearRank;
  scoreRanks.rankBySha256["sha-forced-cn"]
      .ranks[long_note_mode::kLnValue] = kClearTypeHardClearRank;
  auto &courseRanks = scoreRanks.rankByCourseKey[
      "course:v1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaa"];
  courseRanks.ranks[long_note_mode::kCnValue] = kClearTypeExHardClearRank;
  courseRanks.wildcardRank = kClearTypeNormalClearRank;
  scoreRanks.rankByLegacyCourseId[20]
      .ranks[long_note_mode::kLnValue] = kClearTypeFullComboRank;
  // This keyed record historically used ID 30. Its nonempty key differs from
  // the current course key, so it must not enter the legacy ID fallback map.
  scoreRanks
      .rankByCourseKey["course:v1:ddddddddddddddddddddddddddddddddddddddddddddd"
                       "ddddddddddddddddddd"]
      .ranks[long_note_mode::kLnValue] = kClearTypeFullComboRank;

  repository_test::StatementTrace trace;
  chart_library::FolderClearDataByLongNoteMode data;
  {
    repository_test::ScopedStatementTrace observation(db, trace);
    data = chart_repository_detail::LoadFolderClearDataByLongNoteMode(
        db, scoreRanks);
  }
  ASSERT_EQ(4, trace.count,
            "folder clear aggregation keeps four streaming SELECTs");

  ASSERT_EQ(kClearTypeHardClearRank,
            folderRankForLn(data, main_menu_library::folderKeyForTable(1)),
            "difficulty table folder uses matched chart sha");
  ASSERT_EQ(kClearTypeHardClearRank,
            folderRankForLn(data,
                            main_menu_library::folderKeyForLevel(1, "12")),
            "difficulty level folder uses matched chart sha");
  ASSERT_EQ(1,
            folderClearCountForLn(
                data, main_menu_library::folderKeyForTable(1),
                kClearTypeHardClearRank),
            "difficulty table clear mark count uses matched chart sha");
  ASSERT_EQ(1,
            folderClearCountForLn(
                data, main_menu_library::folderKeyForLevel(1, "12"),
                kClearTypeHardClearRank),
            "difficulty level clear mark count uses matched chart sha");
  ASSERT_EQ(kClearTypeHardClearRank,
            folderRankForMode(data,
                              main_menu_library::folderKeyForTable(2),
                              long_note_mode::kCnValue),
            "forced-CN folder inherits a historical classic-LN lamp");
  ASSERT_EQ(kClearTypeHardClearRank,
            folderRankForMode(data,
                              main_menu_library::folderKeyForTable(2),
                              long_note_mode::kHcnValue),
            "forced-CN folder keeps its historical lamp for every selection");
  ASSERT_EQ(kClearTypeHardClearRank,
            folderRankForLn(data,
                            main_menu_library::folderKeyForCourseTable(1)),
            "course table folder uses matched chart sha");
  ASSERT_EQ(kClearTypeHardClearRank,
            folderRankForLn(
                data, main_menu_library::folderKeyForCourseGroup(1, "Courses")),
            "course group folder uses matched chart sha");
  ASSERT_EQ(kClearTypeNormalClearRank,
            folderRankForLn(data, main_menu_library::folderKeyForCourse(10)),
            "course folder uses canonical-key wildcard score lamp");
  ASSERT_EQ(kClearTypeNormalClearRank,
            folderRankForMode(data,
                              main_menu_library::folderKeyForCourse(10),
                              long_note_mode::kLnValue),
            "renamed and renumbered course keeps canonical-key wildcard lamp");
  ASSERT_EQ(kClearTypeExHardClearRank,
            folderRankForMode(data,
                              main_menu_library::folderKeyForCourse(10),
                              long_note_mode::kCnValue),
            "course canonical-key lamp separates exact CN mode");
  ASSERT_EQ(kClearTypeNormalClearRank,
            folderRankForMode(data,
                              main_menu_library::folderKeyForCourse(10),
                              long_note_mode::kHcnValue),
            "course wildcard lamp contributes to HCN mode");
  ASSERT_EQ(kClearTypeFullComboRank,
            folderRankForMode(data,
                              main_menu_library::folderKeyForCourse(20),
                              long_note_mode::kLnValue),
            "blank-key score uses same-ID course folder fallback");
  ASSERT_EQ(kClearTypeHardClearRank,
            folderRankForMode(data,
                              main_menu_library::folderKeyForCourse(30),
                              long_note_mode::kLnValue),
            "same-ID nonempty mismatching key cannot override course folder");

  sqlite3_close(db);
  return 0;
}
