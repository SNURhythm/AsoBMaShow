#include "ChartRepository.h"
#include "ChartRepositoryInternal.h"

#include "../ArchiveFile.h"
#include "../BmsMetadataText.h"
#include "../path.h"
#include "ChartStorageIdentity.h"
#include "SqliteRAII.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace {
using asobmshow::bms_metadata::normalizedHash;

void logSqlErrorText(const char *context, const std::string &error) {
  std::cerr << "SQL error while " << context << ": " << error << "\n";
}

void logSqlError(const char *context, sqlite3 *database) {
  logSqlErrorText(context, sqliteDatabaseError(database));
}

void logSdlSqlErrorText(const char *context, const std::string &error) {
  SDL_Log("SQL error while %s: %s", context, error.c_str());
}

void logSdlSqlError(const char *context, sqlite3 *database) {
  logSdlSqlErrorText(context, sqliteDatabaseError(database));
}

sqlite3_int64 clampSqlInteger(std::uint64_t value) {
  return value > static_cast<std::uint64_t>(
                     std::numeric_limits<sqlite3_int64>::max())
             ? std::numeric_limits<sqlite3_int64>::max()
             : static_cast<sqlite3_int64>(value);
}

sqlite3_int64 fileTimeToSqlNs(std::filesystem::file_time_type time) {
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         time.time_since_epoch())
                         .count();
  if (nanos > std::numeric_limits<sqlite3_int64>::max()) {
    return std::numeric_limits<sqlite3_int64>::max();
  }
  if (nanos < std::numeric_limits<sqlite3_int64>::min()) {
    return std::numeric_limits<sqlite3_int64>::min();
  }
  return static_cast<sqlite3_int64>(nanos);
}

bool archiveFileStateForDatabase(const std::filesystem::path &path,
                                 sqlite3_int64 &archiveSize,
                                 sqlite3_int64 &mtimeNs) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return false;
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return false;
  }
  const auto mtime = std::filesystem::last_write_time(path, error);
  if (error) {
    return false;
  }
  archiveSize =
      clampSqlInteger(static_cast<std::uint64_t>(std::min<std::uintmax_t>(
          size, std::numeric_limits<std::uint64_t>::max())));
  mtimeNs = fileTimeToSqlNs(mtime);
  return true;
}

std::filesystem::path storedPathFromDatabase(const std::string &text) {
  std::filesystem::path path(utf8_to_path_t(text));
  if (!path.empty()) {
    chart_storage_identity::ToAbsolutePath(path);
  }
  return path;
}

bool pathIsInsideDirectory(const std::filesystem::path &path,
                           const std::filesystem::path &directory) {
  if (path.empty() || directory.empty()) {
    return false;
  }
  const std::filesystem::path normalizedPath = path.lexically_normal();
  const std::filesystem::path normalizedDirectory =
      directory.lexically_normal();
  if (normalizedPath == normalizedDirectory) {
    return false;
  }
  const std::filesystem::path relative =
      normalizedPath.lexically_relative(normalizedDirectory);
  if (relative.empty() || relative.is_absolute()) {
    return false;
  }
  const auto first = relative.begin();
  return first != relative.end() && *first != std::filesystem::path("..") &&
         *first != std::filesystem::path(".");
}

const char *insertChartMetaSql() {
  return "INSERT INTO chart_meta ("
         "path,"
         "md5,"
         "sha256,"
         "title,"
         "subtitle,"
         "genre,"
         "artist,"
         "sub_artist,"
         "folder,"
         "stage_file,"
         "banner,"
         "back_bmp,"
         "preview,"
         "level,"
         "difficulty,"
         "total,"
         "has_total,"
         "bpm,"
         "max_bpm,"
         "min_bpm,"
         "length,"
         "rank,"
         "player,"
         "keys,"
         "total_notes,"
         "total_long_notes,"
         "total_scratch_notes,"
         "total_backspin_notes,"
         "ln_mode,"
         "has_document,"
         "has_bpm_stop,"
         "has_scroll_change,"
         "add_date,"
         "total_landmine_notes,"
         "has_random_sequence,"
         "most_prevalent_bpm,"
         "has_bga,"
         "source_priority,"
         "source_archive_size"
         ") VALUES("
         "@path,"
         "@md5,"
         "@sha256,"
         "@title,"
         "@subtitle,"
         "@genre,"
         "@artist,"
         "@sub_artist,"
         "@folder,"
         "@stage_file,"
         "@banner,"
         "@back_bmp,"
         "@preview,"
         "@level,"
         "@difficulty,"
         "@total,"
         "@has_total,"
         "@bpm,"
         "@max_bpm,"
         "@min_bpm,"
         "@length,"
         "@rank,"
         "@player,"
         "@keys,"
         "@total_notes,"
         "@total_long_notes,"
         "@total_scratch_notes,"
         "@total_backspin_notes,"
         "@ln_mode,"
         "@has_document,"
         "@has_bpm_stop,"
         "@has_scroll_change,"
         "@add_date,"
         "@total_landmine_notes,"
         "@has_random_sequence,"
         "@most_prevalent_bpm,"
         "@has_bga,"
         "@source_priority,"
         "@source_archive_size"
         ") ON CONFLICT(path) DO UPDATE SET "
         "md5=excluded.md5,"
         "sha256=excluded.sha256,"
         "title=excluded.title,"
         "subtitle=excluded.subtitle,"
         "genre=excluded.genre,"
         "artist=excluded.artist,"
         "sub_artist=excluded.sub_artist,"
         "folder=excluded.folder,"
         "stage_file=excluded.stage_file,"
         "banner=excluded.banner,"
         "back_bmp=excluded.back_bmp,"
         "preview=excluded.preview,"
         "level=excluded.level,"
         "difficulty=excluded.difficulty,"
         "total=excluded.total,"
         "has_total=excluded.has_total,"
         "bpm=excluded.bpm,"
         "max_bpm=excluded.max_bpm,"
         "min_bpm=excluded.min_bpm,"
         "length=excluded.length,"
         "rank=excluded.rank,"
         "player=excluded.player,"
         "keys=excluded.keys,"
         "total_notes=excluded.total_notes,"
         "total_long_notes=excluded.total_long_notes,"
         "total_scratch_notes=excluded.total_scratch_notes,"
         "total_backspin_notes=excluded.total_backspin_notes,"
         "ln_mode=excluded.ln_mode,"
         "has_document=excluded.has_document,"
         "has_bpm_stop=excluded.has_bpm_stop,"
         "has_scroll_change=excluded.has_scroll_change,"
         "total_landmine_notes=excluded.total_landmine_notes,"
         "has_random_sequence=excluded.has_random_sequence,"
         "most_prevalent_bpm=excluded.most_prevalent_bpm,"
         "has_bga=excluded.has_bga,"
         "source_priority=excluded.source_priority,"
         "source_archive_size=excluded.source_archive_size";
}

bool bindAndInsertChartMeta(
    sqlite3 *database, sqlite3_stmt *statement,
    const bms_parser::ChartMeta &chartMeta,
    const std::optional<ChartSourcePreference> &sourcePreferenceHint,
    bool hasDocument, ChartSequenceFeatures sequenceFeatures,
    std::int64_t addDateSeconds) {
  if (statement == nullptr) {
    logSdlSqlErrorText("inserting a chart", "statement is not prepared");
    return false;
  }
  ChartSourcePreference sourcePreference;
  if (sourcePreferenceHint.has_value()) {
    sourcePreference = *sourcePreferenceHint;
  } else {
    const auto inferred =
        archive_file::sourcePreferenceForPath(chartMeta.BmsPath);
    sourcePreference = {
        .priority = inferred.priority,
        .archiveSize = inferred.archiveSize,
    };
  }

  bindSqliteText(statement, 1,
                 chart_storage_identity::StoredPathText(chartMeta.BmsPath));
  bindSqliteText(statement, 2, normalizedHash(chartMeta.MD5));
  bindSqliteText(statement, 3, normalizedHash(chartMeta.SHA256));
  bindSqliteText(statement, 4, chartMeta.Title);
  bindSqliteText(statement, 5, chartMeta.SubTitle);
  bindSqliteText(statement, 6, chartMeta.Genre);
  bindSqliteText(statement, 7, chartMeta.Artist);
  bindSqliteText(statement, 8, chartMeta.SubArtist);

  bindSqliteText(
      statement, 9,
      chart_storage_identity::StoredFolderPathText(chartMeta.Folder));
  bindSqliteText(statement, 10, fspath_to_utf8(chartMeta.StageFile));
  bindSqliteText(statement, 11, fspath_to_utf8(chartMeta.Banner));
  bindSqliteText(statement, 12, fspath_to_utf8(chartMeta.BackBmp));
  bindSqliteText(statement, 13, fspath_to_utf8(chartMeta.Preview));
  sqlite3_bind_double(statement, 14, chartMeta.PlayLevel);
  sqlite3_bind_int(statement, 15, chartMeta.Difficulty);
  sqlite3_bind_double(statement, 16, chartMeta.Total);
  sqlite3_bind_int(statement, 17, chartMeta.HasTotal ? 1 : 0);
  sqlite3_bind_double(statement, 18, chartMeta.Bpm);
  sqlite3_bind_double(statement, 19, chartMeta.MaxBpm);
  sqlite3_bind_double(statement, 20, chartMeta.MinBpm);
  sqlite3_bind_int64(statement, 21, chartMeta.PlayLength);
  sqlite3_bind_int(statement, 22, chartMeta.Rank);
  sqlite3_bind_int(statement, 23, chartMeta.Player);
  sqlite3_bind_int(statement, 24, chartMeta.KeyMode);
  sqlite3_bind_int(statement, 25, chartMeta.TotalNotes);
  sqlite3_bind_int(statement, 26, chartMeta.TotalLongNotes);
  sqlite3_bind_int(statement, 27, chartMeta.TotalScratchNotes);
  sqlite3_bind_int(statement, 28, chartMeta.TotalBackSpinNotes);
  sqlite3_bind_int(statement, 29, chartMeta.LnMode);
  sqlite3_bind_int(statement, 30, hasDocument ? 1 : 0);
  sqlite3_bind_int(statement, 31, sequenceFeatures.hasBpmStop ? 1 : 0);
  sqlite3_bind_int(statement, 32, sequenceFeatures.hasScrollChange ? 1 : 0);
  sqlite3_bind_int64(statement, 33, addDateSeconds);
  sqlite3_bind_int(statement, 34, chartMeta.TotalLandmineNotes);
  sqlite3_bind_int(statement, 35, chartMeta.RandomValues.empty() ? 0 : 1);
  sqlite3_bind_double(statement, 36, chartMeta.MostPrevalentBpm);
  sqlite3_bind_int(statement, 37, sequenceFeatures.hasBga ? 1 : 0);
  sqlite3_bind_int(statement, 38, sourcePreference.priority);
  sqlite3_bind_int64(statement, 39,
                     clampSqlInteger(sourcePreference.archiveSize));
  if (sqlite3_step(statement) != SQLITE_DONE) {
    logSdlSqlError("inserting a chart", database);
    return false;
  }
  return true;
}

bool selectScanCheckpoint(sqlite3 *database, ChartScanCheckpoint &checkpoint) {
  const char *query =
      "SELECT scan_signature, phase, next_index, sub_index, last_path, "
      "archive_path, archive_size, archive_mtime_ns, last_inner_path "
      "FROM chart_scan_checkpoint WHERE id = 1";
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(database, query, statement,
                                    "selecting chart scan checkpoint",
                                    logSqlErrorText)) {
    return false;
  }
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return true;
  }
  checkpoint.found = true;
  checkpoint.scanSignature = sqliteColumnString(statement.get(), 0);
  checkpoint.phase = sqliteColumnString(statement.get(), 1);
  checkpoint.nextIndex = std::max(0, sqlite3_column_int(statement.get(), 2));
  checkpoint.subIndex = std::max(0, sqlite3_column_int(statement.get(), 3));
  checkpoint.lastPath =
      storedPathFromDatabase(sqliteColumnString(statement.get(), 4));
  checkpoint.archivePath =
      storedPathFromDatabase(sqliteColumnString(statement.get(), 5));
  checkpoint.archiveSize = sqlite3_column_int64(statement.get(), 6);
  checkpoint.archiveMtimeNs = sqlite3_column_int64(statement.get(), 7);
  checkpoint.lastInnerPath = sqliteColumnString(statement.get(), 8);
  return true;
}

bool upsertScanCheckpoint(sqlite3 *database,
                          const ChartScanCheckpoint &checkpoint) {
  const char *query =
      "INSERT INTO chart_scan_checkpoint "
      "(id, scan_signature, phase, next_index, sub_index, last_path, "
      "archive_path, archive_size, archive_mtime_ns, last_inner_path, "
      "updated_at) VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
      "CURRENT_TIMESTAMP) "
      "ON CONFLICT(id) DO UPDATE SET "
      "scan_signature = excluded.scan_signature,"
      "phase = excluded.phase,"
      "next_index = excluded.next_index,"
      "sub_index = excluded.sub_index,"
      "last_path = excluded.last_path,"
      "archive_path = excluded.archive_path,"
      "archive_size = excluded.archive_size,"
      "archive_mtime_ns = excluded.archive_mtime_ns,"
      "last_inner_path = excluded.last_inner_path,"
      "updated_at = CURRENT_TIMESTAMP";
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(database, query, statement,
                                    "preparing chart scan checkpoint upsert",
                                    logSqlErrorText)) {
    return false;
  }
  bindSqliteText(statement.get(), 1, checkpoint.scanSignature);
  bindSqliteText(statement.get(), 2, checkpoint.phase);
  sqlite3_bind_int(statement.get(), 3, std::max(0, checkpoint.nextIndex));
  sqlite3_bind_int(statement.get(), 4, std::max(0, checkpoint.subIndex));
  bindSqliteText(statement.get(), 5,
                 chart_storage_identity::StoredPathText(checkpoint.lastPath));
  bindSqliteText(
      statement.get(), 6,
      chart_storage_identity::StoredPathText(checkpoint.archivePath));
  sqlite3_bind_int64(statement.get(), 7, checkpoint.archiveSize);
  sqlite3_bind_int64(statement.get(), 8, checkpoint.archiveMtimeNs);
  bindSqliteText(statement.get(), 9, checkpoint.lastInnerPath);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    logSqlError("upserting chart scan checkpoint", database);
    return false;
  }
  return true;
}

bool clearScanCheckpoint(sqlite3 *database) {
  return executeSqliteLogged(database, "DELETE FROM chart_scan_checkpoint",
                             "clearing chart scan checkpoint", logSqlErrorText);
}

bool clearMetadataRebuildRequired(sqlite3 *database) {
  bool exists = false;
  if (const auto error = querySqliteTableExists(
          database, "chart_meta_rebuild_state", exists)) {
    logSqlErrorText("checking chart metadata rebuild state table", *error);
    return false;
  }
  if (!exists) {
    return true;
  }
  const char *query =
      "INSERT INTO chart_meta_rebuild_state (id, required, updated_at) "
      "VALUES (1, ?, CURRENT_TIMESTAMP) "
      "ON CONFLICT(id) DO UPDATE SET required = excluded.required, "
      "updated_at = CURRENT_TIMESTAMP";
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(database, query, statement,
                                    "preparing chart metadata rebuild state",
                                    logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(statement.get(), 1, 0);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    logSqlError("updating chart metadata rebuild state", database);
    return false;
  }
  return true;
}

ChartScanSnapshot loadScanSnapshot(sqlite3 *database,
                                   ChartScanSnapshotLoad load) {
  ChartScanSnapshot snapshot;
  if (load == ChartScanSnapshotLoad::Full) {
    chart_repository_detail::SelectAllChartMeta(database, snapshot.charts);

    SqliteStatementHandle solidStatement;
    if (prepareSqliteStatementLogged(
            database, "SELECT path FROM solid_archives", solidStatement,
            "selecting solid archive paths", logSqlErrorText)) {
      while (sqlite3_step(solidStatement.get()) == SQLITE_ROW) {
        snapshot.solidArchives.push_back(
            {.path = storedPathFromDatabase(
                 sqliteColumnString(solidStatement.get(), 0))});
      }
    }

    const char *cacheQuery =
        "SELECT path, archive_size, mtime_ns, solid, uncompressed_size, "
        "file_count, chart_count FROM archive_scan_cache";
    SqliteStatementHandle cacheStatement;
    if (prepareSqliteStatementLogged(database, cacheQuery, cacheStatement,
                                     "selecting archive scan cache",
                                     logSqlErrorText)) {
      while (sqlite3_step(cacheStatement.get()) == SQLITE_ROW) {
        snapshot.archiveCache.push_back({
            .path = storedPathFromDatabase(
                sqliteColumnString(cacheStatement.get(), 0)),
            .archiveSize = sqlite3_column_int64(cacheStatement.get(), 1),
            .mtimeNs = sqlite3_column_int64(cacheStatement.get(), 2),
            .solid = sqlite3_column_int(cacheStatement.get(), 3) != 0,
            .uncompressedSize =
                static_cast<std::uint64_t>(std::max<sqlite3_int64>(
                    0, sqlite3_column_int64(cacheStatement.get(), 4))),
            .fileCount =
                std::max(0, sqlite3_column_int(cacheStatement.get(), 5)),
            .chartCount = sqlite3_column_int(cacheStatement.get(), 6),
        });
      }
    }
  }

  ChartScanCheckpoint checkpoint;
  if (selectScanCheckpoint(database, checkpoint) && checkpoint.found) {
    snapshot.checkpoint = std::move(checkpoint);
  }
  return snapshot;
}
} // namespace

bool ChartRepository::Session::InsertChartMeta(
    bms_parser::ChartMeta &chartMeta) {
  sqlite3 *database = impl_->database();
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(
          database, insertChartMetaSql(), statement,
          "preparing statement to insert a chart", logSdlSqlErrorText)) {
    return false;
  }
  if (!bindAndInsertChartMeta(database, statement.get(), chartMeta,
                              std::nullopt, false, {},
                              std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now()
                                      .time_since_epoch())
                                  .count())) {
    return false;
  }
  chart_repository_detail::BumpLibraryRevision();
  return true;
}

struct ChartRepository::Session::ScanBatch::Impl {
  explicit Impl(std::shared_ptr<ChartSessionStorage> storageValue)
      : storage(std::move(storageValue)),
        addDateSeconds(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count()) {
    if (!beginTransaction()) {
      return;
    }
    ready = true;
  }

  sqlite3 *database() const { return storage->database(); }

  bool beginTransaction() {
    std::string error;
    auto next =
        std::make_unique<SqliteTransactionHandle>(database(), "BEGIN", error);
    if (!next->active()) {
      SDL_Log("Failed to begin chart scan transaction: %s", error.c_str());
      return false;
    }
    transaction = std::move(next);
    return true;
  }

  bool commitTransaction() {
    if (transaction == nullptr || !transaction->active()) {
      return false;
    }
    std::string error;
    if (!transaction->commit(error)) {
      SDL_Log("Failed to commit chart scan transaction: %s", error.c_str());
      return false;
    }
    transaction.reset();
    return true;
  }

  void noteChanged() {
    if (sqlite3_changes(database()) > 0) {
      ++changedCount;
    }
  }

  void noteFolderChanged(bool changed = true) {
    folderChanged = folderChanged || changed;
  }

  bool ensureChartInsertStatement() {
    if (chartInsertStatementReady) {
      return true;
    }
    if (chartInsertStatementAttempted) {
      return false;
    }
    chartInsertStatementAttempted = true;
    chartInsertStatementReady = prepareSqliteStatementLogged(
        database(), insertChartMetaSql(), chartInsertStatement,
        "preparing statement to insert chart scan batch", logSdlSqlErrorText);
    return chartInsertStatementReady;
  }

  std::shared_ptr<ChartSessionStorage> storage;
  std::unique_ptr<SqliteTransactionHandle> transaction;
  SqliteStatementHandle chartInsertStatement;
  const std::int64_t addDateSeconds;
  int changedCount = 0;
  bool folderChanged = false;
  bool ready = false;
  bool committed = false;
  bool chartInsertStatementAttempted = false;
  bool chartInsertStatementReady = false;
};

ChartRepository::Session::ScanBatch::ScanBatch(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ChartRepository::Session::ScanBatch::~ScanBatch() = default;
ChartRepository::Session::ScanBatch::ScanBatch(ScanBatch &&) noexcept = default;
ChartRepository::Session::ScanBatch &
ChartRepository::Session::ScanBatch::operator=(ScanBatch &&) noexcept = default;

bool ChartRepository::Session::ScanBatch::UpsertChart(
    const bms_parser::ChartMeta &meta,
    std::optional<ChartSourcePreference> sourcePreference, bool hasDocument,
    ChartSequenceFeatures sequenceFeatures) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed) {
    return false;
  }
  if (!impl_->ensureChartInsertStatement()) {
    return false;
  }
  sqlite3_reset(impl_->chartInsertStatement.get());
  sqlite3_clear_bindings(impl_->chartInsertStatement.get());
  if (!bindAndInsertChartMeta(impl_->database(),
                              impl_->chartInsertStatement.get(), meta,
                              sourcePreference, hasDocument,
                              sequenceFeatures, impl_->addDateSeconds)) {
    return false;
  }
  impl_->noteChanged();
  return true;
}

bool ChartRepository::Session::ScanBatch::UpdateChartHasDocument(
    const std::filesystem::path &path, bool hasDocument) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed) {
    return false;
  }
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(
          impl_->database(),
          "UPDATE chart_meta SET has_document = @has_document "
          "WHERE path = @path AND has_document != @has_document",
          statement, "preparing chart document flag update", logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(statement.get(), 1, hasDocument ? 1 : 0);
  bindSqliteText(statement.get(), 2,
                 chart_storage_identity::StoredPathText(path));
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    logSqlError("updating chart document flag", impl_->database());
    return false;
  }
  if (sqlite3_changes(impl_->database()) > 0) {
    impl_->noteChanged();
  }
  return true;
}

bool ChartRepository::Session::ScanBatch::DeleteChart(
    const std::filesystem::path &path) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed) {
    return false;
  }
  std::filesystem::path relative = path;
  chart_storage_identity::ToRelativePath(relative);
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(
          impl_->database(), "DELETE FROM chart_meta WHERE path = @path",
          statement, "preparing statement to delete a chart",
          logSqlErrorText)) {
    return false;
  }
  bindSqliteText(statement.get(), 1, fspath_to_utf8(relative));
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    logSqlError("deleting a chart", impl_->database());
    return false;
  }
  impl_->noteChanged();
  return true;
}

bool ChartRepository::Session::ScanBatch::DeleteCharts(
    std::span<const std::filesystem::path> paths) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed) {
    return false;
  }
  if (paths.empty()) {
    return true;
  }
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(
          impl_->database(), "DELETE FROM chart_meta WHERE path = @path",
          statement, "preparing statement to delete chart paths",
          logSqlErrorText)) {
    return false;
  }
  bool changed = false;
  for (const auto &path : paths) {
    std::filesystem::path relative = path;
    chart_storage_identity::ToRelativePath(relative);
    sqlite3_reset(statement.get());
    sqlite3_clear_bindings(statement.get());
    bindSqliteText(statement.get(), 1, fspath_to_utf8(relative));
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
      logSqlError("deleting chart paths", impl_->database());
      return false;
    }
    changed = changed || sqlite3_changes(impl_->database()) > 0;
  }
  if (changed) {
    impl_->noteChanged();
  }
  return true;
}

bool ChartRepository::Session::ScanBatch::DeleteChartsInArchive(
    const std::filesystem::path &archivePath) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed) {
    return false;
  }
  std::vector<std::filesystem::path> chartPaths;
  SqliteStatementHandle selectStatement;
  if (!prepareSqliteStatementLogged(
          impl_->database(), "SELECT path FROM chart_meta", selectStatement,
          "selecting archive chart paths", logSqlErrorText)) {
    return false;
  }
  while (sqlite3_step(selectStatement.get()) == SQLITE_ROW) {
    chartPaths.push_back(
        storedPathFromDatabase(sqliteColumnString(selectStatement.get(), 0)));
  }

  SqliteStatementHandle deleteStatement;
  if (!prepareSqliteStatementLogged(
          impl_->database(), "DELETE FROM chart_meta WHERE path = ?",
          deleteStatement, "preparing archive chart delete", logSqlErrorText)) {
    return false;
  }
  bool changed = false;
  const auto target = archivePath.lexically_normal();
  for (const auto &path : chartPaths) {
    if (!pathIsInsideDirectory(path, target)) {
      continue;
    }
    sqlite3_reset(deleteStatement.get());
    sqlite3_clear_bindings(deleteStatement.get());
    bindSqliteText(deleteStatement.get(), 1,
                   chart_storage_identity::StoredPathText(path));
    if (sqlite3_step(deleteStatement.get()) != SQLITE_DONE) {
      logSqlError("deleting archive chart", impl_->database());
      return false;
    }
    if (!changed && sqlite3_changes(impl_->database()) > 0) {
      changed = true;
      ++impl_->changedCount;
    }
  }
  return true;
}

bool ChartRepository::Session::ScanBatch::DeleteSolidArchive(
    const std::filesystem::path &path) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed) {
    return false;
  }
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(
          impl_->database(), "DELETE FROM solid_archives WHERE path = ?",
          statement, "preparing solid archive delete", logSqlErrorText)) {
    return false;
  }
  bindSqliteText(statement.get(), 1,
                 chart_storage_identity::StoredPathText(path));
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    logSqlError("deleting solid archive", impl_->database());
    return false;
  }
  impl_->noteChanged();
  return true;
}

bool ChartRepository::Session::ScanBatch::DeleteArchiveCache(
    const std::filesystem::path &path) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed) {
    return false;
  }
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(
          impl_->database(), "DELETE FROM archive_scan_cache WHERE path = ?",
          statement, "preparing archive scan cache delete", logSqlErrorText)) {
    return false;
  }
  bindSqliteText(statement.get(), 1,
                 chart_storage_identity::StoredPathText(path));
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    logSqlError("deleting archive scan cache", impl_->database());
    return false;
  }
  impl_->noteChanged();
  return true;
}

bool ChartRepository::Session::ScanBatch::UpsertSolidArchive(
    const SolidArchiveUpdate &update) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed) {
    return false;
  }
  sqlite3_int64 archiveSize = 0;
  sqlite3_int64 mtimeNs = 0;
  if (!archiveFileStateForDatabase(update.path, archiveSize, mtimeNs)) {
    return false;
  }
  const std::string name = update.path.filename().generic_string().empty()
                               ? update.path.generic_string()
                               : update.path.filename().generic_string();
  const char *query =
      "INSERT INTO solid_archives "
      "(path, name, archive_size, uncompressed_size, file_count, mtime_ns, "
      "updated_at) VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP) "
      "ON CONFLICT(path) DO UPDATE SET "
      "name = excluded.name,"
      "archive_size = excluded.archive_size,"
      "uncompressed_size = excluded.uncompressed_size,"
      "file_count = excluded.file_count,"
      "mtime_ns = excluded.mtime_ns,"
      "updated_at = CURRENT_TIMESTAMP "
      "WHERE solid_archives.name != excluded.name "
      "OR solid_archives.archive_size != excluded.archive_size "
      "OR solid_archives.uncompressed_size != excluded.uncompressed_size "
      "OR solid_archives.file_count != excluded.file_count "
      "OR solid_archives.mtime_ns != excluded.mtime_ns";
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(impl_->database(), query, statement,
                                    "preparing solid archive insert",
                                    logSqlErrorText)) {
    return false;
  }
  bindSqliteText(statement.get(), 1,
                 chart_storage_identity::StoredPathText(update.path));
  bindSqliteText(statement.get(), 2, name);
  sqlite3_bind_int64(statement.get(), 3, archiveSize);
  sqlite3_bind_int64(statement.get(), 4,
                     clampSqlInteger(update.uncompressedSize));
  sqlite3_bind_int(statement.get(), 5, std::max(0, update.fileCount));
  sqlite3_bind_int64(statement.get(), 6, mtimeNs);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    logSqlError("inserting solid archive", impl_->database());
    return false;
  }
  impl_->noteChanged();
  return true;
}

bool ChartRepository::Session::ScanBatch::UpsertArchiveCache(
    const ArchiveScanCacheUpdate &update) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed) {
    return false;
  }
  sqlite3_int64 archiveSize = 0;
  sqlite3_int64 mtimeNs = 0;
  if (!archiveFileStateForDatabase(update.path, archiveSize, mtimeNs)) {
    return false;
  }
  const char *query =
      "INSERT INTO archive_scan_cache "
      "(path, archive_size, mtime_ns, solid, uncompressed_size, file_count, "
      "chart_count, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, "
      "CURRENT_TIMESTAMP) "
      "ON CONFLICT(path) DO UPDATE SET "
      "archive_size = excluded.archive_size,"
      "mtime_ns = excluded.mtime_ns,"
      "solid = excluded.solid,"
      "uncompressed_size = excluded.uncompressed_size,"
      "file_count = excluded.file_count,"
      "chart_count = excluded.chart_count,"
      "updated_at = CURRENT_TIMESTAMP "
      "WHERE archive_scan_cache.archive_size != excluded.archive_size "
      "OR archive_scan_cache.mtime_ns != excluded.mtime_ns "
      "OR archive_scan_cache.solid != excluded.solid "
      "OR archive_scan_cache.uncompressed_size != excluded.uncompressed_size "
      "OR archive_scan_cache.file_count != excluded.file_count "
      "OR archive_scan_cache.chart_count != excluded.chart_count";
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(impl_->database(), query, statement,
                                    "preparing archive scan cache upsert",
                                    logSqlErrorText)) {
    return false;
  }
  bindSqliteText(statement.get(), 1,
                 chart_storage_identity::StoredPathText(update.path));
  sqlite3_bind_int64(statement.get(), 2, archiveSize);
  sqlite3_bind_int64(statement.get(), 3, mtimeNs);
  sqlite3_bind_int(statement.get(), 4, update.solid ? 1 : 0);
  sqlite3_bind_int64(statement.get(), 5,
                     clampSqlInteger(update.uncompressedSize));
  sqlite3_bind_int(statement.get(), 6, std::max(0, update.fileCount));
  sqlite3_bind_int(statement.get(), 7, std::max(0, update.chartCount));
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    logSqlError("upserting archive scan cache", impl_->database());
    return false;
  }
  impl_->noteChanged();
  return true;
}

bool ChartRepository::Session::ScanBatch::UpdateSourcePreference(
    const ChartSourcePreferenceUpdate &update) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed) {
    return false;
  }
  const char *query =
      "UPDATE chart_meta SET source_priority = ?, source_archive_size = ? "
      "WHERE path = ? AND (source_priority IS NULL OR source_priority != ? "
      "OR source_archive_size IS NULL OR source_archive_size != ?)";
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(impl_->database(), query, statement,
                                    "preparing chart source preference update",
                                    logSqlErrorText)) {
    return false;
  }
  const sqlite3_int64 archiveSize = clampSqlInteger(update.archiveSize);
  sqlite3_bind_int(statement.get(), 1, update.priority);
  sqlite3_bind_int64(statement.get(), 2, archiveSize);
  bindSqliteText(statement.get(), 3,
                 chart_storage_identity::StoredPathText(update.path));
  sqlite3_bind_int(statement.get(), 4, update.priority);
  sqlite3_bind_int64(statement.get(), 5, archiveSize);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    logSqlError("updating chart source preference", impl_->database());
    return false;
  }
  impl_->noteChanged();
  return true;
}

bool ChartRepository::Session::ScanBatch::SynchronizeFolders(
    std::span<const ChartFolderScanNode> nodes,
    std::span<const std::filesystem::path> roots) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed) {
    return false;
  }

  struct StoredFolder {
    std::string storedPath;
    std::int64_t dateSeconds = 0;
    std::int64_t addDateSeconds = 0;
  };
  std::map<std::filesystem::path, StoredFolder> stored;
  SqliteStatementHandle select;
  if (!prepareSqliteStatementLogged(
          impl_->database(), "SELECT path, date, adddate FROM folder", select,
          "selecting folder scan records", logSqlErrorText)) {
    return false;
  }
  int selectResult = SQLITE_OK;
  while ((selectResult = sqlite3_step(select.get())) == SQLITE_ROW) {
    const std::string storedPath = sqliteColumnString(select.get(), 0);
    const auto path = storedPathFromDatabase(storedPath).lexically_normal();
    if (!path.empty()) {
          stored.emplace(path, StoredFolder{
                               .storedPath = storedPath,
                               .dateSeconds = sqlite3_column_int64(select.get(), 1),
                               .addDateSeconds = sqlite3_column_int64(select.get(), 2),
                           });
    }
  }
  if (selectResult != SQLITE_DONE) {
    logSqlError("selecting folder scan records", impl_->database());
    return false;
  }

  std::map<std::filesystem::path, const ChartFolderScanNode *> nodesByPath;
  std::map<std::filesystem::path, std::vector<std::filesystem::path>>
      children;
  for (const auto &node : nodes) {
    const auto path = std::filesystem::path(node.path).lexically_normal();
    if (path.empty()) {
      continue;
    }
    nodesByPath[path] = &node;
  }
  for (const auto &[path, _] : nodesByPath) {
    const auto parent = path.parent_path().lexically_normal();
    if (nodesByPath.contains(parent)) {
      children[parent].push_back(path);
    }
  }

  SqliteStatementHandle upsert;
  if (!prepareSqliteStatementLogged(
          impl_->database(),
          "INSERT INTO folder(path, date, adddate) VALUES (?, ?, ?) "
          "ON CONFLICT(path) DO UPDATE SET date = excluded.date, "
          "adddate = excluded.adddate",
          upsert, "preparing folder scan upsert", logSqlErrorText)) {
    return false;
  }
  SqliteStatementHandle erase;
  if (!prepareSqliteStatementLogged(
          impl_->database(), "DELETE FROM folder WHERE path = ?", erase,
          "preparing folder scan delete", logSqlErrorText)) {
    return false;
  }

  std::set<std::filesystem::path> deleted;
  const auto deleteSubtree = [&](const std::filesystem::path &root) {
    bool succeeded = true;
    for (const auto &[path, record] : stored) {
      if (deleted.contains(path) ||
          (path != root && !pathIsInsideDirectory(path, root))) {
        continue;
      }
      sqlite3_reset(erase.get());
      sqlite3_clear_bindings(erase.get());
      bindSqliteText(erase.get(), 1, record.storedPath);
      if (sqlite3_step(erase.get()) != SQLITE_DONE) {
        logSqlError("deleting missing folder scan record", impl_->database());
        succeeded = false;
        break;
      }
          impl_->noteFolderChanged();
      deleted.insert(path);
    }
    return succeeded;
  };

  std::function<bool(const std::filesystem::path &, bool)> process;
  process = [&](const std::filesystem::path &path, bool updateFolder) {
    const auto node = nodesByPath.find(path);
    if (node == nodesByPath.end()) {
      return true;
    }
    std::set<std::filesystem::path> presentChildren;
    for (const auto &child : children[path]) {
      presentChildren.insert(child);
    }

    if (!node->second->containsBms) {
      for (const auto &child : children[path]) {
        bool updateChild = true;
        const auto existing = stored.find(child);
        if (existing != stored.end() && !deleted.contains(child)) {
          const auto childNode = nodesByPath.find(child);
          updateChild = childNode == nodesByPath.end() ||
                        existing->second.dateSeconds !=
                            childNode->second->dateSeconds;
        }
        if (!process(child, updateChild)) {
          return false;
        }
      }
    }

    if (updateFolder) {
      const auto existing = stored.find(path);
      const bool valuesChanged =
          existing == stored.end() ||
          existing->second.dateSeconds != node->second->dateSeconds ||
          existing->second.addDateSeconds != impl_->addDateSeconds;
      sqlite3_reset(upsert.get());
      sqlite3_clear_bindings(upsert.get());
      bindSqliteText(upsert.get(), 1,
                     chart_storage_identity::StoredPathText(path));
      sqlite3_bind_int64(upsert.get(), 2, node->second->dateSeconds);
      sqlite3_bind_int64(upsert.get(), 3, impl_->addDateSeconds);
      if (sqlite3_step(upsert.get()) != SQLITE_DONE) {
        logSqlError("upserting folder scan record", impl_->database());
        return false;
      }
      impl_->noteFolderChanged(valuesChanged);
    }

    for (const auto &[candidate, _] : stored) {
      if (deleted.contains(candidate) ||
          candidate.parent_path().lexically_normal() != path ||
          presentChildren.contains(candidate)) {
        continue;
      }
      if (!deleteSubtree(candidate)) {
        return false;
      }
    }
    return true;
  };

  for (const auto &root : roots) {
    const auto normalized = root.lexically_normal();
    if (nodesByPath.contains(normalized) && !process(normalized, true)) {
      return false;
    }
  }
  return true;
}

std::optional<int> ChartRepository::Session::ScanBatch::CountChartsInArchive(
    const std::filesystem::path &path) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed) {
    return std::nullopt;
  }
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(
          impl_->database(), "SELECT path FROM chart_meta", statement,
          "counting archive chart rows", logSqlErrorText)) {
    return std::nullopt;
  }
  int count = 0;
  const auto target = path.lexically_normal();
  int stepResult = SQLITE_OK;
  while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
    const auto chartPath =
        storedPathFromDatabase(sqliteColumnString(statement.get(), 0));
    if (pathIsInsideDirectory(chartPath, target)) {
      ++count;
    }
  }
  if (stepResult != SQLITE_DONE) {
    logSqlError("counting archive chart rows", impl_->database());
    return std::nullopt;
  }
  return count;
}

bool ChartRepository::Session::ScanBatch::CheckpointAndContinue(
    const ChartScanCheckpoint &checkpoint) {
  if (impl_ == nullptr || !impl_->ready || impl_->committed ||
      !impl_->commitTransaction()) {
    return false;
  }
  const bool checkpointSaved =
      upsertScanCheckpoint(impl_->database(), checkpoint);
  const bool transactionStarted = impl_->beginTransaction();
  impl_->ready = transactionStarted;
  return checkpointSaved && transactionStarted;
}

bool ChartRepository::Session::ScanBatch::Commit() {
  if (impl_ == nullptr || !impl_->ready || impl_->committed ||
      !impl_->commitTransaction()) {
    return false;
  }
  impl_->committed = true;
  impl_->ready = false;
  if (impl_->changedCount > 0 || impl_->folderChanged) {
    chart_repository_detail::BumpLibraryRevision();
  }
  return true;
}

int ChartRepository::Session::ScanBatch::ChangedCount() const {
  return impl_ != nullptr ? impl_->changedCount : 0;
}

ChartScanSnapshot ChartRepository::Session::LoadScanSnapshot(
    ChartScanSnapshotLoad load) {
  return loadScanSnapshot(impl_->database(), load);
}

std::optional<ChartRepository::Session::ScanBatch>
ChartRepository::Session::BeginScanBatch() {
  auto batchImpl = std::make_unique<ScanBatch::Impl>(impl_->storage);
  if (!batchImpl->ready) {
    return std::nullopt;
  }
  return ScanBatch(std::move(batchImpl));
}

bool ChartRepository::Session::ClearScanCheckpoint() {
  return clearScanCheckpoint(impl_->database());
}

bool ChartRepository::Session::ClearChartMetadataRebuildRequired() {
  return clearMetadataRebuildRequired(impl_->database());
}
