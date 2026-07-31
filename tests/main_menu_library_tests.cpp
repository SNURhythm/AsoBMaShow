#include "../src/scene/MainMenuLibrary.h"
#include "../src/LongNoteModeUtils.h"
#include "../src/repositories/ScoreRepositoryModels.h"
#include "RepositorySqliteTestSupport.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace repository_test {

chart_library::FolderClearDataByLongNoteMode
loadFolderClearDataByLongNoteMode(
    sqlite3 *database, const ScoreClearRankCache &projectedChartRanks,
    const ScoreClearRankCache &localCourseRanks);

} // namespace repository_test

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
  const int mode = long_note_mode::normalizeValue(longNoteMode);
  const auto rankForMode = [mode](const ScoreRankByLongNoteMode &byMode) {
    const int selected = byMode.ranks[static_cast<std::size_t>(mode)];
    if (selected != kNoClearTypeRank ||
        mode == long_note_mode::kUnknownValue) {
      return selected;
    }
    const int classic = byMode.ranks[long_note_mode::kLnValue];
    return classic != kNoClearTypeRank
               ? classic
               : byMode.ranks[long_note_mode::kUnknownValue];
  };
  const auto local = rankBySha256.find(sha256);
  return local == rankBySha256.end() ? kNoClearTypeRank
                                     : rankForMode(local->second);
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
  ASSERT_EQ(
      static_cast<int>(
          main_menu_library::EmptyLibraryBootstrapMode::DefaultFolder),
      static_cast<int>(main_menu_library::emptyLibraryBootstrapMode(
          TargetPlatform::iOS)),
      "iOS empty library uses the default folder");
  ASSERT_EQ(
      static_cast<int>(
          main_menu_library::EmptyLibraryBootstrapMode::DefaultFolder),
      static_cast<int>(main_menu_library::emptyLibraryBootstrapMode(
          TargetPlatform::Android)),
      "Android empty library uses the default folder");
  ASSERT_EQ(
      static_cast<int>(
          main_menu_library::EmptyLibraryBootstrapMode::FolderPicker),
      static_cast<int>(main_menu_library::emptyLibraryBootstrapMode(
          TargetPlatform::MacOS)),
      "desktop empty library uses the folder picker");

  const auto directoryEntries = main_menu_library::downloadedPathScanEntries(
      std::filesystem::path("/library/downloaded-folder"));
  ASSERT_EQ(static_cast<std::size_t>(1), directoryEntries.size(),
            "downloaded folder produces one scan root");
  ASSERT_EQ(std::filesystem::path("/library/downloaded-folder"),
            std::filesystem::path(directoryEntries.front().path),
            "downloaded folder is the exact scan root");

  const auto archiveEntries = main_menu_library::downloadedPathScanEntries(
      std::filesystem::path("/library/_archives/downloaded.zip"));
  ASSERT_EQ(static_cast<std::size_t>(1), archiveEntries.size(),
            "downloaded archive produces one scan root");
  ASSERT_EQ(std::filesystem::path("/library/_archives/downloaded.zip"),
            std::filesystem::path(archiveEntries.front().path),
            "downloaded archive is the exact scan root");
  ASSERT_EQ(true, main_menu_library::downloadedPathScanEntries({}).empty(),
            "empty result path produces no scan root");

  const std::string shaA(64, 'a');
  const std::string shaB(64, 'b');
  const std::string md5A(32, '1');
  ChartMetaRecord missingTarget;
  missingTarget.unavailable = true;
  missingTarget.meta.SHA256 = "  " + std::string(64, 'A') + "\n";
  missingTarget.meta.MD5 = md5A;
  const auto durableTarget =
      main_menu_library::findBmsChartIdentity(missingTarget.meta);
  ASSERT_EQ(shaA, durableTarget.sha256,
            "Find BMS target normalizes SHA-256");
  ASSERT_EQ(md5A, durableTarget.md5, "Find BMS target preserves valid MD5");
  ASSERT_EQ(true,
            main_menu_library::findBmsSelectionHandoffAllowed(
                7, 7, durableTarget, missingTarget),
            "unchanged missing selection allows preview handoff");
  ASSERT_EQ(false,
            main_menu_library::findBmsSelectionHandoffAllowed(
                7, 8, durableTarget, missingTarget),
            "an intervening selection change rejects preview handoff");
  ASSERT_EQ(false,
            main_menu_library::findBmsSelectionHandoffAllowed(
                7, 9, durableTarget, missingTarget),
            "away-and-back selection still rejects preview handoff");

  ChartMetaRecord sameMd5WrongSha = missingTarget;
  sameMd5WrongSha.meta.SHA256 = shaB;
  ASSERT_EQ(false,
            main_menu_library::findBmsSelectionHandoffAllowed(
                7, 7, durableTarget, sameMd5WrongSha),
            "conflicting SHA-256 cannot be rescued by matching MD5");

  ChartMetaRecord md5OnlyTarget = missingTarget;
  md5OnlyTarget.meta.SHA256.clear();
  const auto md5Identity =
      main_menu_library::findBmsChartIdentity(md5OnlyTarget.meta);
  ASSERT_EQ(true,
            main_menu_library::findBmsSelectionHandoffAllowed(
                11, 11, md5Identity, md5OnlyTarget),
            "MD5 is the fallback durable Find BMS identity");
  md5OnlyTarget.unavailable = false;
  ASSERT_EQ(false,
            main_menu_library::findBmsSelectionHandoffAllowed(
                11, 11, md5Identity, md5OnlyTarget),
            "an already available selection does not need a handoff");

  ChartMetaRecord titleOnlyTarget;
  titleOnlyTarget.unavailable = true;
  titleOnlyTarget.meta.Title = "Title-only result";
  const auto titleOnlyIdentity =
      main_menu_library::findBmsChartIdentity(titleOnlyTarget.meta);
  ASSERT_EQ(false, titleOnlyIdentity.valid(),
            "title-only targets have no durable handoff identity");
  ASSERT_EQ(false,
            main_menu_library::findBmsSelectionHandoffAllowed(
                3, 3, titleOnlyIdentity, titleOnlyTarget),
            "title-only downloads never guess a preview chart");

  ASSERT_EQ(true,
            main_menu_library::sameChartSelection(missingTarget,
                                                  missingTarget),
            "reselecting the same row does not change selection generation");
  ChartMetaRecord differentSelection = missingTarget;
  differentSelection.meta.SHA256 = shaB;
  differentSelection.meta.MD5 = std::string(32, '2');
  ASSERT_EQ(false,
            main_menu_library::sameChartSelection(missingTarget,
                                                  differentSelection),
            "selecting another chart changes selection generation");
  std::uint64_t changedGeneration =
      main_menu_library::chartSelectionGenerationAfter(
          7, missingTarget, differentSelection);
  ASSERT_EQ(std::uint64_t{8}, changedGeneration,
            "switching away advances selection generation");
  changedGeneration = main_menu_library::chartSelectionGenerationAfter(
      changedGeneration, differentSelection, missingTarget);
  ASSERT_EQ(std::uint64_t{9}, changedGeneration,
            "switching back advances selection generation again");
  ASSERT_EQ(changedGeneration,
            main_menu_library::chartSelectionGenerationAfter(
                changedGeneration, missingTarget, missingTarget),
            "reselecting the same row preserves selection generation");

  std::vector<bms_parser::ChartMeta> downloadedMatches(3);
  downloadedMatches[0].BmsPath = "/library/other/outside.bms";
  downloadedMatches[1].BmsPath = "/library/downloaded/z-chart.bms";
  downloadedMatches[2].BmsPath = "/library/downloaded/a-chart.bms";
  ASSERT_EQ(std::filesystem::path("/library/downloaded/a-chart.bms"),
            *main_menu_library::downloadedChartPath(
                downloadedMatches, "/library/downloaded"),
            "downloaded directory selects the stable first in-scope match");
  const std::vector<std::filesystem::path> upsertedDownloadedPaths{
      "/library/downloaded/z-chart.bms"};
  ASSERT_EQ(std::filesystem::path("/library/downloaded/z-chart.bms"),
            *main_menu_library::downloadedChartPath(
                downloadedMatches, "/library/downloaded",
                upsertedDownloadedPaths),
            "downloaded handoff selects only a chart upserted by this scan");
  ASSERT_EQ(false,
            main_menu_library::downloadedChartPath(
                downloadedMatches, "/library/downloaded",
                std::vector<std::filesystem::path>{})
                .has_value(),
            "downloaded handoff rejects stale rows not upserted by this scan");

  downloadedMatches.resize(1);
  downloadedMatches[0].BmsPath =
      "/library/_archives/downloaded.zip/folder/chart.bms";
  ASSERT_EQ(
      std::filesystem::path(
          "/library/_archives/downloaded.zip/folder/chart.bms"),
      *main_menu_library::downloadedChartPath(
          downloadedMatches, "/library/_archives/downloaded.zip"),
      "downloaded archive accepts its virtual chart path");
  ASSERT_EQ(false,
            main_menu_library::downloadedChartPath(
                downloadedMatches, "/library/_archives/other.zip")
                .has_value(),
            "downloaded chart lookup rejects matches outside exact output");

  ChartMetaRecord explicitFolderRecord;
  explicitFolderRecord.meta.Folder = "/library/A/../A";
  const auto explicitFolder =
      main_menu_library::sameFolderForChart(explicitFolderRecord);
  ASSERT_EQ(true, explicitFolder.has_value(),
            "same-folder scope uses chart metadata folder");
  ASSERT_EQ(std::filesystem::path("/library/A"), *explicitFolder,
            "same-folder scope normalizes metadata folder");

  ChartMetaRecord archiveRecord;
  archiveRecord.meta.BmsPath = "/packs/pack.zip/A/song.bms";
  const auto archiveFolder =
      main_menu_library::sameFolderForChart(archiveRecord);
  ASSERT_EQ(true, archiveFolder.has_value(),
            "same-folder scope falls back to chart path parent");
  ASSERT_EQ(std::filesystem::path("/packs/pack.zip/A"), *archiveFolder,
            "archive chart scope keeps its exact inner parent path");

  ChartMetaRecord emptyRecord;
  ASSERT_EQ(false,
            main_menu_library::sameFolderForChart(emptyRecord).has_value(),
            "same-folder scope rejects records without a usable path");

  ChartRecordFilters activeFilters;
  activeFilters.clearMarkRank = kClearTypeHardClearRank;
  activeFilters.clearMarkOrAbove = true;
  activeFilters.scoreRank = "AAA";
  activeFilters.scoreRankOrBelow = true;
  activeFilters.bpmMin = 120.0;
  activeFilters.bpmMax = 180.0;
  activeFilters.difficultyMinLevel = "10";
  activeFilters.difficultyMaxLevel = "12";
  activeFilters.sort = {
      .criterion = ChartRecordSortCriterion::Difficulty,
      .direction = ChartRecordSortDirection::Ascending,
  };
  const auto sameFolderFilters =
      main_menu_library::filtersForSameFolder(activeFilters);
  ASSERT_EQ(false, sameFolderFilters.clearMarkRank.has_value(),
            "same-folder scope clears clear-mark filtering");
  ASSERT_EQ(false, sameFolderFilters.scoreRank.has_value(),
            "same-folder scope clears score filtering");
  ASSERT_EQ(false, sameFolderFilters.bpmMin.has_value(),
            "same-folder scope clears minimum BPM filtering");
  ASSERT_EQ(false, sameFolderFilters.bpmMax.has_value(),
            "same-folder scope clears maximum BPM filtering");
  ASSERT_EQ(false, sameFolderFilters.difficultyMinLevel.has_value(),
            "same-folder scope clears minimum difficulty filtering");
  ASSERT_EQ(false, sameFolderFilters.difficultyMaxLevel.has_value(),
            "same-folder scope clears maximum difficulty filtering");
  ASSERT_EQ(static_cast<int>(ChartRecordSortCriterion::Difficulty),
            static_cast<int>(sameFolderFilters.sort.criterion),
            "same-folder scope preserves sort criterion");
  ASSERT_EQ(static_cast<int>(ChartRecordSortDirection::Ascending),
            static_cast<int>(sameFolderFilters.sort.direction),
            "same-folder scope preserves sort direction");

  const auto sameFolderQuery = main_menu_library::chartQueryForSameFolder(
      *archiveFolder, "needle", sameFolderFilters,
      long_note_mode::kCnValue);
  ASSERT_EQ(std::filesystem::path("/packs/pack.zip/A"),
            *sameFolderQuery.exactFolder,
            "same-folder query uses exact archive-internal parent");
  ASSERT_EQ(std::string("needle"), sameFolderQuery.keyword,
            "same-folder query preserves its current search text");
  ASSERT_EQ(0, sameFolderQuery.tableId,
            "same-folder query does not inherit a table scope");
  ASSERT_EQ(false, sameFolderQuery.favoritesOnly,
            "same-folder query does not inherit favorites scope");
  ASSERT_EQ(long_note_mode::kCnValue, sameFolderQuery.selectedLongNoteMode,
            "same-folder query preserves selected long-note mode");
  ASSERT_EQ(static_cast<int>(ChartRecordSortCriterion::Difficulty),
            static_cast<int>(sameFolderQuery.sortCriterion),
            "same-folder query preserves sort criterion");
  ASSERT_EQ(static_cast<int>(ChartRecordSortDirection::Ascending),
            static_cast<int>(sameFolderQuery.sortDirection),
            "same-folder query preserves sort direction");

  ChartMetaRecord retainedSelection;
  retainedSelection.meta.BmsPath = "/library/A/selected.bms";
  ASSERT_EQ(std::filesystem::path("/library/A/selected.bms"),
            main_menu_library::chartSelectionPathForReload(
                {}, retainedSelection),
            "reload falls back to retained chart after list selection reset");
  ASSERT_EQ(std::filesystem::path("/library/B/visible.bms"),
            main_menu_library::chartSelectionPathForReload(
                "/library/B/visible.bms", retainedSelection),
            "visible list selection takes priority during reload");

  ASSERT_EQ(70.0f,
            main_menu_library::centeredScrollOffsetForItem(2, 100, 108, 400),
            "selected chart is centered in a long list");
  ASSERT_EQ(300.0f,
            main_menu_library::centeredScrollOffsetForItem(9, 10, 50, 200),
            "selected chart scroll clamps at the list end");
  ASSERT_EQ(0.0f,
            main_menu_library::centeredScrollOffsetForItem(0, 10, 50, 200),
            "selected chart scroll clamps at the list start");
  ASSERT_EQ(0.0f,
            main_menu_library::centeredScrollOffsetForItem(-1, 10, 50, 200),
            "missing selection leaves scroll at the start");

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
  ScoreClearRankCache localCourseRanks = scoreRanks;
  {
    repository_test::ScopedStatementTrace observation(db, trace);
    data = repository_test::loadFolderClearDataByLongNoteMode(
        db, scoreRanks, localCourseRanks);
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
            folderRankForLn(data, "courses"),
            "local chart clear contributes to the course root");
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
