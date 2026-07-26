#include "ReplayRepository.h"
#include "ReplayRepositoryInternal.h"
#include "ReplayRepositoryReplayFileMigration.h"

#include "../ProfileDatabaseActivity.h"
#include "../Utils.h"
#include "../path.h"
#include "../replay/BeatorajaReplayCodec.h"
#include "../replay/ReplayFileStore.h"
#include "SqliteRAII.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr int kReplayDatabaseSchemaVersion =
    ReplayRepository::kCurrentSchemaVersion;

void logSqlErrorText(const char *context, const std::string &error) {
  SDL_Log("SQL error while %s: %s", context, error.c_str());
}

bool execSql(sqlite3 *database, const char *query, const char *context) {
  return executeSqliteLogged(database, query, context, logSqlErrorText);
}

bool setDatabaseUserVersion(sqlite3 *database, int version) {
  const std::string query =
      "PRAGMA user_version = " + std::to_string(std::max(0, version));
  return execSql(database, query.c_str(), "updating replay database version");
}

bool rejectFutureReplayDatabase(sqlite3 *database) {
  std::string error;
  const auto version = readSqliteUserVersion(database, error);
  if (!version.has_value()) {
    SDL_Log("Refusing replay database with unreadable version: %s",
            error.c_str());
    return true;
  }
  return *version > kReplayDatabaseSchemaVersion;
}

bool databaseHasApplicationTables(sqlite3 *database, bool &hasTables) {
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(
          database,
          "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
          "AND name NOT LIKE 'sqlite_%'",
          statement, "inspecting replay database tables", logSqlErrorText) ||
      sqlite3_step(statement.get()) != SQLITE_ROW ||
      sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
    return false;
  }
  hasTables = sqlite3_column_int64(statement.get(), 0) != 0;
  return sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool namedSchemaObjectExists(sqlite3 *database, std::string_view type,
                             std::string_view name) {
  SqliteStatementHandle statement;
  return prepareSqliteStatementLogged(
             database,
             "SELECT 1 FROM sqlite_master WHERE type=? AND name=?",
             statement, "inspecting compact replay schema", logSqlErrorText) &&
         bindSqliteText(statement.get(), 1, std::string(type)) &&
         bindSqliteText(statement.get(), 2, std::string(name)) &&
         sqlite3_step(statement.get()) == SQLITE_ROW;
}

bool validateCompactReplaySchema11(sqlite3 *database) {
  constexpr std::array<std::string_view, 11> requiredTables{
      "chart_results",
      "course_results",
      "course_result_stages",
      "replay_files",
      "replay_file_reservations",
      "replay_stem_sequences",
      "ir_submission_snapshots",
      "pending_chart_score_writes",
      "ir_outbox",
      "ir_submission_receipts",
      "ir_remote_scores",
  };
  if (!std::ranges::all_of(requiredTables, [&](std::string_view table) {
        return namedSchemaObjectExists(database, "table", table);
      })) {
    SDL_Log("Refusing version 11 replay database with incomplete compact "
            "schema");
    return false;
  }

  if (!replay_repository_detail::
          compactReplaySchemaHasNoLegacyPayloadTables(database)) {
    SDL_Log("Refusing version 11 replay database with legacy payload tables");
    return false;
  }

  constexpr std::array<std::string_view, 15> requiredIndexes{
      "idx_chart_results_sha256_played",
      "idx_chart_results_md5_played",
      "idx_course_results_key_played",
      "idx_course_result_stages_sha256",
      "idx_replay_files_chart_result",
      "idx_replay_files_course_result",
      "idx_replay_reservations_stem_index",
      "idx_ir_submission_snapshots_fingerprint",
      "idx_pending_chart_score_created",
      "idx_ir_outbox_due",
      "idx_ir_outbox_attempt",
      "idx_ir_submission_receipts_attempt",
      "idx_ir_submission_receipts_remote_score",
      "idx_ir_remote_scores_chart_sha256",
      "idx_ir_remote_scores_remote_chart_id",
  };
  if (!std::ranges::all_of(requiredIndexes, [&](std::string_view index) {
        return namedSchemaObjectExists(database, "index", index);
      })) {
    SDL_Log("Refusing version 11 replay database with incomplete indexes");
    return false;
  }
  return true;
}

constexpr const char *kIrOutboxTableSql =
    "CREATE TABLE ir_outbox("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,provider_id TEXT NOT NULL,"
    "attempt_id TEXT NOT NULL,chart_md5 TEXT,chart_sha256 TEXT NOT NULL,"
    "payload_json TEXT NOT NULL,ruleset_id TEXT NOT NULL,"
    "ruleset_revision INTEGER NOT NULL,validation_fingerprint TEXT NOT NULL,"
    "state INTEGER NOT NULL,local_result_ready INTEGER NOT NULL DEFAULT 0,"
    "request_attempt_count INTEGER NOT NULL DEFAULT 0,"
    "consecutive_failure_count INTEGER NOT NULL DEFAULT 0,"
    "remote_poll_count INTEGER NOT NULL DEFAULT 0,next_attempt_at_ms INTEGER,"
    "next_request_user_intent INTEGER NOT NULL DEFAULT 0,remote_job_id TEXT,"
    "remote_origin TEXT,last_error_code TEXT,last_error_message TEXT,"
    "created_at_ms INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL,"
    "completed_at_ms INTEGER,UNIQUE(provider_id,attempt_id),"
    "CHECK(local_result_ready IN (0,1)),"
    "CHECK(next_request_user_intent IN (0,1)),"
    "CHECK((remote_job_id IS NULL AND remote_origin IS NULL) OR "
    "(remote_job_id IS NOT NULL AND remote_origin IS NOT NULL)))";

constexpr const char *kIrRemoteScoresTableSql =
    "CREATE TABLE ir_remote_scores("
    "provider_id TEXT NOT NULL,server_origin TEXT NOT NULL,"
    "remote_score_id TEXT NOT NULL,remote_user_id INTEGER NOT NULL,"
    "game TEXT NOT NULL,remote_chart_id TEXT NOT NULL,"
    "chart_md5 TEXT NOT NULL,chart_sha256 TEXT NOT NULL,title TEXT NOT NULL,"
    "artist TEXT NOT NULL,difficulty TEXT,level TEXT,level_number REAL,"
    "note_count INTEGER NOT NULL,score INTEGER NOT NULL,lamp_rank INTEGER "
    "NOT NULL,service TEXT NOT NULL,time_achieved_ms INTEGER,"
    "time_added_ms INTEGER NOT NULL,pgreat INTEGER,great INTEGER,good INTEGER,"
    "bad INTEGER,poor INTEGER,early_pgreat INTEGER,late_pgreat INTEGER,"
    "early_great INTEGER,late_great INTEGER,early_good INTEGER,"
    "late_good INTEGER,early_bad INTEGER,late_bad INTEGER,early_poor INTEGER,"
    "late_poor INTEGER,fast INTEGER,slow INTEGER,max_combo INTEGER,"
    "bad_points INTEGER,final_gauge REAL,gauge_history_json TEXT,"
    "random_mode TEXT,gauge_mode TEXT,input_device TEXT,client TEXT,"
    "sync_generation INTEGER NOT NULL,"
    "PRIMARY KEY(provider_id,server_origin,remote_score_id),"
    "CHECK(game IN ('bms-7k','bms-14k')),CHECK(remote_user_id>0),"
    "CHECK(note_count>=0),CHECK(score>=0),CHECK(sync_generation>0))";

bool createCompactReplaySchema11(sqlite3 *database) {
  const char *tables[] = {
      "CREATE TABLE chart_results("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,attempt_id TEXT UNIQUE,"
      "chart_path TEXT NOT NULL,chart_md5 TEXT NOT NULL,"
      "chart_sha256 TEXT NOT NULL,chart_title TEXT NOT NULL,"
      "chart_artist TEXT NOT NULL,key_mode INTEGER NOT NULL,"
      "long_note_mode INTEGER NOT NULL,score INTEGER NOT NULL,"
      "max_score INTEGER NOT NULL,max_combo INTEGER NOT NULL,"
      "combo_break INTEGER NOT NULL,p_great INTEGER NOT NULL,"
      "great INTEGER NOT NULL,good INTEGER NOT NULL,bad INTEGER NOT NULL,"
      "poor INTEGER NOT NULL,k_poor INTEGER NOT NULL,fast INTEGER NOT NULL,"
      "slow INTEGER NOT NULL,final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL,gauge_history_json TEXT NOT NULL,"
      "judgement_timing_json TEXT,provenance_json TEXT NOT NULL,"
      "result_fingerprint TEXT NOT NULL,played_at_unix_ms INTEGER NOT NULL,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)",
      "CREATE TABLE course_results("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,attempt_id TEXT UNIQUE,"
      "course_key TEXT NOT NULL,legacy_course_id INTEGER NOT NULL,"
      "course_name TEXT NOT NULL,course_group_name TEXT NOT NULL,"
      "constraint_json TEXT NOT NULL,completed_charts INTEGER NOT NULL,"
      "total_charts INTEGER NOT NULL,requested_play_option TEXT NOT NULL,"
      "assist_option TEXT NOT NULL,initial_gauge_type INTEGER NOT NULL,"
      "gauge_profile INTEGER NOT NULL,gauge_auto_shift INTEGER NOT NULL,"
      "gauge_auto_shift_lower_bound INTEGER NOT NULL,"
      "long_note_mode INTEGER NOT NULL,final_score INTEGER NOT NULL,"
      "max_score INTEGER NOT NULL,max_combo INTEGER NOT NULL,"
      "final_gauge REAL NOT NULL,clear_type INTEGER NOT NULL,"
      "provenance_json TEXT NOT NULL,result_fingerprint TEXT NOT NULL,"
      "played_at_unix_ms INTEGER NOT NULL,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)",
      "CREATE TABLE course_result_stages("
      "course_result_id INTEGER NOT NULL,stage_index INTEGER NOT NULL,"
      "chart_path TEXT NOT NULL,chart_md5 TEXT NOT NULL,"
      "chart_sha256 TEXT NOT NULL,chart_title TEXT NOT NULL,"
      "chart_artist TEXT NOT NULL,key_mode INTEGER NOT NULL,"
      "long_note_mode INTEGER NOT NULL,score INTEGER NOT NULL,"
      "max_score INTEGER NOT NULL,max_combo INTEGER NOT NULL,"
      "combo_break INTEGER NOT NULL,p_great INTEGER NOT NULL,"
      "great INTEGER NOT NULL,good INTEGER NOT NULL,bad INTEGER NOT NULL,"
      "poor INTEGER NOT NULL,k_poor INTEGER NOT NULL,fast INTEGER NOT NULL,"
      "slow INTEGER NOT NULL,final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL,gauge_history_json TEXT NOT NULL,"
      "judgement_timing_json TEXT,provenance_json TEXT NOT NULL,"
      "PRIMARY KEY(course_result_id,stage_index),"
      "FOREIGN KEY(course_result_id) REFERENCES course_results(id) "
      "ON DELETE CASCADE)",
      "CREATE TABLE replay_files("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,chart_result_id INTEGER UNIQUE,"
      "course_result_id INTEGER UNIQUE,stem TEXT NOT NULL,"
      "history_index INTEGER NOT NULL,relative_path TEXT UNIQUE NOT NULL,"
      "content_sha256 TEXT NOT NULL,compressed_size INTEGER NOT NULL,"
      "codec_version INTEGER NOT NULL,"
      "CHECK((chart_result_id IS NOT NULL)!=(course_result_id IS NOT NULL)),"
      "CHECK(history_index>=0),CHECK(length(content_sha256)=64),"
      "CHECK(compressed_size>0),CHECK(codec_version=2),"
      "UNIQUE(stem,history_index),"
      "FOREIGN KEY(chart_result_id) REFERENCES chart_results(id) "
      "ON DELETE CASCADE,"
      "FOREIGN KEY(course_result_id) REFERENCES course_results(id) "
      "ON DELETE CASCADE)",
      "CREATE TABLE replay_file_reservations("
      "attempt_id TEXT PRIMARY KEY,stem TEXT NOT NULL,"
      "history_index INTEGER NOT NULL,relative_path TEXT UNIQUE NOT NULL,"
      "created_at_unix_ms INTEGER NOT NULL,CHECK(history_index>=0),"
      "UNIQUE(stem,history_index))",
      "CREATE TABLE replay_stem_sequences("
      "stem TEXT PRIMARY KEY,last_history_index INTEGER NOT NULL,"
      "CHECK(last_history_index>=0))",
      "CREATE TABLE ir_submission_snapshots("
      "attempt_id TEXT PRIMARY KEY,schema_version INTEGER NOT NULL,"
      "payload_json TEXT NOT NULL,fingerprint TEXT NOT NULL,"
      "FOREIGN KEY(attempt_id) REFERENCES chart_results(attempt_id) "
      "ON DELETE CASCADE)",
      "CREATE TABLE pending_chart_score_writes("
      "attempt_id TEXT PRIMARY KEY NOT NULL,result_id INTEGER NOT NULL UNIQUE,"
      "chart_path TEXT NOT NULL,chart_md5 TEXT NOT NULL,"
      "chart_sha256 TEXT NOT NULL,chart_title TEXT NOT NULL,"
      "chart_artist TEXT NOT NULL,ln_mode INTEGER NOT NULL,"
      "score INTEGER NOT NULL,max_score INTEGER NOT NULL,"
      "max_combo INTEGER NOT NULL,combo_break INTEGER NOT NULL,"
      "pgreat INTEGER NOT NULL,great INTEGER NOT NULL,good INTEGER NOT NULL,"
      "bad INTEGER NOT NULL,poor INTEGER NOT NULL,kpoor INTEGER NOT NULL,"
      "fast INTEGER NOT NULL,slow INTEGER NOT NULL,final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL,ruleset_version INTEGER NOT NULL,"
      "eligibility INTEGER NOT NULL,provenance_json TEXT NOT NULL,"
      "created_at TEXT NOT NULL,recovery_attempts INTEGER NOT NULL DEFAULT 0,"
      "last_recovery_at TEXT,FOREIGN KEY(result_id) REFERENCES "
      "chart_results(id) ON DELETE CASCADE)",
      kIrOutboxTableSql,
      "CREATE TABLE ir_submission_receipts("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,provider_id TEXT NOT NULL,"
      "server_origin TEXT NOT NULL,result_id INTEGER NOT NULL,"
      "attempt_id TEXT NOT NULL,chart_md5 TEXT,chart_sha256 TEXT NOT NULL,"
      "remote_user_id INTEGER,remote_chart_id TEXT,remote_score_id TEXT,"
      "confirmation_source INTEGER NOT NULL,"
      "observed_in_snapshot INTEGER NOT NULL DEFAULT 0,"
      "confirmed_at_ms INTEGER NOT NULL,"
      "UNIQUE(provider_id,server_origin,result_id),"
      "CHECK(observed_in_snapshot IN (0,1)),"
      "FOREIGN KEY(result_id) REFERENCES chart_results(id) ON DELETE CASCADE)",
      kIrRemoteScoresTableSql,
  };
  for (const char *table : tables) {
    if (!execSql(database, table, "creating compact replay table")) {
      return false;
    }
  }

  const char *indexes[] = {
      "CREATE INDEX idx_chart_results_sha256_played ON "
      "chart_results(chart_sha256,played_at_unix_ms DESC,id DESC)",
      "CREATE INDEX idx_chart_results_md5_played ON "
      "chart_results(chart_md5,played_at_unix_ms DESC,id DESC)",
      "CREATE INDEX idx_course_results_key_played ON "
      "course_results(course_key,played_at_unix_ms DESC,id DESC)",
      "CREATE INDEX idx_course_result_stages_sha256 ON "
      "course_result_stages(chart_sha256,course_result_id,stage_index)",
      "CREATE INDEX idx_replay_files_chart_result ON "
      "replay_files(chart_result_id)",
      "CREATE INDEX idx_replay_files_course_result ON "
      "replay_files(course_result_id)",
      "CREATE INDEX idx_replay_reservations_stem_index ON "
      "replay_file_reservations(stem,history_index)",
      "CREATE INDEX idx_ir_submission_snapshots_fingerprint ON "
      "ir_submission_snapshots(fingerprint)",
      "CREATE INDEX idx_pending_chart_score_created ON "
      "pending_chart_score_writes(recovery_attempts,last_recovery_at,"
      "created_at,attempt_id)",
      "CREATE INDEX idx_ir_outbox_due ON "
      "ir_outbox(local_result_ready,state,next_attempt_at_ms,id)",
      "CREATE INDEX idx_ir_outbox_attempt ON "
      "ir_outbox(provider_id,attempt_id)",
      "CREATE INDEX idx_ir_submission_receipts_attempt ON "
      "ir_submission_receipts(provider_id,server_origin,attempt_id)",
      "CREATE INDEX idx_ir_submission_receipts_remote_score ON "
      "ir_submission_receipts(provider_id,server_origin,remote_score_id)",
      "CREATE INDEX idx_ir_remote_scores_chart_sha256 ON "
      "ir_remote_scores(provider_id,server_origin,chart_sha256)",
      "CREATE INDEX idx_ir_remote_scores_remote_chart_id ON "
      "ir_remote_scores(provider_id,server_origin,remote_chart_id)",
  };
  for (const char *index : indexes) {
    if (!execSql(database, index, "creating compact replay index")) {
      return false;
    }
  }
  return setDatabaseUserVersion(database, kReplayDatabaseSchemaVersion);
}

bool migrateReplayDatabaseSchema(
    sqlite3 *database,
    const std::filesystem::path &chartDatabasePath = {}) {
  std::string versionError;
  const auto version = readSqliteUserVersion(database, versionError);
  if (!version.has_value()) {
    logSqlErrorText("reading replay migration version", versionError);
    return false;
  }
  if (*version == kReplayDatabaseSchemaVersion) {
    return validateCompactReplaySchema11(database);
  }
  if (*version != 10) {
    SDL_Log("Replay database schema %d is not supported", *version);
    return false;
  }

  const char *filename = sqlite3_db_filename(database, "main");
  if (filename == nullptr || *filename == '\0') {
    SDL_Log("Replay database migration cannot resolve the profile root");
    return false;
  }
  const std::filesystem::path profileRoot =
      std::filesystem::path(filename).parent_path();
  replay::BeatorajaReplayCodec codec;
  replay::ReplayFileStore fileStore(profileRoot);
  const auto resolveKeyMode = replay_repository_detail::
      makeChartDatabaseReplayKeyModeResolver(chartDatabasePath);
  const auto outcome = replay_repository_detail::migrateReplaySchema10To11(
      database, profileRoot, codec, fileStore, {}, resolveKeyMode);
  if (outcome.status !=
          replay_repository_detail::ReplayMigrationOutcome::Status::Migrated &&
      outcome.status != replay_repository_detail::ReplayMigrationOutcome::
                            Status::AlreadyCurrent) {
    SDL_Log("Replay database migration failed: %s",
            outcome.diagnostic.c_str());
    return false;
  }
  return validateCompactReplaySchema11(database);
}

std::filesystem::path resolvedReplayDatabasePath(
    const std::filesystem::path &databasePath) {
  return databasePath.empty() ? Utils::GetDocumentsPath("db") / "replay.db"
                              : databasePath;
}

std::filesystem::path normalizedReplayDatabasePath(
    const std::filesystem::path &databasePath) {
  std::filesystem::path resolved = resolvedReplayDatabasePath(databasePath);
  std::error_code error;
  const std::filesystem::path absolute =
      std::filesystem::absolute(resolved, error);
  if (!error) {
    resolved = absolute;
  }
  error.clear();
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(resolved, error);
  return (error ? resolved : canonical).lexically_normal();
}

bool equivalentReplayDatabasePaths(const std::filesystem::path &first,
                                   const std::filesystem::path &second) {
  const std::filesystem::path firstResolved =
      resolvedReplayDatabasePath(first);
  const std::filesystem::path secondResolved =
      resolvedReplayDatabasePath(second);
  std::error_code firstExistsError;
  std::error_code secondExistsError;
  const bool firstExists =
      std::filesystem::exists(firstResolved, firstExistsError);
  const bool secondExists =
      std::filesystem::exists(secondResolved, secondExistsError);
  if (!firstExistsError && !secondExistsError && firstExists && secondExists) {
    std::error_code equivalentError;
    const bool equivalent = std::filesystem::equivalent(
        firstResolved, secondResolved, equivalentError);
    if (!equivalentError) {
      return equivalent;
    }
  }
  return normalizedReplayDatabasePath(first) ==
         normalizedReplayDatabasePath(second);
}

sqlite3 *openReplayDatabase(const std::filesystem::path &path,
                            std::string &errorMessage) {
  const std::filesystem::path directory = path.parent_path();
  std::error_code directoryError;
  if (!directory.empty() &&
      !Utils::EnsureDirectoryExists(directory, directoryError)) {
    errorMessage = "can't create replay database directory " +
                   fspath_to_utf8(directory) + ": " +
                   directoryError.message();
    return nullptr;
  }
  return openValidatedSqliteDatabase(path, kReplayDatabaseSchemaVersion, true,
                                     errorMessage);
}

} // namespace

bool replay_repository_detail::CreateCompactReplaySchema11OnConnection(
    sqlite3 *database) {
  return database != nullptr && createCompactReplaySchema11(database);
}

bool replay_repository_detail::CreateReplayTablesOnConnection(
    sqlite3 *database, const std::filesystem::path &chartDatabasePath) {
  if (database == nullptr || rejectFutureReplayDatabase(database)) {
    return false;
  }
  std::string versionError;
  const auto version = readSqliteUserVersion(database, versionError);
  if (!version.has_value()) {
    logSqlErrorText("reading replay schema version", versionError);
    return false;
  }
  if (*version != 0) {
    return migrateReplayDatabaseSchema(database, chartDatabasePath);
  }

  bool hasTables = false;
  if (!databaseHasApplicationTables(database, hasTables) || hasTables) {
    if (hasTables) {
      SDL_Log("Refusing unversioned replay database with existing tables");
    }
    return false;
  }

  const bool callerOwnsTransaction = sqlite3_get_autocommit(database) == 0;
  const char *beginQuery = callerOwnsTransaction
                               ? "SAVEPOINT asobmashow_replay_schema_ensure"
                               : "BEGIN IMMEDIATE TRANSACTION";
  const char *commitQuery = callerOwnsTransaction
                                ? "RELEASE asobmashow_replay_schema_ensure"
                                : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_replay_schema_ensure; RELEASE "
            "asobmashow_replay_schema_ensure"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(database, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    logSqlErrorText("starting replay schema ensure", transactionError);
    return false;
  }
  if (!createCompactReplaySchema11(database)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing compact replay schema", transactionError);
    return false;
  }
  return true;
}

sqlite3 *replay_repository_detail::OpenDatabase(
    const std::filesystem::path &path, std::string &errorMessage) {
  return openReplayDatabase(path, errorMessage);
}

std::filesystem::path replay_repository_detail::ResolvedDatabasePath(
    const std::filesystem::path &databasePath) {
  return resolvedReplayDatabasePath(databasePath);
}

bool replay_repository_detail::EquivalentDatabasePaths(
    const std::filesystem::path &first,
    const std::filesystem::path &second) {
  return equivalentReplayDatabasePaths(first, second);
}

bool replay_repository_detail::MigrateSchema(
    sqlite3 *database,
    const std::filesystem::path &chartDatabasePath) {
  return migrateReplayDatabaseSchema(database, chartDatabasePath);
}
