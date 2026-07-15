// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "../CourseIdentity.h"
#include "../LibraryFolderClearData.h"
#include "../ThreadCompat.h"
#include "../bms_parser.hpp"
#include "../path.h"
#include "../sqlite3.h"
#include "ScoreRepositoryModels.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ScoreRepository;

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
class ChartRepository {
public:
  class Session {
  public:
    ~Session();
    Session(Session &&) noexcept;
    Session &operator=(Session &&) noexcept;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    bool EnsureSchema();
    bool InsertChartMeta(bms_parser::ChartMeta &chartMeta);
    int CountAllChartMeta();
    int CountSolidArchives();
    void SelectAllChartMeta(std::vector<bms_parser::ChartMeta> &chartMetas);
    void SelectFavoriteMusicTracks(std::vector<MusicTrackRecord> &tracks);
    int CountFavoriteCharts();
    bool SetFavorite(const bms_parser::ChartMeta &chartMeta, bool favorite);
    void QueryChartMeta(const ChartMetaQuery &query,
                        std::vector<ChartMetaRecord> &chartMetas);
    int CountChartMeta(const ChartMetaQuery &query);
    int FindChartMetaIndex(const ChartMetaQuery &query,
                           const std::filesystem::path &path);
    bool DeleteChartMeta(std::filesystem::path path);
    int DeleteChartMetaInDirectory(const std::filesystem::path &directory);
    bool DeleteArchiveRecords(const std::filesystem::path &archivePath);
    bool ClearChartMeta();
    bool InsertEntry(const std::filesystem::path &path,
                     const std::string &iosBookmark = "");
    std::vector<ChartEntry> SelectAllEntries();
    std::vector<ChartEntry> SelectEffectiveEntries();
    bool DeleteEntry(const std::filesystem::path &path);
    bool DeleteEntryAndChartMetaInDirectory(
        const std::filesystem::path &path, int &removedChartCount);
    bool ClearEntries();
    int ScanChartRoots(
        const std::vector<std::filesystem::path> &roots,
        const std::stop_token *stopToken = nullptr,
        ChartScanProgressCallback progressCallback = nullptr,
        ChartScanPauseCallback pauseCallback = nullptr,
        ChartScanFlushRequestCallback flushRequestCallback = nullptr,
        ChartScanFlushCompleteCallback flushCompleteCallback = nullptr);
    bool ImportDifficultyTable(const std::string &headerJson,
                               const std::string &dataJson,
                               const std::string &sourceUrl = "");
    bool ImportDifficultyTableFromUrl(
        const std::string &pageUrl, std::string *errorMessage = nullptr,
        DifficultyTableImportProgressCallback progressCallback = nullptr);
    bool UpdateDifficultyTableFromSourceUrl(
        int tableId, std::string *errorMessage = nullptr);
    bool DeleteDifficultyTable(int tableId);
    int ImportDifficultyTablesFromDirectory(
        const std::filesystem::path &directory);
    std::vector<DifficultyTableInfo> SelectDifficultyTables();
    std::vector<DifficultyLevelInfo> SelectDifficultyLevels(int tableId);
    std::vector<DifficultyCourseTableInfo> SelectDifficultyCourseTables();
    std::vector<DifficultyCourseGroupInfo>
    SelectDifficultyCourseGroups(int tableId);
    std::vector<DifficultyCourseInfo>
    SelectDifficultyCourses(int tableId, const std::string &groupName);
    std::vector<course_identity::Definition>
    SelectDifficultyCourseDefinitions();
    std::string
    DifficultyTableLabelsForChart(const bms_parser::ChartMeta &meta);
    chart_library::FolderClearDataByLongNoteMode
    LoadFolderClearDataByLongNoteMode(const ScoreClearRankCache &scoreRanks);

  private:
    friend class ChartRepository;
    friend class ScoreRepository;
    struct Impl;
    explicit Session(std::unique_ptr<Impl> impl);
    sqlite3 *NativeHandleForScoreRepository() const;
    std::unique_ptr<Impl> impl_;
  };

  ChartRepository();
  explicit ChartRepository(std::filesystem::path databasePath);
  ~ChartRepository();
  ChartRepository(const ChartRepository &) = delete;
  ChartRepository &operator=(const ChartRepository &) = delete;

  bool EnsureReady();
  std::optional<Session> OpenSession(ScoreRepository *scores = nullptr);
  [[nodiscard]] const std::filesystem::path &DatabasePath() const;
  [[nodiscard]] std::uint64_t GetLibraryRevision() const;
  static std::filesystem::path DefaultBmsFolderPath();
  static bool IsDefaultBmsFolderPath(const std::filesystem::path &path);

private:
  // Insert ChartMeta
  bool InsertChartMeta(sqlite3 *db, bms_parser::ChartMeta &chartMeta);
  bool DeleteChartMeta(sqlite3 *db, std::filesystem::path path);
  int DeleteChartMetaInDirectory(sqlite3 *db,
                                 const std::filesystem::path &directory);
  bool DeleteArchiveRecords(sqlite3 *db,
                            const std::filesystem::path &archivePath);
  bool ClearChartMeta(sqlite3 *db);
  bool InsertEntry(sqlite3 *db, const std::filesystem::path &path,
                   const std::string &iosBookmark = "");
  std::vector<ChartEntry> SelectAllEntries(sqlite3 *db);
  std::vector<ChartEntry> SelectEffectiveEntries(sqlite3 *db);
  bool DeleteEntry(sqlite3 *db, const std::filesystem::path &path);
  bool ClearEntries(sqlite3 *db);
  int ScanChartRoots(sqlite3 *db,
                     const std::vector<std::filesystem::path> &roots,
                     const std::stop_token *stopToken = nullptr,
                     ChartScanProgressCallback progressCallback = nullptr,
                     ChartScanPauseCallback pauseCallback = nullptr,
                     ChartScanFlushRequestCallback flushRequestCallback =
                         nullptr,
                     ChartScanFlushCompleteCallback flushCompleteCallback =
                         nullptr);

  bool ImportDifficultyTable(sqlite3 *db, const std::string &headerJson,
                             const std::string &dataJson,
                             const std::string &sourceUrl = "");
  bool ImportDifficultyTableFromUrl(sqlite3 *db, const std::string &pageUrl,
                                    std::string *errorMessage = nullptr,
                                    DifficultyTableImportProgressCallback
                                        progressCallback = nullptr);
  bool UpdateDifficultyTableFromSourceUrl(sqlite3 *db, int tableId,
                                          std::string *errorMessage = nullptr);
  int ImportDifficultyTablesFromDirectory(
      sqlite3 *db, const std::filesystem::path &directory);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
