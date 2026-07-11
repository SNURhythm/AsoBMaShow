// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CourseIdentity.h"
#include "ThreadCompat.h"
#include "bms_parser.hpp"
#include "path.h"
#include "sqlite3.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class ChartRecordSortCriterion {
  Default,
  ClearMark,
  Score,
  Title,
  MinBpm,
  MaxBpm,
  MainBpm,
  Difficulty,
};

enum class ChartRecordSortDirection {
  Ascending,
  Descending,
};

struct ChartRecordSortState {
  ChartRecordSortCriterion criterion = ChartRecordSortCriterion::Default;
  ChartRecordSortDirection direction = ChartRecordSortDirection::Descending;
};

struct ChartMetaQuery {
  std::string keyword;
  int tableId = 0;
  std::string tableLevel;
  bool coursesOnly = false;
  bool solidArchivesOnly = false;
  int courseId = 0;
  int courseTableId = 0;
  std::string courseGroupName;
  bool clearMarkFilter = false;
  int clearMarkRank = -1;
  bool clearMarkOrAbove = false;
  bool clearMarkOrBelow = false;
  std::optional<std::string> scoreRank;
  bool scoreRankOrAbove = false;
  bool scoreRankOrBelow = false;
  std::optional<double> bpmMin;
  std::optional<double> bpmMax;
  std::optional<std::string> difficultyMinLevel;
  std::optional<std::string> difficultyMaxLevel;
  ChartRecordSortCriterion sortCriterion = ChartRecordSortCriterion::Default;
  ChartRecordSortDirection sortDirection =
      ChartRecordSortDirection::Descending;
  int selectedLongNoteMode = 1;
  bool favoritesOnly = false;
  int limit = 0;
  int offset = 0;
};

struct ChartMetaRecord {
  bms_parser::ChartMeta meta;
  std::string difficultyTableLabels;
  bool courseStart = false;
  bool unavailable = false;
  bool solidArchive = false;
  std::uint64_t archiveSize = 0;
  std::uint64_t archiveUncompressedSize = 0;
  int archiveFileCount = 0;
  bool favorite = false;
};

struct MusicTrackRecord {
  bms_parser::ChartMeta representativeChart;
  int storedItemId = 0;
  int chartCount = 0;
  bool useChartPathIdentity = false;
};

struct ChartEntry {
  path_t path;
  std::string iosBookmark;
  bool removable = true;
};

struct DifficultyTableInfo {
  int id = 0;
  std::string name;
  std::string symbol;
  std::string sourceUrl;
  int chartCount = 0;
};

struct DifficultyLevelInfo {
  int tableId = 0;
  std::string tableName;
  std::string tableSymbol;
  std::string level;
  int chartCount = 0;
};

struct DifficultyCourseGroupInfo {
  int tableId = 0;
  std::string tableName;
  std::string groupName;
  int courseCount = 0;
  int singletonCourseId = 0;
  std::string singletonCourseKey;
  std::string singletonCourseLevel;
  std::string singletonCourseName;
  std::string singletonCourseConstraintJson;
};

struct DifficultyCourseTableInfo {
  int tableId = 0;
  std::string tableName;
  std::string tableSymbol;
};

struct DifficultyCourseInfo {
  int id = 0;
  std::string courseKey;
  int tableId = 0;
  std::string tableName;
  std::string groupName;
  std::string level;
  std::string name;
  std::string constraintJson;
};

struct DifficultyTableImportProgress {
  int current = 0;
  int total = 0;
  std::string tableName;
};

using DifficultyTableImportProgressCallback =
    std::function<void(const DifficultyTableImportProgress &)>;

enum class ChartScanProgressStage {
  Preparing,
  ScanningRoots,
  PreparingUpdates,
  RemovingDeleted,
  ParsingCharts,
  ReadingArchive,
};

struct ChartScanProgress {
  int current = 0;
  int total = 0;
  ChartScanProgressStage stage = ChartScanProgressStage::Preparing;
};

using ChartScanProgressCallback =
    std::function<void(const ChartScanProgress &)>;
using ChartScanPauseCallback = std::function<bool()>;
using ChartScanFlushRequestCallback = std::function<std::uint64_t()>;
using ChartScanFlushCompleteCallback = std::function<void(std::uint64_t)>;

/**
 *
 */
class ChartDBHelper {
public:
  // Singleton
  ChartDBHelper();

  ChartDBHelper(const ChartDBHelper &) {}

  ChartDBHelper &operator=(const ChartDBHelper &) { return *this; }

  static ChartDBHelper &GetInstance() {
    sqlite3_config(SQLITE_CONFIG_SERIALIZED);
    static ChartDBHelper instance;
    return instance;
  }

  // Connect, return connection
  sqlite3 *Connect();

  // CreateTable
  bool CreateChartMetaTable(sqlite3 *db);
  bool CreateSolidArchiveTable(sqlite3 *db);
  bool CreateFavoritesTable(sqlite3 *db);
  bool CreateChartStateTables(sqlite3 *db);

  // Insert ChartMeta
  bool InsertChartMeta(sqlite3 *db, bms_parser::ChartMeta &chartMeta);
  int CountAllChartMeta(sqlite3 *db);
  int CountSolidArchives(sqlite3 *db);
  void SelectAllChartMeta(sqlite3 *db,
                          std::vector<bms_parser::ChartMeta> &chartMetas);
  void SelectFavoriteMusicTracks(sqlite3 *db,
                                 std::vector<MusicTrackRecord> &tracks);
  int CountFavoriteCharts(sqlite3 *db);
  bool SetFavorite(sqlite3 *db, const bms_parser::ChartMeta &chartMeta,
                   bool favorite);
  void QueryChartMeta(sqlite3 *db, const ChartMetaQuery &query,
                      std::vector<ChartMetaRecord> &chartMetas);
  int CountChartMeta(sqlite3 *db, const ChartMetaQuery &query);
  int FindChartMetaIndex(sqlite3 *db, const ChartMetaQuery &query,
                         const std::filesystem::path &path);
  bool DeleteChartMeta(sqlite3 *db, std::filesystem::path path);
  int DeleteChartMetaInDirectory(sqlite3 *db,
                                 const std::filesystem::path &directory);
  bool DeleteArchiveRecords(sqlite3 *db,
                            const std::filesystem::path &archivePath);
  bool ClearChartMeta(sqlite3 *db);
  void Close(sqlite3 *db);
  bool CreateEntriesTable(sqlite3 *db);
  bool InsertEntry(sqlite3 *db, const std::filesystem::path &path,
                   const std::string &iosBookmark = "");
  std::vector<ChartEntry> SelectAllEntries(sqlite3 *db);
  std::vector<ChartEntry> SelectEffectiveEntries(sqlite3 *db);
  bool DeleteEntry(sqlite3 *db, const std::filesystem::path &path);
  bool ClearEntries(sqlite3 *db);
  static std::filesystem::path DefaultBmsFolderPath();
  static bool IsDefaultBmsFolderPath(const std::filesystem::path &path);
  int ScanChartRoots(sqlite3 *db,
                     const std::vector<std::filesystem::path> &roots,
                     const std::stop_token *stopToken = nullptr,
                     ChartScanProgressCallback progressCallback = nullptr,
                     ChartScanPauseCallback pauseCallback = nullptr,
                     ChartScanFlushRequestCallback flushRequestCallback =
                         nullptr,
                     ChartScanFlushCompleteCallback flushCompleteCallback =
                         nullptr);

  bool CreateDifficultyTableTables(sqlite3 *db);
  bool ImportDifficultyTable(sqlite3 *db, const std::string &headerJson,
                             const std::string &dataJson,
                             const std::string &sourceUrl = "");
  bool ImportDifficultyTableFromUrl(sqlite3 *db, const std::string &pageUrl,
                                    std::string *errorMessage = nullptr,
                                    DifficultyTableImportProgressCallback
                                        progressCallback = nullptr);
  bool UpdateDifficultyTableFromSourceUrl(sqlite3 *db, int tableId,
                                          std::string *errorMessage = nullptr);
  bool DeleteDifficultyTable(sqlite3 *db, int tableId);
  int ImportDifficultyTablesFromDirectory(
      sqlite3 *db, const std::filesystem::path &directory);
  std::vector<DifficultyTableInfo> SelectDifficultyTables(sqlite3 *db);
  std::vector<DifficultyLevelInfo> SelectDifficultyLevels(sqlite3 *db,
                                                          int tableId);
  std::vector<DifficultyCourseTableInfo>
  SelectDifficultyCourseTables(sqlite3 *db);
  std::vector<DifficultyCourseGroupInfo>
  SelectDifficultyCourseGroups(sqlite3 *db, int tableId);
  std::vector<DifficultyCourseInfo>
  SelectDifficultyCourses(sqlite3 *db, int tableId,
                          const std::string &groupName);
  std::vector<course_identity::Definition>
  SelectDifficultyCourseDefinitions(sqlite3 *db);
  std::string DifficultyTableLabelsForChart(
      const bms_parser::ChartMeta &meta);
  std::string DifficultyTableLabelsForChart(
      sqlite3 *db, const bms_parser::ChartMeta &meta);
  [[nodiscard]] std::uint64_t GetLibraryRevision() const;

  static std::string StoredChartPathText(std::filesystem::path path);
  static void ToRelativePath(std::filesystem::path &path);
  static void ToAbsolutePath(std::filesystem::path &path);

private:
  bms_parser::ChartMeta ReadChartMeta(sqlite3_stmt *stmt);
  ChartMetaRecord ReadChartMetaRecord(sqlite3_stmt *stmt);
  path_t ReadPath(sqlite3_stmt *stmt, int idx) {
#ifdef _WIN32
    if (sqlite3_column_type(stmt, idx) == SQLITE_NULL) {
      return {};
    }
    int n = sqlite3_column_bytes(stmt, idx);
    const auto utf8 = std::string(
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx)), n);
    const path_t t = utf8_to_path_t(utf8);
#else
    const auto *text =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
    const path_t t = text != nullptr ? path_t(text) : path_t();
#endif
    return t;
  }
};
