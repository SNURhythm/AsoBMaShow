// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "../CourseIdentity.h"
#include "../DifficultyTableModel.h"
#include "../LibraryFolderClearData.h"
#include "../bms_parser.hpp"
#include "../path.h"
#include "ChartScanStore.h"
#include "ScoreRepositoryModels.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
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
  std::optional<std::filesystem::path> exactFolder;
  ChartRecordSortCriterion sortCriterion = ChartRecordSortCriterion::Default;
  ChartRecordSortDirection sortDirection =
      ChartRecordSortDirection::Descending;
  int selectedLongNoteMode = 1;
  bool favoritesOnly = false;
  int limit = 0;
  int offset = 0;

  bool operator==(const ChartMetaQuery &) const = default;
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

enum class ChartMetaPathBatchReadStatus { Loaded, Invalid, StorageFailure };

struct ChartMetaPathBatchReadOutcome {
  ChartMetaPathBatchReadStatus status =
      ChartMetaPathBatchReadStatus::StorageFailure;
  std::vector<ChartMetaRecord> records;
  std::size_t missingPaths = 0;
  std::string diagnostic;
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
  bool primaryStorageFolder = false;
  bool primaryStorageEligible = false;
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

/**
 *
 */
class ChartRepository {
public:
  class Session {
  public:
    class ScanBatch {
    public:
      ~ScanBatch();
      ScanBatch(ScanBatch &&) noexcept;
      ScanBatch &operator=(ScanBatch &&) noexcept;
      ScanBatch(const ScanBatch &) = delete;
      ScanBatch &operator=(const ScanBatch &) = delete;

      bool UpsertChart(
          const bms_parser::ChartMeta &meta,
          std::optional<ChartSourcePreference> sourcePreference);
      bool DeleteChart(const std::filesystem::path &path);
      bool DeleteCharts(std::span<const std::filesystem::path> paths);
      bool DeleteChartsInArchive(const std::filesystem::path &path);
      bool DeleteSolidArchive(const std::filesystem::path &path);
      bool DeleteArchiveCache(const std::filesystem::path &path);
      bool UpsertSolidArchive(const SolidArchiveUpdate &update);
      bool UpsertArchiveCache(const ArchiveScanCacheUpdate &update);
      bool UpdateSourcePreference(
          const ChartSourcePreferenceUpdate &update);
      std::optional<int>
      CountChartsInArchive(const std::filesystem::path &path);
      bool CheckpointAndContinue(const ChartScanCheckpoint &checkpoint);
      bool Commit();
      int ChangedCount() const;

    private:
      friend class Session;
      struct Impl;
      explicit ScanBatch(std::unique_ptr<Impl> impl);
      std::unique_ptr<Impl> impl_;
    };

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
    ChartMetaPathBatchReadOutcome SelectChartMetaByPaths(
        std::span<const std::filesystem::path> paths);
    std::vector<bms_parser::ChartMeta>
    SelectChartMetaByHash(const std::string &sha256,
                          const std::string &md5);
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
    bool SetPrimaryStorageEntry(const std::filesystem::path &path);
    std::optional<ChartEntry> SelectPrimaryStorageEntry();
    bool DeleteEntry(const std::filesystem::path &path);
    bool DeleteEntryAndChartMetaInDirectory(
        const std::filesystem::path &path, int &removedChartCount);
    bool ClearEntries();
    ChartScanSnapshot LoadScanSnapshot(
        ChartScanSnapshotLoad load = ChartScanSnapshotLoad::Full);
    std::optional<ScanBatch> BeginScanBatch();
    bool ClearScanCheckpoint();
    bool ClearChartMetadataRebuildRequired();
    bool ReplaceDifficultyTable(const difficulty_table::Document &document);
    bool DeleteDifficultyTable(int tableId);
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
    LoadFolderClearDataByLongNoteMode(
        const ScoreClearRankCache &projectedChartRanks,
        const ScoreClearRankCache &localCourseRanks);

  private:
    friend class ChartRepository;
    friend class ScoreRepository;
    struct Impl;
    explicit Session(std::unique_ptr<Impl> impl);
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
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
