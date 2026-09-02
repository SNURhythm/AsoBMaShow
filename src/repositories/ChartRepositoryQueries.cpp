// Fill out your copyright notice in the Description page of Project Settings.

#include "ChartRepository.h"
#include "ChartRepositoryInternal.h"
#include "../BmsMetadataText.h"
#include "ChartMetaSql.h"
#include "ChartSqlExpressions.h"
#include "ChartStorageIdentity.h"
#include "../CanonicalDigest.h"
#include "../LongNoteModeUtils.h"
#include "../view/ClearLampColors.h"
#include "ScoreRepository.h"
#include "ScoreCacheQueries.h"
#include "SqliteRAII.h"
#include "../Utils.h"
#include "../yoga/lib/nlohmann/json.hpp"
#include <SDL2/SDL.h>
#include "../path.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if !defined(ASOBMASHOW_FOLDER_QUERY_ONLY)
namespace {
using asobmshow::chart_sql::chartArtworkOrderBy;
using asobmshow::chart_sql::chartSourceOrderBy;
using asobmshow::chart_sql::defaultChartMetaBeforeTargetPredicate;
using asobmshow::chart_sql::kChartMetaColumnCount;
using asobmshow::chart_sql::kChartMetaSelectColumns;
using asobmshow::chart_sql::matchedChartPathSubquery;
using asobmshow::chart_sql::normalizedSqlHash;
using asobmshow::chart_sql::preferredChartPredicate;
using asobmshow::chart_sql::sqlTextHasValue;

constexpr int kNoPlayClearMarkRank = -1;

constexpr const char *kDifficultyEntrySelectColumns =
    "COALESCE(cm.path, ''),"
    "COALESCE(NULLIF(dte.md5, ''), cm.md5, ''),"
    "COALESCE(NULLIF(dte.sha256, ''), cm.sha256, ''),"
    "COALESCE(NULLIF(cm.title, ''), NULLIF(dte.title, ''), "
    "NULLIF(dte.sha256, ''), NULLIF(dte.md5, ''), 'Untitled'),"
    "COALESCE(NULLIF(cm.subtitle, ''), dte.subtitle, ''),"
    "COALESCE(cm.genre, ''),"
    "COALESCE(NULLIF(cm.artist, ''), dte.artist, ''),"
    "COALESCE(NULLIF(cm.sub_artist, ''), dte.subartist, ''),"
    "COALESCE(cm.folder, ''),"
    "COALESCE(cm.stage_file, ''),"
    "COALESCE(cm.banner, ''),"
    "COALESCE(cm.back_bmp, ''),"
    "COALESCE(cm.preview, ''),"
    "COALESCE(cm.level, 0),"
    "COALESCE(cm.difficulty, 0),"
    "COALESCE(cm.total, 100),"
    "COALESCE(cm.has_total, 0),"
    "COALESCE(cm.bpm, 0),"
    "COALESCE(cm.max_bpm, 0),"
    "COALESCE(cm.min_bpm, 0),"
    "COALESCE(cm.length, 0),"
    "COALESCE(cm.rank, 3),"
    "COALESCE(cm.player, 1),"
    "COALESCE(cm.keys, 5),"
    "COALESCE(cm.total_notes, 0),"
    "COALESCE(cm.total_long_notes, 0),"
    "COALESCE(cm.total_scratch_notes, 0),"
    "COALESCE(cm.total_backspin_notes, 0),"
    "COALESCE(cm.ln_mode, 0),"
    "COALESCE(cm.has_document, 0),"
    "COALESCE(cm.has_bpm_stop, 0),"
    "COALESCE(cm.has_scroll_change, 0),"
    "COALESCE(cm.add_date, 0),"
    "dt.symbol || dte.level,"
    "CASE WHEN cm.path IS NULL THEN 1 ELSE 0 END";

constexpr const char *kDifficultyEntrySearchText =
    "rtrim(COALESCE(NULLIF(cm.title, ''), dte.title, '') || ' ' || "
    "COALESCE(NULLIF(cm.subtitle, ''), dte.subtitle, '') || ' ' || "
    "COALESCE(NULLIF(cm.artist, ''), dte.artist, '') || ' ' || "
    "COALESCE(NULLIF(cm.sub_artist, ''), dte.subartist, '') || ' ' || "
    "COALESCE(cm.genre, ''))";
constexpr const char *kDifficultyCourseEntrySelectColumns =
    "COALESCE(cm.path, ''),"
    "COALESCE(NULLIF(dce.md5, ''), NULLIF(dte.md5, ''), cm.md5, ''),"
    "COALESCE(NULLIF(dce.sha256, ''), NULLIF(dte.sha256, ''), cm.sha256, ''),"
    "COALESCE(NULLIF(cm.title, ''), NULLIF(dce.title, ''), "
    "NULLIF(dte.title, ''), NULLIF(dce.sha256, ''), NULLIF(dce.md5, ''), "
    "'Course chart ' || (dce.sort_order + 1)),"
    "COALESCE(NULLIF(cm.subtitle, ''), NULLIF(dce.subtitle, ''), "
    "dte.subtitle, ''),"
    "COALESCE(cm.genre, ''),"
    "COALESCE(NULLIF(cm.artist, ''), NULLIF(dce.artist, ''), dte.artist, ''),"
    "COALESCE(NULLIF(cm.sub_artist, ''), NULLIF(dce.subartist, ''), "
    "dte.subartist, ''),"
    "COALESCE(cm.folder, ''),"
    "COALESCE(cm.stage_file, ''),"
    "COALESCE(cm.banner, ''),"
    "COALESCE(cm.back_bmp, ''),"
    "COALESCE(cm.preview, ''),"
    "CASE WHEN cm.path IS NOT NULL THEN COALESCE(cm.level, 0) "
    "ELSE COALESCE(CAST(NULLIF(NULLIF(dce.level, ''), '0') AS REAL), "
    "CAST(NULLIF(dte.level, '') AS REAL), "
    "CAST(NULLIF(dce.level, '') AS REAL), 0) END,"
    "COALESCE(cm.difficulty, 0),"
    "COALESCE(cm.total, 100),"
    "COALESCE(cm.has_total, 0),"
    "COALESCE(cm.bpm, 0),"
    "COALESCE(cm.max_bpm, 0),"
    "COALESCE(cm.min_bpm, 0),"
    "COALESCE(cm.length, 0),"
    "COALESCE(cm.rank, 3),"
    "COALESCE(cm.player, 1),"
    "COALESCE(cm.keys, 5),"
    "COALESCE(cm.total_notes, 0),"
    "COALESCE(cm.total_long_notes, 0),"
    "COALESCE(cm.total_scratch_notes, 0),"
    "COALESCE(cm.total_backspin_notes, 0),"
    "COALESCE(cm.ln_mode, 0),"
    "COALESCE(cm.has_document, 0),"
    "COALESCE(cm.has_bpm_stop, 0),"
    "COALESCE(cm.has_scroll_change, 0),"
    "COALESCE(cm.add_date, 0),"
    "COALESCE(NULLIF(dt.symbol || NULLIF(NULLIF(dce.level, ''), '0'), "
    "dt.symbol), NULLIF(dt.symbol || NULLIF(dte.level, ''), dt.symbol), "
    "NULLIF(dt.symbol || NULLIF(dce.level, ''), dt.symbol), "
    "NULLIF(dt.symbol || NULLIF(dc.level, ''), dt.symbol), ''),"
    "CASE WHEN cm.path IS NULL THEN 1 ELSE 0 END";
constexpr const char *kDifficultyCourseEntrySearchText =
    "rtrim(COALESCE(NULLIF(cm.title, ''), NULLIF(dce.title, ''), "
    "dte.title, '') || ' ' || "
    "COALESCE(NULLIF(cm.subtitle, ''), NULLIF(dce.subtitle, ''), "
    "dte.subtitle, '') || ' ' || "
    "COALESCE(NULLIF(cm.artist, ''), NULLIF(dce.artist, ''), "
    "dte.artist, '') || ' ' || "
    "COALESCE(NULLIF(cm.sub_artist, ''), NULLIF(dce.subartist, ''), "
    "dte.subartist, '') || ' ' || "
    "COALESCE(cm.genre, '') || ' ' || dc.name || ' ' || dc.group_name)";
constexpr const char *kSolidArchiveSelectColumns =
    "sa.path, sa.name, sa.archive_size, sa.uncompressed_size, sa.file_count";

struct DifficultyLabelCache {
  bool loaded = false;
  std::unordered_map<std::string, std::vector<std::string>> labelsBySha256;
  std::unordered_map<std::string, std::vector<std::string>> labelsByMd5;
};

std::mutex gDifficultyLabelCacheMutex;
DifficultyLabelCache gDifficultyLabelCache;
std::atomic<std::uint64_t> gLibraryRevision{1};

std::string columnString(sqlite3_stmt *stmt, int idx);
void invalidateDifficultyLabelCache();

void bumpLibraryRevision() {
  gLibraryRevision.fetch_add(1, std::memory_order_relaxed);
}

using asobmshow::bms_metadata::lowerCopy;
using asobmshow::bms_metadata::normalizedHash;
using asobmshow::bms_metadata::trimCopy;

std::string sqlColumn(std::string_view alias, std::string_view column) {
  return std::string(alias) + "." + std::string(column);
}

std::string storedHashColumn(std::string_view alias, std::string_view column) {
  return sqlColumn(alias, column);
}

std::string sqlHashColumnHasValue(std::string_view alias,
                                  std::string_view column) {
  return sqlColumn(alias, column) + " != ''";
}

std::string chartFavoriteIdentityKey(const char *favoriteAlias);

std::string chartFavoriteChartCandidateBranch(const std::string &matchCondition,
                                              int identityRank) {
  std::string query = "SELECT cm.*, cf.added_at AS favorite_added_at, ";
  query += chartFavoriteIdentityKey("cf");
  query += " AS favorite_identity_key, ";
  query += std::to_string(identityRank);
  query += " AS identity_rank FROM chart_favorites cf JOIN chart_meta cm ON ";
  query += matchCondition;
  return query;
}

std::string chartFavoriteChartCandidateQuery() {
  const std::string prefix(asobmshow::chart_sql::kStoredDocumentsBmsPrefix);
  const std::string favoriteSha256 = "cf.chart_sha256";
  const std::string favoriteMd5 = "cf.chart_md5";
  const std::string favoritePath = "cf.chart_path";
  const std::string shaMatch =
      favoriteSha256 + " != '' AND cm.sha256 = " + favoriteSha256;
  const std::string md5Match =
      favoriteMd5 + " != '' AND cm.md5 = " + favoriteMd5;
  const std::string nonHashMatch =
      "NOT (" + shaMatch + ") AND NOT (" + md5Match + ")";

  std::vector<std::string> branches;
  branches.push_back(chartFavoriteChartCandidateBranch(shaMatch, 0));
  branches.push_back(chartFavoriteChartCandidateBranch(
      md5Match + " AND NOT (" + shaMatch + ")", 1));
  branches.push_back(chartFavoriteChartCandidateBranch(
      favoritePath + " != '' AND cm.path = " + favoritePath + " AND " +
          nonHashMatch,
      2));
  branches.push_back(chartFavoriteChartCandidateBranch(
      favoritePath + " != '' AND cm.path = '" + prefix + "' || " +
          favoritePath + " AND " + nonHashMatch,
      2));
  branches.push_back(chartFavoriteChartCandidateBranch(
      favoritePath + " LIKE '" + prefix + "%' AND cm.path = substr(" +
          favoritePath + ", length('" + prefix + "') + 1) AND " +
          nonHashMatch,
      2));

  std::string query;
  for (std::size_t i = 0; i < branches.size(); ++i) {
    if (i > 0) {
      query += " UNION ALL ";
    }
    query += branches[i];
  }
  return query;
}

std::string chartFavoriteIndexedPathPredicate(const std::string &chartPath) {
  const std::string prefix(asobmshow::chart_sql::kStoredDocumentsBmsPrefix);
  return chartPath +
         " != '' AND (EXISTS (SELECT 1 FROM chart_favorites cf_path WHERE "
         "cf_path.chart_path = " +
         chartPath +
         ") OR EXISTS (SELECT 1 FROM chart_favorites cf_path_prefixed WHERE "
         "cf_path_prefixed.chart_path = '" +
         prefix + "' || " + chartPath +
         ") OR (" + chartPath + " LIKE '" + prefix +
         "%' AND EXISTS (SELECT 1 FROM chart_favorites cf_path_legacy WHERE "
         "cf_path_legacy.chart_path = substr(" +
         chartPath + ", length('" + prefix + "') + 1))))";
}

std::string chartFavoriteIndexedPredicate(const char *chartAlias) {
  const std::string alias(chartAlias);
  const std::string path = alias + ".path";
  const std::string sha256 = alias + ".sha256";
  const std::string md5 = alias + ".md5";
  return "((" + chartFavoriteIndexedPathPredicate(path) + ") OR (" + sha256 +
         " != '' AND EXISTS (SELECT 1 FROM chart_favorites cf_sha WHERE "
         "cf_sha.chart_sha256 = " +
         sha256 +
         ")) OR (" + md5 +
         " != '' AND EXISTS (SELECT 1 FROM chart_favorites cf_md5 WHERE "
         "cf_md5.chart_md5 = " +
         md5 + ")))";
}

std::string chartFavoriteColumnExpr(const char *chartAlias) {
  return "CASE WHEN " + chartFavoriteIndexedPredicate(chartAlias) +
         " THEN 1 ELSE 0 END";
}

std::string songReviewFavoriteColumnExpr(const char *chartAlias) {
  const std::string alias(chartAlias);
  return "COALESCE((SELECT sr.favorite FROM review sr WHERE sr.sha256 = " +
         alias + ".sha256), 0)";
}

std::string chartFavoriteIdentityKey(const char *favoriteAlias) {
  const std::string alias(favoriteAlias);
  return "COALESCE(NULLIF(" + alias + ".chart_sha256, ''), NULLIF(" + alias +
         ".chart_md5, ''), " + alias + ".chart_path)";
}

struct ChartFavoriteIdentity {
  std::string chartPath;
  std::string sha256;
  std::string md5;
};

ChartFavoriteIdentity chartFavoriteIdentityFor(
    const bms_parser::ChartMeta &chartMeta) {
  return {
      .chartPath = chart_storage_identity::StoredPathText(chartMeta.BmsPath),
      .sha256 = normalizedHash(chartMeta.SHA256),
      .md5 = normalizedHash(chartMeta.MD5),
  };
}

std::string chartFavoriteDeletePredicate() {
  return "chart_path = ?1 OR (?2 != '' AND chart_sha256 = ?2) "
         "OR (?3 != '' AND chart_md5 = ?3)";
}

void bindChartFavoriteDeleteIdentity(sqlite3_stmt *stmt,
                                     const ChartFavoriteIdentity &identity) {
  bindSqliteText(stmt, 1, identity.chartPath);
  bindSqliteText(stmt, 2, identity.sha256);
  bindSqliteText(stmt, 3, identity.md5);
}

void logSqlErrorText(const char *context, const std::string &error);
void logSqlError(const char *context, sqlite3 *db);

bool updateSongReviewChartFavorite(sqlite3 *db, std::string_view sha256,
                                   bool favorite, bool &changed) {
  changed = false;
  // SongReviewAccessor only persists a review when SongData supplies a
  // non-empty SHA-256.  Keep a path-only Aso library favourite usable by its
  // existing UI, but do not manufacture a review identity for it.
  if (sha256.empty()) {
    return true;
  }

  const char *query =
      favorite
          ? "INSERT INTO review(sha256, favorite) VALUES(?1, 2) "
            "ON CONFLICT(sha256) DO UPDATE SET "
            "favorite = review.favorite | excluded.favorite"
          : "UPDATE review SET favorite = favorite & ~2 WHERE sha256 = ?1";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing SongReview chart favorite",
                                    logSqlErrorText) ||
      !bindSqliteText(stmt.get(), 1, std::string(sha256))) {
    return false;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    logSqlError("updating SongReview chart favorite", db);
    return false;
  }
  changed = sqlite3_changes(db) > 0;
  return true;
}

bool queryNeedsDifficultyTableSchema(const ChartMetaQuery &query) {
  return query.tableId > 0 || query.coursesOnly || query.courseId > 0 ||
         query.courseTableId > 0 || !query.courseGroupName.empty() ||
         query.difficultyMinLevel.has_value() ||
         query.difficultyMaxLevel.has_value();
}
void logSqlErrorText(const char *context, const std::string &error) {
  std::cerr << "SQL error while " << context << ": " << error << "\n";
}

void logSqlError(const char *context, sqlite3 *db) {
  logSqlErrorText(context, sqliteDatabaseError(db));
}
std::string scoreLongNoteModeExpr(const std::string &alias,
                                  int selectedLongNoteMode) {
  const std::string selected =
      std::to_string(
          long_note_mode::normalizeSelectedValue(selectedLongNoteMode));
  const std::string lnModeExpr = "COALESCE(" + alias + ".ln_mode, 0)";
  return "(CASE WHEN COALESCE(" + alias + ".total_long_notes, 0) + COALESCE(" +
         alias + ".total_backspin_notes, 0) <= 0 THEN 0 "
         "WHEN " +
         long_note_mode::sqlValidValuePredicate(lnModeExpr) + " THEN " +
         alias + ".ln_mode ELSE " + selected + " END)";
}

std::string scoreRankLookupExpr(const std::string &sha256Expr,
                                const std::string &lnModeExpr) {
  return score_cache_queries::scoreRankLookupExpr(sha256Expr, lnModeExpr);
}

std::string scoreBestLookupExpr(const std::string &sha256Expr,
                                const std::string &lnModeExpr,
                                const std::string &column) {
  return score_cache_queries::scoreBestLookupExpr(sha256Expr, lnModeExpr,
                                                  column);
}

std::string scoreRankOrderExpr(const std::string &scoreRankExpr) {
  return "(CASE " + scoreRankExpr +
         " WHEN 'MAX' THEN 9"
         " WHEN 'MAX -' THEN 8"
         " WHEN 'AAA' THEN 7"
         " WHEN 'AA' THEN 6"
         " WHEN 'A' THEN 5"
         " WHEN 'B' THEN 4"
         " WHEN 'C' THEN 3"
         " WHEN 'D' THEN 2"
         " WHEN 'E' THEN 1"
         " WHEN 'F' THEN 0"
         " ELSE NULL END)";
}

std::string chartClearMarkRankExpr(const std::string &alias,
                                   int selectedLongNoteMode) {
  const std::string lnModeExpr =
      scoreLongNoteModeExpr(alias, selectedLongNoteMode);
  return "COALESCE(" +
         scoreRankLookupExpr(storedHashColumn(alias, "sha256"), lnModeExpr) +
         ", " +
         std::to_string(kNoPlayClearMarkRank) + ")";
}

std::string chartClearMarkPredicate(const std::string &alias,
                                    const ChartMetaQuery &chartQuery) {
  std::string comparison = " = ";
  if (chartQuery.clearMarkOrAbove) {
    comparison = " >= ";
  } else if (chartQuery.clearMarkOrBelow) {
    comparison = " <= ";
  }
  return chartClearMarkRankExpr(alias, chartQuery.selectedLongNoteMode) +
         comparison + "@clear_mark_rank";
}

std::string difficultyEntryClearMarkRankExpr(
    const std::string &entryAlias, const std::string &chartAlias,
    int selectedLongNoteMode) {
  const std::string lnModeExpr =
      scoreLongNoteModeExpr(chartAlias, selectedLongNoteMode);
  return "COALESCE(" +
         scoreRankLookupExpr(storedHashColumn(entryAlias, "sha256"),
                             lnModeExpr) +
         ", " +
         scoreRankLookupExpr(storedHashColumn(chartAlias, "sha256"),
                             lnModeExpr) +
         ", " + std::to_string(kNoPlayClearMarkRank) + ")";
}

std::string difficultyEntryClearMarkPredicate(const std::string &entryAlias,
                                             const std::string &chartAlias,
                                             const ChartMetaQuery &chartQuery) {
  std::string comparison = " = ";
  if (chartQuery.clearMarkOrAbove) {
    comparison = " >= ";
  } else if (chartQuery.clearMarkOrBelow) {
    comparison = " <= ";
  }
  return difficultyEntryClearMarkRankExpr(entryAlias, chartAlias,
                                          chartQuery.selectedLongNoteMode) +
         comparison + "@clear_mark_rank";
}

std::string chartBestScoreExpr(const std::string &alias,
                               const std::string &column,
                               int selectedLongNoteMode) {
  const std::string lnModeExpr =
      scoreLongNoteModeExpr(alias, selectedLongNoteMode);
  return scoreBestLookupExpr(storedHashColumn(alias, "sha256"), lnModeExpr,
                             column);
}

std::string difficultyEntryBestScoreExpr(const std::string &entryAlias,
                                         const std::string &chartAlias,
                                         const std::string &column,
                                         int selectedLongNoteMode) {
  const std::string lnModeExpr =
      scoreLongNoteModeExpr(chartAlias, selectedLongNoteMode);
  return "COALESCE(" +
         scoreBestLookupExpr(storedHashColumn(entryAlias, "sha256"),
                             lnModeExpr, column) +
         ", " +
         scoreBestLookupExpr(storedHashColumn(chartAlias, "sha256"),
                             lnModeExpr, column) +
         ")";
}

std::string effectiveMinBpmExpr(const std::string &alias) {
  return "(CASE WHEN COALESCE(" + alias +
         ".min_bpm, 0) > 0 THEN " + alias +
         ".min_bpm WHEN COALESCE(" + alias + ".max_bpm, 0) > 0 THEN " +
         alias + ".max_bpm ELSE COALESCE(" + alias + ".bpm, 0) END)";
}

std::string effectiveMaxBpmExpr(const std::string &alias) {
  return "(CASE WHEN COALESCE(" + alias +
         ".max_bpm, 0) > 0 THEN " + alias +
         ".max_bpm WHEN COALESCE(" + alias + ".min_bpm, 0) > 0 THEN " +
         alias + ".min_bpm ELSE COALESCE(" + alias + ".bpm, 0) END)";
}

std::string mainBpmExpr(const std::string &alias) {
  return "COALESCE(" + alias + ".bpm, 0)";
}

bool chartMetaQueryHasCourseFilter(const ChartMetaQuery &chartQuery) {
  return chartQuery.coursesOnly || chartQuery.courseId > 0 ||
         chartQuery.courseTableId > 0 || !chartQuery.courseGroupName.empty();
}

bool chartMetaQueryUsesDifficultyEntries(const ChartMetaQuery &chartQuery) {
  return chartQuery.tableId > 0 && !chartQuery.coursesOnly &&
         chartQuery.courseId <= 0 && chartQuery.courseTableId <= 0 &&
         chartQuery.courseGroupName.empty();
}

bool chartMetaQueryUsesCourseEntries(const ChartMetaQuery &chartQuery) {
  return chartQuery.coursesOnly || chartQuery.courseId > 0 ||
         chartQuery.courseTableId > 0 || !chartQuery.courseGroupName.empty();
}

bool chartMetaQueryHasBpmFilter(const ChartMetaQuery &chartQuery) {
  return chartQuery.bpmMin.has_value() || chartQuery.bpmMax.has_value();
}

bool chartMetaQueryHasScoreFilter(const ChartMetaQuery &chartQuery) {
  return chartQuery.scoreRank.has_value();
}

bool chartMetaQueryNeedsBestScore(const ChartMetaQuery &chartQuery) {
  return chartMetaQueryHasScoreFilter(chartQuery) ||
         chartQuery.sortCriterion == ChartRecordSortCriterion::Score;
}

bool chartMetaQueryNeedsScoreCache(const ChartMetaQuery &chartQuery) {
  return chartQuery.clearMarkFilter || chartMetaQueryHasScoreFilter(chartQuery) ||
         chartQuery.sortCriterion == ChartRecordSortCriterion::ClearMark ||
         chartQuery.sortCriterion == ChartRecordSortCriterion::Score;
}

bool chartMetaQueryNeedsChartJoinForDifficultyEntries(
    const ChartMetaQuery &chartQuery) {
  return !chartQuery.keyword.empty() || chartQuery.clearMarkFilter ||
         chartQuery.exactFolder.has_value() ||
         chartMetaQueryHasBpmFilter(chartQuery) ||
         chartMetaQueryHasScoreFilter(chartQuery) ||
         chartMetaQueryNeedsBestScore(chartQuery);
}

bool chartMetaQueryNeedsChartJoinForCourseEntries(
    const ChartMetaQuery &chartQuery) {
  return !chartQuery.keyword.empty() || chartQuery.clearMarkFilter ||
         chartQuery.exactFolder.has_value() ||
         chartMetaQueryHasBpmFilter(chartQuery) ||
         chartMetaQueryHasScoreFilter(chartQuery) ||
         chartMetaQueryNeedsBestScore(chartQuery);
}

void appendExactFolderFilter(std::string &query, const std::string &chartAlias,
                             const ChartMetaQuery &chartQuery) {
  if (chartQuery.exactFolder.has_value()) {
    const std::string normalizedPath =
        "replace(" + chartAlias + ".path, '\\', '/')";
    const auto folderPrefix =
        "(rtrim(replace(@exact_folder, '\\', '/'), '/') || '/')";
    const std::string pathParentMatches =
        "substr(" + normalizedPath + ", 1, length(" + folderPrefix + ")) = " +
        folderPrefix + " AND instr(substr(" + normalizedPath + ", length(" +
        folderPrefix + ") + 1), '/') = 0";
    query += " AND (" + chartAlias + ".folder = @exact_folder OR (" +
             chartAlias + ".folder = '' AND " + pathParentMatches + ") OR (" +
             chartAlias + ".folder IS NULL AND " + pathParentMatches + "))";
  }
}

void bindExactFolderFilter(sqlite3_stmt *stmt, int &bindIndex,
                           const ChartMetaQuery &chartQuery) {
  if (chartQuery.exactFolder.has_value()) {
    bindSqliteText(
        stmt, bindIndex++,
        chart_storage_identity::StoredFolderPathText(*chartQuery.exactFolder));
  }
}

void appendBpmFilters(std::string &query, const std::string &chartAlias,
                      const ChartMetaQuery &chartQuery) {
  if (!chartMetaQueryHasBpmFilter(chartQuery)) {
    return;
  }
  query += " AND " + chartAlias + ".path IS NOT NULL";
  if (chartQuery.bpmMin.has_value()) {
    query += " AND " + effectiveMaxBpmExpr(chartAlias) + " >= @bpm_min";
  }
  if (chartQuery.bpmMax.has_value()) {
    query += " AND " + effectiveMinBpmExpr(chartAlias) + " <= @bpm_max";
  }
}

void appendDifficultyLevelRangeFilter(std::string &query,
                                      const ChartMetaQuery &chartQuery) {
  if (!chartQuery.difficultyMinLevel.has_value() &&
      !chartQuery.difficultyMaxLevel.has_value()) {
    return;
  }
  query +=
      " AND dte.level IN (SELECT dl.level FROM ("
      "SELECT level, MIN(sort_order) AS level_sort "
      "FROM difficulty_table_entries WHERE table_id = @table_id "
      "GROUP BY level) dl WHERE 1 = 1";
  if (chartQuery.difficultyMinLevel.has_value()) {
    query +=
        " AND dl.level_sort >= (SELECT MIN(dte_min.sort_order) "
        "FROM difficulty_table_entries dte_min WHERE "
        "dte_min.table_id = @table_id AND dte_min.level = "
        "@difficulty_min_level)";
  }
  if (chartQuery.difficultyMaxLevel.has_value()) {
    query +=
        " AND dl.level_sort <= (SELECT MIN(dte_max.sort_order) "
        "FROM difficulty_table_entries dte_max WHERE "
        "dte_max.table_id = @table_id AND dte_max.level = "
        "@difficulty_max_level)";
  }
  query += ")";
}

void appendChartScoreRankFilter(std::string &query,
                                const std::string &chartAlias,
                                const ChartMetaQuery &chartQuery) {
  if (!chartQuery.scoreRank.has_value()) {
    return;
  }
  const std::string scoreRankExpr =
      chartBestScoreExpr(chartAlias, "score_rank",
                         chartQuery.selectedLongNoteMode);
  query += " AND ";
  if (chartQuery.scoreRankOrAbove) {
    query += scoreRankOrderExpr(scoreRankExpr) + " >= " +
             scoreRankOrderExpr("@score_rank");
  } else if (chartQuery.scoreRankOrBelow) {
    query += scoreRankOrderExpr(scoreRankExpr) + " <= " +
             scoreRankOrderExpr("@score_rank");
  } else {
    query += scoreRankExpr + " = @score_rank";
  }
}

void appendDifficultyEntryScoreRankFilter(std::string &query,
                                          const std::string &entryAlias,
                                          const std::string &chartAlias,
                                          const ChartMetaQuery &chartQuery) {
  if (!chartQuery.scoreRank.has_value()) {
    return;
  }
  const std::string scoreRankExpr =
      difficultyEntryBestScoreExpr(entryAlias, chartAlias, "score_rank",
                                   chartQuery.selectedLongNoteMode);
  query += " AND ";
  if (chartQuery.scoreRankOrAbove) {
    query += scoreRankOrderExpr(scoreRankExpr) + " >= " +
             scoreRankOrderExpr("@score_rank");
  } else if (chartQuery.scoreRankOrBelow) {
    query += scoreRankOrderExpr(scoreRankExpr) + " <= " +
             scoreRankOrderExpr("@score_rank");
  } else {
    query += scoreRankExpr + " = @score_rank";
  }
}

void bindCommonChartFilterParameters(sqlite3_stmt *stmt, int &bindIndex,
                                     const ChartMetaQuery &chartQuery) {
  if (chartQuery.bpmMin.has_value()) {
    sqlite3_bind_double(stmt, bindIndex++, *chartQuery.bpmMin);
  }
  if (chartQuery.bpmMax.has_value()) {
    sqlite3_bind_double(stmt, bindIndex++, *chartQuery.bpmMax);
  }
  if (chartQuery.scoreRank.has_value()) {
    bindSqliteText(stmt, bindIndex++, *chartQuery.scoreRank);
  }
}

std::string sortDirectionSql(ChartRecordSortDirection direction) {
  return direction == ChartRecordSortDirection::Ascending ? "ASC" : "DESC";
}

void appendNullableOrderExpr(std::string &query, const std::string &expr,
                             ChartRecordSortDirection direction) {
  query += expr;
  query += " IS NULL, ";
  query += expr;
  query += " ";
  query += sortDirectionSql(direction);
}

std::string difficultyLevelSortExpr(const std::string &entryAlias) {
  return "(SELECT MIN(dte_level.sort_order) FROM difficulty_table_entries "
         "dte_level WHERE dte_level.table_id = " +
         entryAlias + ".table_id AND dte_level.level = " + entryAlias +
         ".level)";
}

void appendChartMetaOrderBy(std::string &query,
                            const ChartMetaQuery &chartQuery,
                            const std::string &chartAlias) {
  query += " ORDER BY ";
  const auto direction = chartQuery.sortDirection;
  switch (chartQuery.sortCriterion) {
  case ChartRecordSortCriterion::ClearMark:
    query += chartClearMarkRankExpr(chartAlias, chartQuery.selectedLongNoteMode);
    query += " ";
    query += sortDirectionSql(direction);
    query += ", ";
    appendNullableOrderExpr(
        query,
        chartBestScoreExpr(chartAlias, "score",
                           chartQuery.selectedLongNoteMode),
        ChartRecordSortDirection::Descending);
    query += ", ";
    appendNullableOrderExpr(
        query,
        chartBestScoreExpr(chartAlias, "max_combo",
                           chartQuery.selectedLongNoteMode),
        ChartRecordSortDirection::Descending);
    query += ", ";
    break;
  case ChartRecordSortCriterion::Score:
    appendNullableOrderExpr(
        query,
        chartBestScoreExpr(chartAlias, "score",
                           chartQuery.selectedLongNoteMode),
        direction);
    query += ", ";
    query += chartClearMarkRankExpr(chartAlias, chartQuery.selectedLongNoteMode);
    query += " DESC, ";
    appendNullableOrderExpr(
        query,
        chartBestScoreExpr(chartAlias, "max_combo",
                           chartQuery.selectedLongNoteMode),
        ChartRecordSortDirection::Descending);
    query += ", ";
    break;
  case ChartRecordSortCriterion::Title:
    query += chartAlias + ".title COLLATE NOCASE " + sortDirectionSql(direction) +
             ", ";
    break;
  case ChartRecordSortCriterion::MinBpm:
    query += effectiveMinBpmExpr(chartAlias) + " " + sortDirectionSql(direction) +
             ", ";
    break;
  case ChartRecordSortCriterion::MaxBpm:
    query += effectiveMaxBpmExpr(chartAlias) + " " + sortDirectionSql(direction) +
             ", ";
    break;
  case ChartRecordSortCriterion::MainBpm:
    query += mainBpmExpr(chartAlias) + " " + sortDirectionSql(direction) +
             ", ";
    break;
  case ChartRecordSortCriterion::Difficulty:
    query += chartAlias + ".level " + sortDirectionSql(direction) + ", ";
    break;
  case ChartRecordSortCriterion::Default:
    break;
  }
  query += chartAlias + ".title COLLATE NOCASE, " + chartAlias + ".path";
}

void appendDifficultyEntryOrderBy(std::string &query,
                                  const ChartMetaQuery &chartQuery) {
  const std::string missingExpr = "CASE WHEN cm.path IS NULL THEN 1 ELSE 0 END";
  const std::string titleExpr =
      "COALESCE(NULLIF(cm.title, ''), dte.title, '')";
  query += " ORDER BY " + missingExpr + ", ";
  const auto direction = chartQuery.sortDirection;
  switch (chartQuery.sortCriterion) {
  case ChartRecordSortCriterion::ClearMark:
    query += difficultyEntryClearMarkRankExpr(
        "dte", "cm", chartQuery.selectedLongNoteMode);
    query += " ";
    query += sortDirectionSql(direction);
    query += ", ";
    appendNullableOrderExpr(
        query,
        difficultyEntryBestScoreExpr("dte", "cm", "score",
                                     chartQuery.selectedLongNoteMode),
        ChartRecordSortDirection::Descending);
    query += ", ";
    appendNullableOrderExpr(
        query,
        difficultyEntryBestScoreExpr("dte", "cm", "max_combo",
                                     chartQuery.selectedLongNoteMode),
        ChartRecordSortDirection::Descending);
    query += ", ";
    break;
  case ChartRecordSortCriterion::Score:
    appendNullableOrderExpr(
        query,
        difficultyEntryBestScoreExpr("dte", "cm", "score",
                                     chartQuery.selectedLongNoteMode),
        direction);
    query += ", ";
    query += difficultyEntryClearMarkRankExpr(
        "dte", "cm", chartQuery.selectedLongNoteMode);
    query += " DESC, ";
    appendNullableOrderExpr(
        query,
        difficultyEntryBestScoreExpr("dte", "cm", "max_combo",
                                     chartQuery.selectedLongNoteMode),
        ChartRecordSortDirection::Descending);
    query += ", ";
    break;
  case ChartRecordSortCriterion::Title:
    query += titleExpr + " COLLATE NOCASE " + sortDirectionSql(direction) +
             ", ";
    break;
  case ChartRecordSortCriterion::MinBpm:
    query += effectiveMinBpmExpr("cm") + " " + sortDirectionSql(direction) +
             ", ";
    break;
  case ChartRecordSortCriterion::MaxBpm:
    query += effectiveMaxBpmExpr("cm") + " " + sortDirectionSql(direction) +
             ", ";
    break;
  case ChartRecordSortCriterion::MainBpm:
    query += mainBpmExpr("cm") + " " + sortDirectionSql(direction) + ", ";
    break;
  case ChartRecordSortCriterion::Difficulty:
    query += difficultyLevelSortExpr("dte") + " " +
             sortDirectionSql(direction) + ", ";
    break;
  case ChartRecordSortCriterion::Default:
    break;
  }
  query += "dte.sort_order, " + titleExpr + " COLLATE NOCASE, dte.id";
}

void appendDifficultyCourseEntryOrderBy(std::string &query,
                                        const ChartMetaQuery &chartQuery) {
  const std::string titleExpr =
      "COALESCE(NULLIF(cm.title, ''), dce.title, '')";
  query += " ORDER BY ";
  const auto direction = chartQuery.sortDirection;
  switch (chartQuery.sortCriterion) {
  case ChartRecordSortCriterion::ClearMark:
    query += difficultyEntryClearMarkRankExpr(
        "dce", "cm", chartQuery.selectedLongNoteMode);
    query += " ";
    query += sortDirectionSql(direction);
    query += ", ";
    appendNullableOrderExpr(
        query,
        difficultyEntryBestScoreExpr("dce", "cm", "score",
                                     chartQuery.selectedLongNoteMode),
        ChartRecordSortDirection::Descending);
    query += ", ";
    appendNullableOrderExpr(
        query,
        difficultyEntryBestScoreExpr("dce", "cm", "max_combo",
                                     chartQuery.selectedLongNoteMode),
        ChartRecordSortDirection::Descending);
    query += ", ";
    break;
  case ChartRecordSortCriterion::Score:
    appendNullableOrderExpr(
        query,
        difficultyEntryBestScoreExpr("dce", "cm", "score",
                                     chartQuery.selectedLongNoteMode),
        direction);
    query += ", ";
    query += difficultyEntryClearMarkRankExpr(
        "dce", "cm", chartQuery.selectedLongNoteMode);
    query += " DESC, ";
    appendNullableOrderExpr(
        query,
        difficultyEntryBestScoreExpr("dce", "cm", "max_combo",
                                     chartQuery.selectedLongNoteMode),
        ChartRecordSortDirection::Descending);
    query += ", ";
    break;
  case ChartRecordSortCriterion::Title:
    query += titleExpr + " COLLATE NOCASE " + sortDirectionSql(direction) +
             ", ";
    break;
  case ChartRecordSortCriterion::MinBpm:
    query += effectiveMinBpmExpr("cm") + " " + sortDirectionSql(direction) +
             ", ";
    break;
  case ChartRecordSortCriterion::MaxBpm:
    query += effectiveMaxBpmExpr("cm") + " " + sortDirectionSql(direction) +
             ", ";
    break;
  case ChartRecordSortCriterion::MainBpm:
    query += mainBpmExpr("cm") + " " + sortDirectionSql(direction) + ", ";
    break;
  case ChartRecordSortCriterion::Difficulty:
  case ChartRecordSortCriterion::Default:
    break;
  }
  query += "dc.sort_order, dce.sort_order, " + titleExpr +
           " COLLATE NOCASE, dce.id";
}

std::string matchedDifficultyEntryIdSubquery(
    const std::string &courseEntryAlias = "dce",
    const std::string &courseAlias = "dc",
    const std::string &matchAlias = "dte_match") {
  return "(SELECT " + matchAlias +
         ".id FROM difficulty_table_entries " + matchAlias + " WHERE " +
         matchAlias + ".table_id = " + courseAlias + ".table_id AND ((" +
         sqlHashColumnHasValue(courseEntryAlias, "sha256") + " AND " +
         storedHashColumn(matchAlias, "sha256") +
         " = " + storedHashColumn(courseEntryAlias, "sha256") +
         ") OR (" + sqlHashColumnHasValue(courseEntryAlias, "md5") +
         " AND " + storedHashColumn(matchAlias, "md5") +
         " = " + storedHashColumn(courseEntryAlias, "md5") +
         ")) ORDER BY " + matchAlias +
         ".sort_order, " + matchAlias + ".title COLLATE NOCASE LIMIT 1)";
}

void appendChartMetaFilters(std::string &query,
                            const ChartMetaQuery &chartQuery) {
  if (!chartQuery.keyword.empty()) {
    query += " AND rtrim(cm.title || ' ' || cm.subtitle || ' ' || cm.artist || "
             "' ' || cm.sub_artist || ' ' || cm.genre) LIKE @text";
  }

  appendExactFolderFilter(query, "cm", chartQuery);

  if (chartQuery.tableId > 0) {
    query += " AND (cm.sha256 IN (SELECT ";
    query += storedHashColumn("dte", "sha256");
    query += " FROM "
             "difficulty_table_entries dte "
             "WHERE dte.table_id = @table_id AND ";
    query += sqlHashColumnHasValue("dte", "sha256");
    if (!chartQuery.tableLevel.empty()) {
      query += " AND dte.level = @table_level";
    }
    query += ") OR cm.md5 IN (SELECT ";
    query += storedHashColumn("dte", "md5");
    query += " FROM difficulty_table_entries dte "
             "WHERE dte.table_id = @table_id AND ";
    query += sqlHashColumnHasValue("dte", "md5");
    if (!chartQuery.tableLevel.empty()) {
      query += " AND dte.level = @table_level";
    }
    query += "))";
  }

  if (chartMetaQueryHasCourseFilter(chartQuery)) {
    query += " AND (cm.sha256 IN (SELECT ";
    query += storedHashColumn("dce", "sha256");
    query += " FROM "
             "difficulty_course_entries dce "
             "JOIN difficulty_courses dc ON dc.id = dce.course_id "
             "WHERE ";
    query += sqlHashColumnHasValue("dce", "sha256");
    if (chartQuery.courseId > 0) {
      query += " AND dce.course_id = @course_id";
    }
    if (chartQuery.courseTableId > 0) {
      query += " AND dc.table_id = @course_table_id";
    }
    if (!chartQuery.courseGroupName.empty()) {
      query += " AND dc.group_name = @course_group_name";
    }
    query += ") OR cm.md5 IN (SELECT ";
    query += storedHashColumn("dce", "md5");
    query += " FROM difficulty_course_entries dce "
             "JOIN difficulty_courses dc ON dc.id = dce.course_id "
             "WHERE ";
    query += sqlHashColumnHasValue("dce", "md5");
    if (chartQuery.courseId > 0) {
      query += " AND dce.course_id = @course_id";
    }
    if (chartQuery.courseTableId > 0) {
      query += " AND dc.table_id = @course_table_id";
    }
    if (!chartQuery.courseGroupName.empty()) {
      query += " AND dc.group_name = @course_group_name";
    }
    query += "))";
  }

  appendBpmFilters(query, "cm", chartQuery);
  appendChartScoreRankFilter(query, "cm", chartQuery);
  if (chartQuery.clearMarkFilter) {
    query += " AND ";
    query += chartClearMarkPredicate("cm", chartQuery);
  }
  if (chartQuery.favoritesOnly) {
    query += " AND ";
    query += chartFavoriteIndexedPredicate("cm");
  }

  query += " AND ";
  query += preferredChartPredicate("cm");
}

void bindChartMetaFilterParameters(sqlite3_stmt *stmt, int &bindIndex,
                                   const ChartMetaQuery &chartQuery) {
  if (!chartQuery.keyword.empty()) {
    bindSqliteText(stmt, bindIndex++, "%" + chartQuery.keyword + "%");
  }
  bindExactFolderFilter(stmt, bindIndex, chartQuery);
  if (chartQuery.tableId > 0) {
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.tableId);
    if (!chartQuery.tableLevel.empty()) {
      bindSqliteText(stmt, bindIndex++, chartQuery.tableLevel);
    }
  }
  if (chartMetaQueryHasCourseFilter(chartQuery)) {
    if (chartQuery.courseId > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.courseId);
    }
    if (chartQuery.courseTableId > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.courseTableId);
    }
    if (!chartQuery.courseGroupName.empty()) {
      bindSqliteText(stmt, bindIndex++, chartQuery.courseGroupName);
    }
  }
  bindCommonChartFilterParameters(stmt, bindIndex, chartQuery);
  if (chartQuery.clearMarkFilter) {
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.clearMarkRank);
  }
}

void appendDifficultyEntryFilters(std::string &query,
                                  const ChartMetaQuery &chartQuery) {
  query += "WHERE dte.table_id = @table_id";

  if (!chartQuery.tableLevel.empty()) {
    query += " AND dte.level = @table_level";
  }
  if (!chartQuery.keyword.empty()) {
    query += " AND ";
    query += kDifficultyEntrySearchText;
    query += " LIKE @text";
  }
  appendExactFolderFilter(query, "cm", chartQuery);
  appendDifficultyLevelRangeFilter(query, chartQuery);
  appendBpmFilters(query, "cm", chartQuery);
  appendDifficultyEntryScoreRankFilter(query, "dte", "cm", chartQuery);
  if (chartQuery.clearMarkFilter) {
    query += " AND ";
    query += difficultyEntryClearMarkPredicate("dte", "cm", chartQuery);
  }
}

void bindDifficultyEntryFilterParameters(sqlite3_stmt *stmt, int &bindIndex,
                                         const ChartMetaQuery &chartQuery) {
  sqlite3_bind_int(stmt, bindIndex++, chartQuery.tableId);
  if (!chartQuery.tableLevel.empty()) {
    bindSqliteText(stmt, bindIndex++, chartQuery.tableLevel);
  }
  if (!chartQuery.keyword.empty()) {
    bindSqliteText(stmt, bindIndex++, "%" + chartQuery.keyword + "%");
  }
  bindExactFolderFilter(stmt, bindIndex, chartQuery);
  if (chartQuery.difficultyMinLevel.has_value()) {
    bindSqliteText(stmt, bindIndex++, *chartQuery.difficultyMinLevel);
  }
  if (chartQuery.difficultyMaxLevel.has_value()) {
    bindSqliteText(stmt, bindIndex++, *chartQuery.difficultyMaxLevel);
  }
  bindCommonChartFilterParameters(stmt, bindIndex, chartQuery);
  if (chartQuery.clearMarkFilter) {
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.clearMarkRank);
  }
}

void appendDifficultyCourseEntryFilters(std::string &query,
                                        const ChartMetaQuery &chartQuery) {
  query += "WHERE 1 = 1";

  if (chartQuery.courseId > 0) {
    query += " AND dce.course_id = @course_id";
  }
  if (chartQuery.courseTableId > 0) {
    query += " AND dc.table_id = @course_table_id";
  }
  if (!chartQuery.courseGroupName.empty()) {
    query += " AND dc.group_name = @course_group_name";
  }
  if (!chartQuery.keyword.empty()) {
    query += " AND ";
    query += kDifficultyCourseEntrySearchText;
    query += " LIKE @text";
  }
  appendExactFolderFilter(query, "cm", chartQuery);
  appendBpmFilters(query, "cm", chartQuery);
  appendDifficultyEntryScoreRankFilter(query, "dce", "cm", chartQuery);
  if (chartQuery.clearMarkFilter) {
    query += " AND ";
    query += difficultyEntryClearMarkPredicate("dce", "cm", chartQuery);
  }
}

void bindDifficultyCourseEntryFilterParameters(
    sqlite3_stmt *stmt, int &bindIndex, const ChartMetaQuery &chartQuery) {
  if (chartQuery.courseId > 0) {
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.courseId);
  }
  if (chartQuery.courseTableId > 0) {
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.courseTableId);
  }
  if (!chartQuery.courseGroupName.empty()) {
    bindSqliteText(stmt, bindIndex++, chartQuery.courseGroupName);
  }
  if (!chartQuery.keyword.empty()) {
    bindSqliteText(stmt, bindIndex++, "%" + chartQuery.keyword + "%");
  }
  bindExactFolderFilter(stmt, bindIndex, chartQuery);
  bindCommonChartFilterParameters(stmt, bindIndex, chartQuery);
  if (chartQuery.clearMarkFilter) {
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.clearMarkRank);
  }
}

int columnInt(sqlite3_stmt *stmt, int idx) {
  return sqlite3_column_type(stmt, idx) == SQLITE_NULL
             ? 0
             : sqlite3_column_int(stmt, idx);
}

std::string columnString(sqlite3_stmt *stmt, int idx) {
  return sqliteColumnString(stmt, idx);
}

void appendUniqueLabel(
    std::unordered_map<std::string, std::vector<std::string>> &labelsByHash,
    const std::string &hash, const std::string &label) {
  if (hash.empty() || label.empty()) {
    return;
  }
  auto &labels = labelsByHash[hash];
  if (std::find(labels.begin(), labels.end(), label) == labels.end()) {
    labels.push_back(label);
  }
}

void appendChartLabels(const std::vector<std::string> &source,
                       std::vector<std::string> &destination,
                       std::unordered_set<std::string> &seen) {
  for (const auto &label : source) {
    if (seen.insert(label).second) {
      destination.push_back(label);
    }
  }
}

std::string joinLabels(const std::vector<std::string> &labels) {
  std::string joined;
  for (const auto &label : labels) {
    if (!joined.empty()) {
      joined += " / ";
    }
    joined += label;
  }
  return joined;
}

void loadDifficultyLabelCache(sqlite3 *db, DifficultyLabelCache &cache) {
  cache.loaded = false;
  cache.labelsBySha256.clear();
  cache.labelsByMd5.clear();
  auto query = "SELECT dte.sha256, dte.md5, dt.symbol || dte.level AS label "
               "FROM difficulty_table_entries dte "
               "JOIN difficulty_tables dt ON dt.id = dte.table_id "
               "WHERE dte.sha256 != '' OR dte.md5 != '' "
               "ORDER BY dt.id, dte.sort_order, label";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "loading difficulty labels",
                                    logSqlErrorText)) {
    return;
  }

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const std::string sha256 = normalizedHash(columnString(stmt.get(), 0));
    const std::string md5 = normalizedHash(columnString(stmt.get(), 1));
    const std::string label = columnString(stmt.get(), 2);
    appendUniqueLabel(cache.labelsBySha256, sha256, label);
    appendUniqueLabel(cache.labelsByMd5, md5, label);
  }
  cache.loaded = true;
}

void invalidateDifficultyLabelCache() {
  std::lock_guard<std::mutex> lock(gDifficultyLabelCacheMutex);
  gDifficultyLabelCache.loaded = false;
  gDifficultyLabelCache.labelsBySha256.clear();
  gDifficultyLabelCache.labelsByMd5.clear();
}

void populateDifficultyTableLabels(
    sqlite3 *db, std::vector<ChartMetaRecord> &chartRecords) {
  if (chartRecords.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(gDifficultyLabelCacheMutex);
  if (!gDifficultyLabelCache.loaded) {
    loadDifficultyLabelCache(db, gDifficultyLabelCache);
  }
  if (!gDifficultyLabelCache.loaded) {
    return;
  }

  for (auto &chartRecord : chartRecords) {
    std::vector<std::string> labels;
    std::unordered_set<std::string> seen;
    const auto shaIt = gDifficultyLabelCache.labelsBySha256.find(
        normalizedHash(chartRecord.meta.SHA256));
    if (shaIt != gDifficultyLabelCache.labelsBySha256.end()) {
      appendChartLabels(shaIt->second, labels, seen);
    }
    const auto md5It =
        gDifficultyLabelCache.labelsByMd5.find(
            normalizedHash(chartRecord.meta.MD5));
    if (md5It != gDifficultyLabelCache.labelsByMd5.end()) {
      appendChartLabels(md5It->second, labels, seen);
    }
    chartRecord.difficultyTableLabels = joinLabels(labels);
  }
}
} // namespace


namespace {
path_t readPath(sqlite3_stmt *stmt, int idx);
bms_parser::ChartMeta readChartMeta(sqlite3_stmt *stmt);
ChartMetaRecord readChartMetaRecord(sqlite3_stmt *stmt);
void selectAllChartMeta(
    sqlite3 *database, std::vector<bms_parser::ChartMeta> &chartMetas);
void selectFavoriteMusicTracks(sqlite3 *database,
                               std::vector<MusicTrackRecord> &tracks);
int countFavoriteCharts(sqlite3 *database);
bool setFavorite(sqlite3 *database, const bms_parser::ChartMeta &chartMeta,
                 bool favorite);
bool setSongReviewFavorite(sqlite3 *database, std::string_view sha256,
                           int favorite);
int countAllChartMeta(sqlite3 *database);
int countSolidArchives(sqlite3 *database);
void queryChartMeta(sqlite3 *database, const ChartMetaQuery &query,
                    std::vector<ChartMetaRecord> &chartMetas);
int countChartMeta(sqlite3 *database, const ChartMetaQuery &query);
int findChartMetaIndex(sqlite3 *database, const ChartMetaQuery &query,
                       const std::filesystem::path &path);
std::string difficultyTableLabelsForChart(
    sqlite3 *database, const bms_parser::ChartMeta &meta);
} // namespace

void chart_repository_detail::InvalidateDifficultyLabelCache() {
  invalidateDifficultyLabelCache();
}

void chart_repository_detail::BumpLibraryRevision() {
  bumpLibraryRevision();
}

int ChartRepository::Session::CountAllChartMeta() {
  return countAllChartMeta(impl_->database());
}

int ChartRepository::Session::CountSolidArchives() {
  return countSolidArchives(impl_->database());
}

void ChartRepository::Session::SelectAllChartMeta(
    std::vector<bms_parser::ChartMeta> &chartMetas) {
  selectAllChartMeta(impl_->database(), chartMetas);
}

void ChartRepository::Session::SelectFavoriteMusicTracks(
    std::vector<MusicTrackRecord> &tracks) {
  selectFavoriteMusicTracks(impl_->database(), tracks);
}

int ChartRepository::Session::CountFavoriteCharts() {
  return countFavoriteCharts(impl_->database());
}

bool ChartRepository::Session::SetFavorite(
    const bms_parser::ChartMeta &chartMeta, bool favorite) {
  return setFavorite(impl_->database(), chartMeta, favorite);
}

bool ChartRepository::Session::SetSongReviewFavorite(
    std::string_view sha256, int favorite) {
  return setSongReviewFavorite(impl_->database(), sha256, favorite);
}

void ChartRepository::Session::QueryChartMeta(
    const ChartMetaQuery &query, std::vector<ChartMetaRecord> &chartMetas) {
  std::optional<ScoreRepository::PreparedScoreQueryDatabase> prepared;
  if (chartMetaQueryNeedsScoreCache(query)) {
    prepared.emplace(impl_->scoreRepository(), *this);
    if (const auto &error = prepared->error()) {
      SDL_Log("SQL error while preparing score query database: %s",
              error->c_str());
      return;
    }
  }
  queryChartMeta(impl_->database(), query, chartMetas);
}

ChartMetaPathBatchReadOutcome ChartRepository::Session::SelectChartMetaByPaths(
    std::span<const std::filesystem::path> paths) {
  return chart_repository_detail::SelectChartMetaByPaths(impl_->database(),
                                                         paths);
}

std::vector<bms_parser::ChartMeta>
ChartRepository::Session::SelectChartMetaByHash(const std::string &sha256,
                                                 const std::string &md5) {
  return chart_repository_detail::SelectChartMetaByHash(impl_->database(),
                                                         sha256, md5);
}

int ChartRepository::Session::CountChartMeta(const ChartMetaQuery &query) {
  std::optional<ScoreRepository::PreparedScoreQueryDatabase> prepared;
  if (chartMetaQueryNeedsScoreCache(query)) {
    prepared.emplace(impl_->scoreRepository(), *this);
    if (const auto &error = prepared->error()) {
      SDL_Log("SQL error while preparing score query database: %s",
              error->c_str());
      return 0;
    }
  }
  return countChartMeta(impl_->database(), query);
}

int ChartRepository::Session::FindChartMetaIndex(
    const ChartMetaQuery &query, const std::filesystem::path &path) {
  std::optional<ScoreRepository::PreparedScoreQueryDatabase> prepared;
  if (chartMetaQueryNeedsScoreCache(query)) {
    prepared.emplace(impl_->scoreRepository(), *this);
    if (const auto &error = prepared->error()) {
      SDL_Log("SQL error while preparing score query database: %s",
              error->c_str());
      return -1;
    }
  }
  return findChartMetaIndex(impl_->database(), query,
                                                path);
}
std::string ChartRepository::Session::DifficultyTableLabelsForChart(
    const bms_parser::ChartMeta &meta) {
  return difficultyTableLabelsForChart(
      impl_->database(), meta);
}

namespace {
void selectAllChartMeta(
    sqlite3 *db, std::vector<bms_parser::ChartMeta> &chartMetas) {
  std::string query = "SELECT ";
  query += kChartMetaSelectColumns;
  query += " FROM chart_meta cm ORDER BY cm.title";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "getting all charts",
                                    logSqlErrorText)) {
    return;
  }

  // reserve space for the result
  chartMetas.reserve(sqlite3_column_count(stmt));

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    chartMetas.push_back(std::move(readChartMeta(stmt)));
  }
}

void selectFavoriteMusicTracks(
    sqlite3 *db, std::vector<MusicTrackRecord> &tracks) {
  if (db == nullptr) {
    return;
  }

  std::string representativeOrder =
      "choice.favorite_added_at DESC, ";
  representativeOrder += chartArtworkOrderBy("choice");
  representativeOrder +=
      ", choice.identity_rank, choice.total_notes DESC, choice.length DESC, ";
  representativeOrder += chartSourceOrderBy("choice");
  representativeOrder += ", choice.title COLLATE NOCASE, choice.path";

  std::string query = "WITH candidates AS (";
  query += chartFavoriteChartCandidateQuery();
  query += "), preferred_candidates AS (SELECT * FROM candidates pc WHERE ";
  query += preferredChartPredicate("pc");
  query +=
      "), favorite_choices AS (SELECT pc.favorite_identity_key, "
      "COUNT(*) AS music_chart_count, "
      "(SELECT choice.path FROM preferred_candidates choice WHERE "
      "choice.favorite_identity_key = pc.favorite_identity_key ORDER BY ";
  query += representativeOrder;
  query +=
      " LIMIT 1) AS representative_path FROM preferred_candidates pc "
      "GROUP BY pc.favorite_identity_key) SELECT ";
  query += kChartMetaSelectColumns;
  query +=
      ", favorite_choices.music_chart_count FROM preferred_candidates cm "
      "JOIN favorite_choices ON favorite_choices.favorite_identity_key = "
      "cm.favorite_identity_key AND favorite_choices.representative_path = "
      "cm.path ORDER BY cm.favorite_added_at DESC, "
      "cm.title COLLATE NOCASE, cm.path";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting favorite music tracks",
                                    logSqlErrorText)) {
    return;
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MusicTrackRecord record;
    record.representativeChart = readChartMeta(stmt);
    record.chartCount =
        std::max(1, sqlite3_column_int(stmt, kChartMetaColumnCount));
    record.useChartPathIdentity = true;
    tracks.push_back(std::move(record));
  }
}

int countFavoriteCharts(sqlite3 *db) {
  if (db == nullptr) {
    return 0;
  }

  std::string query = "SELECT COUNT(*) FROM chart_meta cm WHERE ";
  query += chartFavoriteIndexedPredicate("cm");
  query += " AND ";
  query += preferredChartPredicate("cm");

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "counting favorite charts",
                                    logSqlErrorText)) {
    return 0;
  }
  int count = 0;
  if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt.get(), 0);
  }
  return count;
}

bool setFavorite(sqlite3 *db,
                                const bms_parser::ChartMeta &chartMeta,
                                bool favorite) {
  if (db == nullptr) {
    return false;
  }

  const ChartFavoriteIdentity identity = chartFavoriteIdentityFor(chartMeta);
  if (identity.chartPath.empty()) {
    return false;
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN", transactionError);
  if (!transaction.active()) {
    logSqlErrorText("starting chart favorite update", transactionError);
    return false;
  }

  bool chartFavoriteChanged = false;

  if (!favorite) {
    const std::string query =
        "DELETE FROM chart_favorites WHERE " + chartFavoriteDeletePredicate();
    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "preparing chart favorite delete",
                                      logSqlErrorText)) {
      return false;
    }
    bindChartFavoriteDeleteIdentity(stmt.get(), identity);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      logSqlError("deleting chart favorite", db);
      return false;
    }
    chartFavoriteChanged = sqlite3_changes(db) > 0;
  } else {
    const char *query =
        "INSERT INTO chart_favorites "
        "(chart_path, chart_md5, chart_sha256, added_at) "
        "VALUES (?1, ?2, ?3, CURRENT_TIMESTAMP) "
        "ON CONFLICT(chart_path) DO UPDATE SET "
        "chart_md5 = excluded.chart_md5,"
        "chart_sha256 = excluded.chart_sha256,"
        "added_at = CURRENT_TIMESTAMP";
    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "preparing chart favorite insert",
                                      logSqlErrorText)) {
      return false;
    }
    bindSqliteText(stmt, 1, identity.chartPath);
    bindSqliteText(stmt, 2, identity.md5);
    bindSqliteText(stmt, 3, identity.sha256);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      logSqlError("saving chart favorite", db);
      return false;
    }
    chartFavoriteChanged = sqlite3_changes(db) > 0;
  }

  bool reviewChanged = false;
  if (!updateSongReviewChartFavorite(db, identity.sha256, favorite,
                                     reviewChanged)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing chart favorite update", transactionError);
    return false;
  }
  if (chartFavoriteChanged || reviewChanged) {
    bumpLibraryRevision();
  }
  return true;
}

bool setSongReviewFavorite(sqlite3 *db, std::string_view sha256,
                           int favorite) {
  if (db == nullptr) return false;
  if (sha256.empty()) return true;
  const char *query =
      "INSERT INTO review(sha256, favorite) VALUES(?1, ?2) "
      "ON CONFLICT(sha256) DO UPDATE SET favorite=excluded.favorite";
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(db, query, statement,
                                    "preparing SongReview favorite update",
                                    logSqlErrorText) ||
      !bindSqliteText(statement.get(), 1, std::string(sha256)) ||
      sqlite3_bind_int(statement.get(), 2, favorite) != SQLITE_OK) {
    return false;
  }
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    logSqlError("updating SongReview favorite", db);
    return false;
  }
  if (sqlite3_changes(db) > 0) bumpLibraryRevision();
  return true;
}

int countAllChartMeta(sqlite3 *db) {
  auto query = "SELECT COUNT(*) FROM chart_meta";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "counting charts",
                                    logSqlErrorText)) {
    return 0;
  }
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  return count;
}

int countSolidArchives(sqlite3 *db) {
  auto query = "SELECT COUNT(*) FROM solid_archives";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "counting solid archives",
                                    logSqlErrorText)) {
    return 0;
  }
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  return count;
}

void queryChartMeta(
    sqlite3 *db, const ChartMetaQuery &chartQuery,
    std::vector<ChartMetaRecord> &chartMetas) {
  if (db == nullptr) {
    return;
  }
  if (queryNeedsDifficultyTableSchema(chartQuery) &&
      !chart_repository_detail::EnsureDifficultySchema(db)) {
    return;
  }
  if (chartQuery.solidArchivesOnly) {
    std::string query = "SELECT ";
    query += kSolidArchiveSelectColumns;
    query += " FROM solid_archives sa WHERE 1 = 1";
    if (!chartQuery.keyword.empty()) {
      query += " AND (sa.name LIKE @text OR sa.path LIKE @text)";
    }
    query += " ORDER BY sa.name COLLATE NOCASE, sa.path";
    if (chartQuery.limit > 0) {
      query += " LIMIT @limit OFFSET @offset";
    }

    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "querying solid archives",
                                      logSqlErrorText)) {
      return;
    }

    int bindIndex = 1;
    if (!chartQuery.keyword.empty()) {
      bindSqliteText(stmt, bindIndex++, "%" + chartQuery.keyword + "%");
    }
    if (chartQuery.limit > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.limit);
      sqlite3_bind_int(stmt, bindIndex++, std::max(0, chartQuery.offset));
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      int idx = 0;
      ChartMetaRecord record;
      std::filesystem::path path(readPath(stmt, idx++));
      if (!path.empty()) {
        chart_storage_identity::ToAbsolutePath(path);
      }
      record.meta.BmsPath = path;
      record.meta.Folder = path.parent_path();
      record.meta.Title = columnString(stmt, idx++);
      record.meta.Artist = "Solid archive skipped";
      record.archiveSize = static_cast<std::uint64_t>(
          std::max<sqlite3_int64>(0, sqlite3_column_int64(stmt, idx++)));
      record.archiveUncompressedSize = static_cast<std::uint64_t>(
          std::max<sqlite3_int64>(0, sqlite3_column_int64(stmt, idx++)));
      record.archiveFileCount = sqlite3_column_int(stmt, idx++);
      record.difficultyTableLabels = "Unzip required";
      record.solidArchive = true;
      chartMetas.push_back(std::move(record));
    }
    return;
  }

  if (chartMetaQueryUsesDifficultyEntries(chartQuery)) {
    std::string query = "SELECT ";
    query += kDifficultyEntrySelectColumns;
    query += ", ";
    query += chartFavoriteColumnExpr("cm");
    query += ", ";
    query += songReviewFavoriteColumnExpr("cm");
    query += ", COALESCE(dte.url, ''), COALESCE(dte.url_diff, ''), "
             "dte.org_md5";
    query += " FROM difficulty_table_entries dte "
             "JOIN difficulty_tables dt ON dt.id = dte.table_id "
             "LEFT JOIN chart_meta cm ON cm.path = ";
    query += matchedChartPathSubquery("dte", true);
    query += " ";
    appendDifficultyEntryFilters(query, chartQuery);

    appendDifficultyEntryOrderBy(query, chartQuery);
    if (chartQuery.limit > 0) {
      query += " LIMIT @limit OFFSET @offset";
    }

    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "querying difficulty entries",
                                      logSqlErrorText)) {
      return;
    }

    int bindIndex = 1;
    bindDifficultyEntryFilterParameters(stmt, bindIndex, chartQuery);
    if (chartQuery.limit > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.limit);
      sqlite3_bind_int(stmt, bindIndex++, std::max(0, chartQuery.offset));
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      chartMetas.push_back(std::move(readChartMetaRecord(stmt)));
    }
    return;
  }

  if (chartMetaQueryUsesCourseEntries(chartQuery)) {
    std::string query = "SELECT ";
    query += kDifficultyCourseEntrySelectColumns;
    query += ", ";
    query += chartFavoriteColumnExpr("cm");
    query += ", ";
    query += songReviewFavoriteColumnExpr("cm");
    query += ", COALESCE(dce.url, ''), COALESCE(dce.url_diff, ''), "
             "dce.org_md5";
    query += " FROM difficulty_course_entries dce "
             "JOIN difficulty_courses dc ON dc.id = dce.course_id "
             "JOIN difficulty_tables dt ON dt.id = dc.table_id "
             "LEFT JOIN difficulty_table_entries dte ON dte.id = ";
    query += matchedDifficultyEntryIdSubquery();
    query += " LEFT JOIN chart_meta cm ON cm.path = ";
    query += matchedChartPathSubquery("dce", true);
    query += " ";
    appendDifficultyCourseEntryFilters(query, chartQuery);

    appendDifficultyCourseEntryOrderBy(query, chartQuery);
    if (chartQuery.limit > 0) {
      query += " LIMIT @limit OFFSET @offset";
    }

    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "querying course entries",
                                      logSqlErrorText)) {
      return;
    }

    int bindIndex = 1;
    bindDifficultyCourseEntryFilterParameters(stmt, bindIndex, chartQuery);
    if (chartQuery.limit > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.limit);
      sqlite3_bind_int(stmt, bindIndex++, std::max(0, chartQuery.offset));
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      chartMetas.push_back(std::move(readChartMetaRecord(stmt)));
    }
    return;
  }

  std::string query = "SELECT ";
  query += kChartMetaSelectColumns;
  query += ", '', 0, ";
  query += chartFavoriteColumnExpr("cm");
  query += ", ";
  query += songReviewFavoriteColumnExpr("cm");
  query += ", '', '', NULL";
  query += " FROM chart_meta cm WHERE 1 = 1";
  appendChartMetaFilters(query, chartQuery);
  appendChartMetaOrderBy(query, chartQuery, "cm");
  if (chartQuery.limit > 0) {
    query += " LIMIT @limit OFFSET @offset";
  }

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "querying charts",
                                    logSqlErrorText)) {
    return;
  }

  int bindIndex = 1;
  bindChartMetaFilterParameters(stmt, bindIndex, chartQuery);
  if (chartQuery.limit > 0) {
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.limit);
    sqlite3_bind_int(stmt, bindIndex++, std::max(0, chartQuery.offset));
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    chartMetas.push_back(std::move(readChartMetaRecord(stmt)));
  }
  populateDifficultyTableLabels(db, chartMetas);
}

int countChartMeta(sqlite3 *db,
                                    const ChartMetaQuery &chartQuery) {
  if (db == nullptr) {
    return 0;
  }
  if (queryNeedsDifficultyTableSchema(chartQuery) &&
      !chart_repository_detail::EnsureDifficultySchema(db)) {
    return 0;
  }
  if (chartQuery.solidArchivesOnly) {
    std::string query = "SELECT COUNT(*) FROM solid_archives sa WHERE 1 = 1";
    if (!chartQuery.keyword.empty()) {
      query += " AND (sa.name LIKE @text OR sa.path LIKE @text)";
    }
    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "counting solid archives",
                                      logSqlErrorText)) {
      return 0;
    }
    if (!chartQuery.keyword.empty()) {
      bindSqliteText(stmt, 1, "%" + chartQuery.keyword + "%");
    }
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
    }
    return count;
  }

  if (chartMetaQueryUsesDifficultyEntries(chartQuery)) {
    std::string query = "SELECT COUNT(*) FROM difficulty_table_entries dte "
                        "JOIN difficulty_tables dt ON dt.id = dte.table_id ";
    if (chartMetaQueryNeedsChartJoinForDifficultyEntries(chartQuery)) {
      query += "LEFT JOIN chart_meta cm ON cm.path = ";
      query += matchedChartPathSubquery("dte", true);
      query += " ";
    }
    appendDifficultyEntryFilters(query, chartQuery);

    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "counting difficulty entries",
                                      logSqlErrorText)) {
      return 0;
    }

    int bindIndex = 1;
    bindDifficultyEntryFilterParameters(stmt, bindIndex, chartQuery);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
    }
    return count;
  }

  if (chartMetaQueryUsesCourseEntries(chartQuery)) {
    const bool needsDifficultyEntryJoin = !chartQuery.keyword.empty();
    std::string query =
        "SELECT COUNT(*) FROM difficulty_course_entries dce "
        "JOIN difficulty_courses dc ON dc.id = dce.course_id ";
    if (needsDifficultyEntryJoin) {
      query += "LEFT JOIN difficulty_table_entries dte ON dte.id = ";
      query += matchedDifficultyEntryIdSubquery();
      query += " ";
    }
    if (chartMetaQueryNeedsChartJoinForCourseEntries(chartQuery)) {
      query += "LEFT JOIN chart_meta cm ON cm.path = ";
      query += matchedChartPathSubquery("dce", true);
      query += " ";
    }
    appendDifficultyCourseEntryFilters(query, chartQuery);

    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "counting course entries",
                                      logSqlErrorText)) {
      return 0;
    }

    int bindIndex = 1;
    bindDifficultyCourseEntryFilterParameters(stmt, bindIndex, chartQuery);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
    }
    return count;
  }

  std::string query = "SELECT COUNT(*) FROM chart_meta cm WHERE 1 = 1";
  appendChartMetaFilters(query, chartQuery);

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "counting charts",
                                    logSqlErrorText)) {
    return 0;
  }

  int bindIndex = 1;
  bindChartMetaFilterParameters(stmt, bindIndex, chartQuery);

  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  return count;
}

int findChartMetaIndex(sqlite3 *db,
                                        const ChartMetaQuery &chartQuery,
                                        const std::filesystem::path &path) {
  if (db == nullptr || path.empty()) {
    return -1;
  }
  if (queryNeedsDifficultyTableSchema(chartQuery) &&
      !chart_repository_detail::EnsureDifficultySchema(db)) {
    return -1;
  }

  const std::string targetPath = chart_storage_identity::StoredPathText(path);
  if (targetPath.empty()) {
    return -1;
  }

  if (chartQuery.sortCriterion != ChartRecordSortCriterion::Default) {
    ChartMetaQuery scanQuery = chartQuery;
    scanQuery.limit = 0;
    scanQuery.offset = 0;
    std::vector<ChartMetaRecord> records;
    queryChartMeta(db, scanQuery, records);
    for (size_t i = 0; i < records.size(); ++i) {
      if (chart_storage_identity::StoredPathText(records[i].meta.BmsPath) ==
          targetPath) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  std::string query;
  if (chartQuery.solidArchivesOnly) {
    query =
        "WITH target AS (SELECT sa.name AS target_name, "
        "sa.path AS target_path FROM solid_archives sa WHERE 1 = 1";
    if (!chartQuery.keyword.empty()) {
      query += " AND (sa.name LIKE @text OR sa.path LIKE @text)";
    }
    query +=
        " AND sa.path = @target_path) "
        "SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM target) THEN -1 ELSE "
        "(SELECT COUNT(*) FROM solid_archives sa, target WHERE 1 = 1";
    if (!chartQuery.keyword.empty()) {
      query += " AND (sa.name LIKE @text OR sa.path LIKE @text)";
    }
    query +=
        " AND (sa.name COLLATE NOCASE < target.target_name COLLATE NOCASE "
        "OR (sa.name COLLATE NOCASE = target.target_name COLLATE NOCASE "
        "AND sa.path < target.target_path))) END";
  } else if (chartMetaQueryUsesDifficultyEntries(chartQuery)) {
    const std::string missingExpr =
        "CASE WHEN cm.path IS NULL THEN 1 ELSE 0 END";
    const std::string titleExpr =
        "COALESCE(NULLIF(cm.title, ''), dte.title, '')";
    auto appendDifficultyEntrySource = [&](std::string &targetQuery,
                                           bool includeTarget = false) {
      targetQuery += " FROM difficulty_table_entries dte "
                     "JOIN difficulty_tables dt ON dt.id = dte.table_id "
                     "LEFT JOIN chart_meta cm ON cm.path = ";
      targetQuery += matchedChartPathSubquery("dte", true);
      targetQuery += " ";
      if (includeTarget) {
        targetQuery += "CROSS JOIN target ";
      }
      appendDifficultyEntryFilters(targetQuery, chartQuery);
    };
    auto appendDifficultyEntryBeforeTarget = [&](std::string &targetQuery) {
      targetQuery += " AND ((";
      targetQuery += missingExpr;
      targetQuery += ") < target.target_missing OR ((";
      targetQuery += missingExpr;
      targetQuery += ") = target.target_missing AND dte.sort_order < "
                     "target.target_sort) OR ((";
      targetQuery += missingExpr;
      targetQuery += ") = target.target_missing AND dte.sort_order = "
                     "target.target_sort AND ";
      targetQuery += titleExpr;
      targetQuery += " COLLATE NOCASE < target.target_title COLLATE NOCASE) "
                     "OR ((";
      targetQuery += missingExpr;
      targetQuery += ") = target.target_missing AND dte.sort_order = "
                     "target.target_sort AND ";
      targetQuery += titleExpr;
      targetQuery += " COLLATE NOCASE = target.target_title COLLATE NOCASE "
                     "AND dte.id < target.target_id))";
    };

    query =
        "WITH target AS (SELECT ";
    query += missingExpr;
    query += " AS target_missing, dte.sort_order AS target_sort, ";
    query += titleExpr;
    query += " AS target_title, dte.id AS target_id";
    appendDifficultyEntrySource(query);
    query += " AND cm.path = @target_path ORDER BY ";
    query += missingExpr;
    query += ", dte.sort_order, ";
    query += titleExpr;
    query +=
        " COLLATE NOCASE, dte.id LIMIT 1) "
        "SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM target) THEN -1 ELSE "
        "(SELECT COUNT(*)";
    appendDifficultyEntrySource(query, true);
    appendDifficultyEntryBeforeTarget(query);
    query += ") END";
  } else if (chartMetaQueryUsesCourseEntries(chartQuery)) {
    const std::string titleExpr =
        "COALESCE(NULLIF(cm.title, ''), dce.title, '')";
    auto appendCourseEntrySource = [&](std::string &targetQuery,
                                       bool includeTarget = false) {
      targetQuery +=
          " FROM difficulty_course_entries dce JOIN difficulty_courses dc "
          "ON dc.id = dce.course_id JOIN difficulty_tables dt ON dt.id = "
          "dc.table_id LEFT JOIN difficulty_table_entries dte ON dte.id = ";
      targetQuery += matchedDifficultyEntryIdSubquery();
      targetQuery += " LEFT JOIN chart_meta cm ON cm.path = ";
      targetQuery += matchedChartPathSubquery("dce", true);
      targetQuery += " ";
      if (includeTarget) {
        targetQuery += "CROSS JOIN target ";
      }
      appendDifficultyCourseEntryFilters(targetQuery, chartQuery);
    };
    auto appendCourseEntryBeforeTarget = [&](std::string &targetQuery) {
      targetQuery += " AND (dc.sort_order < target.target_course_sort OR "
                     "(dc.sort_order = target.target_course_sort AND "
                     "dce.sort_order < target.target_entry_sort) OR "
                     "(dc.sort_order = target.target_course_sort AND "
                     "dce.sort_order = target.target_entry_sort AND ";
      targetQuery += titleExpr;
      targetQuery += " COLLATE NOCASE < target.target_title COLLATE NOCASE) "
                     "OR (dc.sort_order = target.target_course_sort AND "
                     "dce.sort_order = target.target_entry_sort AND ";
      targetQuery += titleExpr;
      targetQuery += " COLLATE NOCASE = target.target_title COLLATE NOCASE "
                     "AND dce.id < target.target_id))";
    };

    query =
        "WITH target AS (SELECT dc.sort_order AS target_course_sort, "
        "dce.sort_order AS target_entry_sort, ";
    query += titleExpr;
    query += " AS target_title, dce.id AS target_id";
    appendCourseEntrySource(query);
    query += " AND cm.path = @target_path ORDER BY dc.sort_order, "
             "dce.sort_order, ";
    query += titleExpr;
    query +=
        " COLLATE NOCASE, dce.id LIMIT 1) "
        "SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM target) THEN -1 ELSE "
        "(SELECT COUNT(*)";
    appendCourseEntrySource(query, true);
    appendCourseEntryBeforeTarget(query);
    query += ") END";
  } else {
    query =
        "WITH target AS (SELECT cm.title AS target_title, "
        "cm.path AS target_path FROM chart_meta cm WHERE 1 = 1";
    appendChartMetaFilters(query, chartQuery);
    query +=
        " AND cm.path = @target_path) "
        "SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM target) THEN -1 ELSE "
        "(SELECT COUNT(*) FROM chart_meta cm, target WHERE 1 = 1";
    appendChartMetaFilters(query, chartQuery);
    query += " AND ";
    query += defaultChartMetaBeforeTargetPredicate("cm", "target");
    query += ") END";
  }

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "finding chart index", logSqlErrorText)) {
    return -1;
  }

  int bindIndex = 1;
  if (chartQuery.solidArchivesOnly) {
    if (!chartQuery.keyword.empty()) {
      bindSqliteText(stmt, bindIndex++, "%" + chartQuery.keyword + "%");
    }
  } else if (chartMetaQueryUsesDifficultyEntries(chartQuery)) {
    bindDifficultyEntryFilterParameters(stmt, bindIndex, chartQuery);
  } else if (chartMetaQueryUsesCourseEntries(chartQuery)) {
    bindDifficultyCourseEntryFilterParameters(stmt, bindIndex, chartQuery);
  } else {
    bindChartMetaFilterParameters(stmt, bindIndex, chartQuery);
  }
  bindSqliteText(stmt, bindIndex++, targetPath);

  if (sqlite3_step(stmt) != SQLITE_ROW) {
    return -1;
  }
  return sqlite3_column_int(stmt, 0);
}

path_t readPath(sqlite3_stmt *stmt, int idx) {
#ifdef _WIN32
  if (sqlite3_column_type(stmt, idx) == SQLITE_NULL) {
    return {};
  }
  const int size = sqlite3_column_bytes(stmt, idx);
  const auto utf8 = std::string(
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx)), size);
  return utf8_to_path_t(utf8);
#else
  const auto *text =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
  return text != nullptr ? path_t(text) : path_t();
#endif
}

bms_parser::ChartMeta readChartMeta(sqlite3_stmt *stmt) {
  const auto absolutePathFromColumn = [](sqlite3_stmt *row, int column) {
    std::filesystem::path path(readPath(row, column));
    if (!path.empty()) {
      chart_storage_identity::ToAbsolutePath(path);
    }
    return path;
  };
  const auto relativePathFromColumn = [](sqlite3_stmt *row, int column) {
    return std::filesystem::path(readPath(row, column));
  };
  return asobmshow::chart_sql::readChartMeta(stmt, absolutePathFromColumn,
                                             relativePathFromColumn);
}

ChartMetaRecord readChartMetaRecord(sqlite3_stmt *stmt) {
  ChartMetaRecord record;
  record.meta = readChartMeta(stmt);
  // ChartMeta consumes the first 29 source columns. The remaining shared
  // columns are SongData facts owned by the record wrapper.
  int idx = 29;
  if (sqlite3_column_count(stmt) > idx) {
    record.hasDocument = sqlite3_column_int(stmt, idx++) != 0;
  }
  if (sqlite3_column_count(stmt) > idx) {
    record.hasBpmStop = sqlite3_column_int(stmt, idx++) != 0;
  }
  if (sqlite3_column_count(stmt) > idx) {
    record.hasScrollChange = sqlite3_column_int(stmt, idx++) != 0;
  }
  if (sqlite3_column_count(stmt) > idx) {
    record.addDateSeconds = sqlite3_column_int64(stmt, idx++);
  }
  if (sqlite3_column_count(stmt) > idx) {
    record.difficultyTableLabels = columnString(stmt, idx++);
  }
  if (sqlite3_column_count(stmt) > idx) {
    record.unavailable = sqlite3_column_int(stmt, idx++) != 0;
  }
  if (sqlite3_column_count(stmt) > idx) {
    record.favorite = sqlite3_column_int(stmt, idx++) != 0;
  }
  if (sqlite3_column_count(stmt) > idx) {
    record.songReviewFavorite = sqlite3_column_int(stmt, idx++);
  }
  if (sqlite3_column_count(stmt) > idx) {
    record.downloadUrl = columnString(stmt, idx++);
  }
  if (sqlite3_column_count(stmt) > idx) {
    record.appendDownloadUrl = columnString(stmt, idx++);
  }
  if (sqlite3_column_count(stmt) > idx &&
      sqlite3_column_type(stmt, idx) != SQLITE_NULL) {
    try {
      const auto value = nlohmann::json::parse(columnString(stmt, idx));
      if (value.is_array()) {
        record.originalMd5s = value.get<std::vector<std::string>>();
      } else {
        record.originalMd5s = std::vector<std::string>{};
      }
    } catch (...) {
      record.originalMd5s = std::vector<std::string>{};
    }
  }
  ++idx;
  return record;
}

std::string difficultyTableLabelsForChart(
    sqlite3 *db, const bms_parser::ChartMeta &meta) {
  if (db == nullptr ||
      !chart_repository_detail::EnsureDifficultySchema(db)) {
    return {};
  }

  std::vector<ChartMetaRecord> records(1);
  records.front().meta = meta;
  populateDifficultyTableLabels(db, records);
  return records.front().difficultyTableLabels;
}

} // namespace

ChartMetaPathBatchReadOutcome chart_repository_detail::SelectChartMetaByPaths(
    sqlite3 *database, std::span<const std::filesystem::path> paths) {
  constexpr std::size_t kMaximumDistinctPaths = 16'384;
  constexpr std::size_t kPathsPerQuery = 256;
  ChartMetaPathBatchReadOutcome outcome;
  try {
    std::vector<std::string> normalizedPaths;
    normalizedPaths.reserve(paths.size());
    std::unordered_set<std::string> seenPaths;
    seenPaths.reserve(paths.size());
    for (const auto &path : paths) {
      const std::string normalized =
          chart_storage_identity::StoredPathText(path);
      if (normalized.empty() || !seenPaths.insert(normalized).second) {
        continue;
      }
      normalizedPaths.push_back(normalized);
      if (normalizedPaths.size() > kMaximumDistinctPaths) {
        outcome.status = ChartMetaPathBatchReadStatus::Invalid;
        outcome.diagnostic = "too many chart paths";
        return outcome;
      }
    }

    if (normalizedPaths.empty()) {
      outcome.status = ChartMetaPathBatchReadStatus::Loaded;
      return outcome;
    }

    std::string transactionError;
    SqliteTransactionHandle transaction(database, "BEGIN TRANSACTION",
                                        transactionError);
    if (!transaction.active()) {
      outcome.diagnostic = "could not begin chart metadata lookup: " +
                           transactionError;
      return outcome;
    }

    std::unordered_map<std::string, ChartMetaRecord> recordsByPath;
    recordsByPath.reserve(normalizedPaths.size());
    for (std::size_t first = 0; first < normalizedPaths.size();
         first += kPathsPerQuery) {
      const std::size_t count =
          std::min(kPathsPerQuery, normalizedPaths.size() - first);
      std::string query = "SELECT ";
      query += kChartMetaSelectColumns;
      query += ", ";
      query += "'', 0, 0, ";
      query += songReviewFavoriteColumnExpr("cm");
      query += ", '', '', NULL";
      query += " FROM chart_meta cm WHERE cm.path IN (";
      for (std::size_t index = 0; index < count; ++index) {
        query += index == 0 ? "?" : ",?";
      }
      query += ")";

      SqliteStatementHandle statement;
      if (prepareSqliteStatement(database, query, statement) != SQLITE_OK) {
        outcome.diagnostic = "could not prepare chart metadata lookup";
        return outcome;
      }
      for (std::size_t index = 0; index < count; ++index) {
        if (!bindSqliteText(statement.get(), static_cast<int>(index + 1),
                            normalizedPaths[first + index])) {
          outcome.diagnostic = "could not bind chart metadata lookup";
          return outcome;
        }
      }
      while (true) {
        const int stepResult = sqlite3_step(statement.get());
        if (stepResult == SQLITE_DONE) {
          break;
        }
        if (stepResult != SQLITE_ROW) {
          outcome.diagnostic = "could not read chart metadata lookup";
          return outcome;
        }
        ChartMetaRecord record = readChartMetaRecord(statement.get());
        recordsByPath.emplace(
            chart_storage_identity::StoredPathText(record.meta.BmsPath),
            std::move(record));
      }
    }

    if (!transaction.commit(transactionError)) {
      outcome.diagnostic = "could not complete chart metadata lookup: " +
                           transactionError;
      return outcome;
    }

    outcome.status = ChartMetaPathBatchReadStatus::Loaded;
    outcome.records.reserve(recordsByPath.size());
    for (const std::string &path : normalizedPaths) {
      auto found = recordsByPath.find(path);
      if (found == recordsByPath.end()) {
        ++outcome.missingPaths;
        continue;
      }
      outcome.records.push_back(std::move(found->second));
    }
  } catch (...) {
    outcome = {};
    outcome.diagnostic = "chart metadata lookup failed";
  }
  return outcome;
}

std::vector<bms_parser::ChartMeta>
chart_repository_detail::SelectChartMetaByHash(sqlite3 *database,
                                                const std::string &sha256,
                                                const std::string &md5) {
  using asobmshow::bms_metadata::normalizedHash;
  const std::string normalizedSha256 = normalizedHash(sha256);
  const std::string normalizedMd5 = normalizedHash(md5);
  const bool useSha256 =
      canonical_digest::isCanonicalLowerHex(normalizedSha256, 64);
  const bool useMd5 = !useSha256 &&
                      canonical_digest::isCanonicalLowerHex(normalizedMd5, 32);
  if (database == nullptr || (!useSha256 && !useMd5)) {
    return {};
  }

  std::string query = "SELECT ";
  query += kChartMetaSelectColumns;
  query += useSha256 ? " FROM chart_meta cm WHERE cm.sha256 = ?"
                     : " FROM chart_meta cm WHERE cm.md5 = ?";
  query += " ORDER BY cm.path";

  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(database, query, statement,
                                    "selecting chart metadata by hash",
                                    logSqlErrorText) ||
      !bindSqliteText(statement.get(), 1,
                     useSha256 ? normalizedSha256 : normalizedMd5)) {
    return {};
  }

  std::vector<bms_parser::ChartMeta> matches;
  while (true) {
    const int stepResult = sqlite3_step(statement.get());
    if (stepResult == SQLITE_DONE) {
      break;
    }
    if (stepResult != SQLITE_ROW) {
      logSqlError("selecting chart metadata by hash", database);
      return {};
    }
    matches.push_back(readChartMeta(statement.get()));
  }
  return matches;
}

void chart_repository_detail::SelectAllChartMeta(
    sqlite3 *database, std::vector<bms_parser::ChartMeta> &chartMetas) {
  selectAllChartMeta(database, chartMetas);
}

std::uint64_t ChartRepository::GetLibraryRevision() const {
  return gLibraryRevision.load(std::memory_order_relaxed);
}

#endif

namespace chart_repository_detail {
namespace {
using asobmshow::chart_sql::matchedChartPathSubquery;
using asobmshow::chart_sql::preferredChartPredicate;
using chart_library::FolderClearMarkCounts;

std::string_view columnText(sqlite3_stmt *stmt, int column) {
  return sqliteColumnTextView(stmt, column);
}

struct FolderClearAggregate {
  bool hasChart = false;
  int minimumClearRank = std::numeric_limits<int>::max();
  bool hasUnclearedChart = false;

  void addChart(int clearRank) {
    hasChart = true;
    if (clearRank < kClearTypeAssistedEasyClearRank) {
      hasUnclearedChart = true;
      return;
    }
    minimumClearRank = std::min(minimumClearRank, clearRank);
  }

  [[nodiscard]] int clearRank() const {
    if (!hasChart || hasUnclearedChart ||
        minimumClearRank == std::numeric_limits<int>::max()) {
      return kNoClearTypeRank;
    }
    return minimumClearRank;
  }
};

void addClearMarkCount(FolderClearMarkCounts &counts,
                       const std::string &folderKey, int clearRank) {
  if (clearRank >= kNoClearTypeRank) {
    counts[folderKey][clearRank]++;
  }
}

using FolderClearAggregateByLongNoteMode =
    std::array<FolderClearAggregate, 4>;

void addFolderChartForAllLongNoteModes(
    std::unordered_map<std::string, FolderClearAggregateByLongNoteMode>
        &aggregates,
    const ScoreClearRankCache &scoreRanks, const std::string &folderKey,
    std::string_view sha256, int chartLongNoteMode, int totalLongNotes,
    int totalBackSpinNotes) {
  auto &aggregateByMode = aggregates[folderKey];
  for (int selectedLongNoteMode : long_note_mode::kPlayableValues) {
    auto &aggregate =
        aggregateByMode[static_cast<std::size_t>(selectedLongNoteMode)];
    if (aggregate.hasUnclearedChart) {
      continue;
    }
    const int longNoteMode = scoreLongNoteModeForClearLamp(
        chartLongNoteMode, totalLongNotes, totalBackSpinNotes,
        selectedLongNoteMode);
    aggregate.addChart(scoreRanks.bestRankForStoredKey(sha256, longNoteMode));
  }
}

void addClearMarkCountsForAllLongNoteModes(
    std::array<FolderClearMarkCounts, 4> &countsByMode,
    const ScoreClearRankCache &scoreRanks, const std::string &folderKey,
    std::string_view sha256, int chartLongNoteMode, int totalLongNotes,
    int totalBackSpinNotes) {
  for (int selectedLongNoteMode : long_note_mode::kPlayableValues) {
    const int longNoteMode = scoreLongNoteModeForClearLamp(
        chartLongNoteMode, totalLongNotes, totalBackSpinNotes,
        selectedLongNoteMode);
    const int clearRank =
        scoreRanks.bestRankForStoredKey(sha256, longNoteMode);
    addClearMarkCount(
        countsByMode[static_cast<std::size_t>(selectedLongNoteMode)], folderKey,
        clearRank);
  }
}
} // namespace

chart_library::FolderClearDataByLongNoteMode
LoadFolderClearDataByLongNoteMode(
    sqlite3 *db, const ScoreClearRankCache &projectedChartRanks,
    const ScoreClearRankCache &localCourseRanks) {
  chart_library::FolderClearDataByLongNoteMode data;
  std::unordered_map<std::string, FolderClearAggregateByLongNoteMode>
      aggregates;

  auto runQuery = [&](const std::string &query, const auto &handleRow) {
    SqliteStatementHandle stmt;
    if (prepareSqliteStatement(db, query, stmt) != SQLITE_OK) {
      SDL_Log("SQL error while loading folder clear data: %s",
              sqlite3_errmsg(db));
      return;
    }
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      handleRow(stmt.get());
    }
  };

  runQuery("SELECT cm.sha256, cm.ln_mode, "
           "cm.total_long_notes, cm.total_backspin_notes "
           "FROM chart_meta cm WHERE " +
               preferredChartPredicate("cm"),
           [&](sqlite3_stmt *row) {
             addFolderChartForAllLongNoteModes(
                 aggregates, projectedChartRanks, "all", columnText(row, 0),
                 sqlite3_column_int(row, 1), sqlite3_column_int(row, 2),
                 sqlite3_column_int(row, 3));
             addClearMarkCountsForAllLongNoteModes(
                 data.clearMarkCounts, projectedChartRanks, "all",
                 columnText(row, 0), sqlite3_column_int(row, 1),
                 sqlite3_column_int(row, 2), sqlite3_column_int(row, 3));
           });

  int currentTableId = 0;
  std::string currentTableKey;
  std::string currentLevel;
  std::string currentLevelKey;
  runQuery(
      "SELECT dte.table_id, dte.level, "
      "COALESCE(NULLIF(dte.sha256, ''), cm.sha256, ''), "
      "COALESCE(cm.ln_mode, 0), COALESCE(cm.total_long_notes, 0), "
      "COALESCE(cm.total_backspin_notes, 0) "
      "FROM difficulty_table_entries dte "
      "LEFT JOIN chart_meta cm ON cm.path = " +
          matchedChartPathSubquery("dte") + " "
          "ORDER BY dte.table_id, dte.level",
      [&](sqlite3_stmt *row) {
        const int tableId = sqlite3_column_int(row, 0);
        const std::string_view level = columnText(row, 1);
        if (tableId != currentTableId) {
          currentTableId = tableId;
          currentTableKey = chart_library::folderKeyForTable(tableId);
          currentLevel.clear();
          currentLevelKey.clear();
        }
        if (level != std::string_view(currentLevel)) {
          currentLevel = std::string(level);
          currentLevelKey =
              chart_library::folderKeyForLevel(tableId, currentLevel);
        }

        addFolderChartForAllLongNoteModes(
            aggregates, projectedChartRanks, currentTableKey,
            columnText(row, 2), sqlite3_column_int(row, 3),
            sqlite3_column_int(row, 4), sqlite3_column_int(row, 5));
        addFolderChartForAllLongNoteModes(
            aggregates, projectedChartRanks, currentLevelKey,
            columnText(row, 2), sqlite3_column_int(row, 3),
            sqlite3_column_int(row, 4), sqlite3_column_int(row, 5));
        addClearMarkCountsForAllLongNoteModes(
            data.clearMarkCounts, projectedChartRanks, currentTableKey,
            columnText(row, 2), sqlite3_column_int(row, 3),
            sqlite3_column_int(row, 4), sqlite3_column_int(row, 5));
        addClearMarkCountsForAllLongNoteModes(
            data.clearMarkCounts, projectedChartRanks, currentLevelKey,
            columnText(row, 2), sqlite3_column_int(row, 3),
            sqlite3_column_int(row, 4), sqlite3_column_int(row, 5));
      });

  int currentCourseId = 0;
  int currentCourseTableId = 0;
  std::string currentCourseTableKey;
  int currentCourseGroupTableId = 0;
  std::string currentCourseGroupName;
  std::string currentCourseGroupKey;
  std::string currentCourseKey;
  runQuery(
      "SELECT dc.id, dc.table_id, dc.group_name, "
      "COALESCE(NULLIF(dce.sha256, ''), cm.sha256, ''), "
      "COALESCE(cm.ln_mode, 0), COALESCE(cm.total_long_notes, 0), "
      "COALESCE(cm.total_backspin_notes, 0) "
      "FROM difficulty_courses dc "
      "JOIN difficulty_course_entries dce ON dce.course_id = dc.id "
      "LEFT JOIN chart_meta cm ON cm.path = " +
          matchedChartPathSubquery("dce") + " "
          "ORDER BY dc.table_id, dc.group_name, dc.id, dce.sort_order",
      [&](sqlite3_stmt *row) {
        const int courseId = sqlite3_column_int(row, 0);
        const int tableId = sqlite3_column_int(row, 1);
        const std::string_view groupName = columnText(row, 2);
        if (tableId != currentCourseTableId) {
          currentCourseTableId = tableId;
          currentCourseTableKey =
              chart_library::folderKeyForCourseTable(tableId);
        }
        if (tableId != currentCourseGroupTableId ||
            groupName != std::string_view(currentCourseGroupName)) {
          currentCourseGroupTableId = tableId;
          currentCourseGroupName = std::string(groupName);
          currentCourseGroupKey = chart_library::folderKeyForCourseGroup(
              tableId, currentCourseGroupName);
        }
        if (courseId != currentCourseId) {
          currentCourseId = courseId;
          currentCourseKey = chart_library::folderKeyForCourse(courseId);
        }

        addFolderChartForAllLongNoteModes(
            aggregates, localCourseRanks, "courses", columnText(row, 3),
            sqlite3_column_int(row, 4), sqlite3_column_int(row, 5),
            sqlite3_column_int(row, 6));
        addFolderChartForAllLongNoteModes(
            aggregates, localCourseRanks, currentCourseTableKey,
            columnText(row, 3), sqlite3_column_int(row, 4),
            sqlite3_column_int(row, 5), sqlite3_column_int(row, 6));
        addFolderChartForAllLongNoteModes(
            aggregates, localCourseRanks, currentCourseGroupKey,
            columnText(row, 3), sqlite3_column_int(row, 4),
            sqlite3_column_int(row, 5), sqlite3_column_int(row, 6));
        addFolderChartForAllLongNoteModes(
            aggregates, localCourseRanks, currentCourseKey, columnText(row, 3),
            sqlite3_column_int(row, 4), sqlite3_column_int(row, 5),
            sqlite3_column_int(row, 6));
      });

  for (const auto &[key, aggregateByMode] : aggregates) {
    for (int selectedLongNoteMode : long_note_mode::kPlayableValues) {
      const int clearRank =
          aggregateByMode[static_cast<std::size_t>(selectedLongNoteMode)]
              .clearRank();
      if (clearRank >= kClearTypeAssistedEasyClearRank) {
        data.clearRanks[static_cast<std::size_t>(selectedLongNoteMode)][key] =
            clearRank;
      }
    }
  }
  runQuery(
      "SELECT id, COALESCE(course_key, '') FROM difficulty_courses",
      [&](sqlite3_stmt *row) {
        const int courseId = sqlite3_column_int(row, 0);
        const std::string_view courseKey = columnText(row, 1);
        const std::string folderKey =
            chart_library::folderKeyForCourse(courseId);
        for (int selectedLongNoteMode : long_note_mode::kPlayableValues) {
          const int clearRank = localCourseRanks.bestCourseRankFor(
              courseKey, courseId, selectedLongNoteMode);
          if (clearRank >= kClearTypeAssistedEasyClearRank) {
            data.clearRanks[static_cast<std::size_t>(selectedLongNoteMode)]
                           [folderKey] = clearRank;
          }
        }
      });

  return data;
}

} // namespace chart_repository_detail

#if defined(ASOBMASHOW_FOLDER_QUERY_ONLY)
namespace repository_test {

chart_library::FolderClearDataByLongNoteMode
loadFolderClearDataByLongNoteMode(
    sqlite3 *database, const ScoreClearRankCache &projectedChartRanks,
    const ScoreClearRankCache &localCourseRanks) {
  return chart_repository_detail::LoadFolderClearDataByLongNoteMode(
      database, projectedChartRanks, localCourseRanks);
}

} // namespace repository_test
#else
chart_library::FolderClearDataByLongNoteMode
ChartRepository::Session::LoadFolderClearDataByLongNoteMode(
    const ScoreClearRankCache &projectedChartRanks,
    const ScoreClearRankCache &localCourseRanks) {
  return chart_repository_detail::LoadFolderClearDataByLongNoteMode(
      impl_->database(), projectedChartRanks, localCourseRanks);
}
#endif
