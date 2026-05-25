// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "bms_parser.hpp"
#include "path.h"
#include "sqlite3.h"
#include <filesystem>
#include <string>
#include <vector>

struct ChartMetaQuery {
  std::string keyword;
  int tableId = 0;
  std::string tableLevel;
  bool coursesOnly = false;
  int courseId = 0;
  int courseTableId = 0;
  std::string courseGroupName;
  std::string difficultyText;
};

struct DifficultyTableInfo {
  int id = 0;
  std::string name;
  std::string symbol;
  std::string sourceUrl;
  int matchedChartCount = 0;
};

struct DifficultyLevelInfo {
  int tableId = 0;
  std::string tableName;
  std::string tableSymbol;
  std::string level;
  int matchedChartCount = 0;
};

struct DifficultyCourseGroupInfo {
  int tableId = 0;
  std::string tableName;
  std::string groupName;
  int matchedChartCount = 0;
};

struct DifficultyCourseInfo {
  int id = 0;
  int tableId = 0;
  std::string tableName;
  std::string groupName;
  std::string level;
  std::string name;
  int matchedChartCount = 0;
};
/**
 *
 */
class ChartDBHelper {
public:
  // Singleton
  ChartDBHelper() {}

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

  // Insert ChartMeta
  bool InsertChartMeta(sqlite3 *db, bms_parser::ChartMeta &chartMeta);
  void SelectAllChartMeta(sqlite3 *db,
                          std::vector<bms_parser::ChartMeta> &chartMetas);
  void SearchChartMeta(sqlite3 *db, const std::string &keyword,
                       std::vector<bms_parser::ChartMeta> &chartMetas);
  void QueryChartMeta(sqlite3 *db, const ChartMetaQuery &query,
                      std::vector<bms_parser::ChartMeta> &chartMetas);
  bool DeleteChartMeta(sqlite3 *db, std::filesystem::path path);
  bool ClearChartMeta(sqlite3 *db);
  void Close(sqlite3 *db);
  void BeginTransaction(sqlite3 *db);
  void CommitTransaction(sqlite3 *db);
  bool CreateEntriesTable(sqlite3 *db);
  bool InsertEntry(sqlite3 *db, const std::filesystem::path &path);
  std::vector<path_t> SelectAllEntries(sqlite3 *db);
  bool DeleteEntry(sqlite3 *db, const std::filesystem::path &path);
  bool ClearEntries(sqlite3 *db);

  bool CreateDifficultyTableTables(sqlite3 *db);
  bool ImportDifficultyTable(sqlite3 *db, const std::string &headerJson,
                             const std::string &dataJson,
                             const std::string &sourceUrl = "");
  int ImportDifficultyTablesFromDirectory(
      sqlite3 *db, const std::filesystem::path &directory);
  std::vector<DifficultyTableInfo> SelectDifficultyTables(sqlite3 *db);
  std::vector<DifficultyLevelInfo> SelectDifficultyLevels(sqlite3 *db,
                                                          int tableId);
  std::vector<DifficultyCourseGroupInfo>
  SelectDifficultyCourseGroups(sqlite3 *db);
  std::vector<DifficultyCourseInfo>
  SelectDifficultyCourses(sqlite3 *db, int tableId,
                          const std::string &groupName);

  static void ToRelativePath(std::filesystem::path &path);
  static void ToAbsolutePath(std::filesystem::path &path);

private:
  bms_parser::ChartMeta ReadChartMeta(sqlite3_stmt *stmt);
  path_t ReadPath(sqlite3_stmt *stmt, int idx) {
#ifdef _WIN32
    int n = sqlite3_column_bytes(stmt, idx);
    const auto utf8 = std::string(
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx)), n);
    const path_t t = utf8_to_path_t(utf8);
#else
    const path_t t =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
#endif
    return t;
  }
};
