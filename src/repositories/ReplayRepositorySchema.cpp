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
#include <cctype>
#include <compare>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

bool namedTableColumnExists(sqlite3 *database, std::string_view table,
                            std::string_view column) {
  SqliteStatementHandle statement;
  const std::string query =
      "SELECT 1 FROM pragma_table_info('" + std::string(table) +
      "') WHERE name=?";
  return prepareSqliteStatementLogged(
             database, query.c_str(), statement,
             "inspecting compact replay columns", logSqlErrorText) &&
         bindSqliteText(statement.get(), 1, std::string(column)) &&
         sqlite3_step(statement.get()) == SQLITE_ROW;
}

bool createCompactReplaySchema11(sqlite3 *database);

std::string normalizeColumnDefault(std::string_view value) {
  const auto trim = [](std::string_view source) {
    while (!source.empty() &&
           std::isspace(static_cast<unsigned char>(source.front())) != 0) {
      source.remove_prefix(1);
    }
    while (!source.empty() &&
           std::isspace(static_cast<unsigned char>(source.back())) != 0) {
      source.remove_suffix(1);
    }
    return source;
  };
  value = trim(value);
  while (value.size() >= 2 && value.front() == '(' && value.back() == ')') {
    int depth = 0;
    char quote = 0;
    bool wrapsWholeExpression = true;
    for (std::size_t index = 0; index < value.size(); ++index) {
      const char character = value[index];
      if (quote != 0) {
        if (character == quote) {
          if (index + 1 < value.size() && value[index + 1] == quote) {
            ++index;
          } else {
            quote = 0;
          }
        }
        continue;
      }
      if (character == '\'' || character == '"') {
        quote = character;
      } else if (character == '(') {
        ++depth;
      } else if (character == ')') {
        --depth;
        if (depth == 0 && index + 1 != value.size()) {
          wrapsWholeExpression = false;
          break;
        }
        if (depth < 0) {
          wrapsWholeExpression = false;
          break;
        }
      }
    }
    if (!wrapsWholeExpression || depth != 0 || quote != 0) {
      break;
    }
    value = trim(value.substr(1, value.size() - 2));
  }

  std::string normalized;
  normalized.reserve(value.size());
  char quote = 0;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(value[index]);
    if (quote != 0) {
      normalized.push_back(static_cast<char>(character));
      if (character == static_cast<unsigned char>(quote)) {
        if (index + 1 < value.size() && value[index + 1] == quote) {
          normalized.push_back(value[++index]);
        } else {
          quote = 0;
        }
      }
      continue;
    }
    if (character == '\'' || character == '"') {
      quote = static_cast<char>(character);
      normalized.push_back(static_cast<char>(character));
    } else if (std::isspace(character) == 0) {
      normalized.push_back(character >= 'A' && character <= 'Z'
                               ? static_cast<char>(character + ('a' - 'A'))
                               : static_cast<char>(character));
    }
  }
  return normalized;
}

struct ColumnShape {
  std::string name;
  std::string type;
  bool notNull = false;
  std::optional<std::string> defaultExpression;
  int primaryKeyOrder = 0;

  bool operator==(const ColumnShape &) const = default;
};

bool columnShapesEquivalent(const ColumnShape &actual,
                            const ColumnShape &expected) {
  if (actual.name != expected.name || actual.type != expected.type ||
      actual.notNull != expected.notNull ||
      actual.primaryKeyOrder != expected.primaryKeyOrder) {
    return false;
  }
  if (actual.defaultExpression == expected.defaultExpression) {
    return true;
  }
  // SQLite cannot drop defaults from columns introduced with ALTER TABLE.
  // Accept only the exact backfill defaults used by the v12/v14 migrations;
  // fresh schemas declare these already-populated columns without defaults.
  return !expected.defaultExpression.has_value() &&
         ((actual.name == "adopted_gauge_type" &&
           actual.defaultExpression == std::optional<std::string>("2")) ||
          (actual.name == "entry_facts_json" &&
           actual.defaultExpression == std::optional<std::string>("'[]'")));
}

std::optional<std::vector<ColumnShape>>
tableColumnShapes(sqlite3 *database, std::string_view table) {
  SqliteStatementHandle statement;
  const std::string query =
      "SELECT name,type,\"notnull\",dflt_value,pk FROM pragma_table_info('" +
      std::string(table) + "') ORDER BY name";
  if (prepareSqliteStatement(database, query.c_str(), statement) != SQLITE_OK) {
    return std::nullopt;
  }
  std::vector<ColumnShape> result;
  int step = SQLITE_OK;
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT ||
        sqlite3_column_type(statement.get(), 1) != SQLITE_TEXT ||
        sqlite3_column_type(statement.get(), 2) != SQLITE_INTEGER ||
        (sqlite3_column_type(statement.get(), 3) != SQLITE_TEXT &&
         sqlite3_column_type(statement.get(), 3) != SQLITE_NULL) ||
        sqlite3_column_type(statement.get(), 4) != SQLITE_INTEGER) {
      return std::nullopt;
    }
    std::optional<std::string> defaultExpression;
    if (sqlite3_column_type(statement.get(), 3) == SQLITE_TEXT) {
      defaultExpression =
          normalizeColumnDefault(sqliteColumnString(statement.get(), 3));
    }
    result.push_back(
        {.name = sqliteColumnString(statement.get(), 0),
         .type = sqliteColumnString(statement.get(), 1),
         .notNull = sqlite3_column_int(statement.get(), 2) != 0,
         .defaultExpression = std::move(defaultExpression),
         .primaryKeyOrder = sqlite3_column_int(statement.get(), 4)});
  }
  return step == SQLITE_DONE
             ? std::optional<std::vector<ColumnShape>>(std::move(result))
             : std::nullopt;
}

bool validateCompactReplayColumnShapes(sqlite3 *database, int version) {
  sqlite3 *templateDatabase = nullptr;
  if (sqlite3_open(":memory:", &templateDatabase) != SQLITE_OK ||
      templateDatabase == nullptr) {
    if (templateDatabase != nullptr) {
      sqlite3_close(templateDatabase);
    }
    return false;
  }
  const bool templateCreated = createCompactReplaySchema11(templateDatabase);
  constexpr std::array<std::string_view, 11> tables{
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
  bool valid = templateCreated;
  for (std::string_view table : tables) {
    auto actual = tableColumnShapes(database, table);
    auto expected = tableColumnShapes(templateDatabase, table);
    if (!actual || !expected) {
      valid = false;
      break;
    }
    std::erase_if(*expected, [&](const ColumnShape &column) {
      return (version < 12 && column.name == "adopted_gauge_type") ||
             (version < 13 &&
              (column.name == "finalized_content_sha256" ||
               column.name == "finalized_compressed_size")) ||
             (version < 14 && column.name == "entry_facts_json");
    });
    if (actual->size() != expected->size() ||
        !std::ranges::equal(*actual, *expected, columnShapesEquivalent)) {
      valid = false;
      break;
    }
  }
  sqlite3_close(templateDatabase);
  if (!valid) {
    SDL_Log("Refusing replay database with incompatible table columns");
  }
  return valid;
}

std::optional<std::string> schemaObjectSql(sqlite3 *database,
                                           std::string_view type,
                                           std::string_view name) {
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(
          database, "SELECT sql FROM sqlite_master WHERE type=? AND name=?",
          statement, "inspecting compact replay schema definitions",
          logSqlErrorText) ||
      !bindSqliteText(statement.get(), 1, std::string(type)) ||
      !bindSqliteText(statement.get(), 2, std::string(name)) ||
      sqlite3_step(statement.get()) != SQLITE_ROW ||
      sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT) {
    return std::nullopt;
  }
  const std::string sql = sqliteColumnString(statement.get(), 0);
  return sqlite3_step(statement.get()) == SQLITE_DONE
             ? std::optional<std::string>(sql)
             : std::nullopt;
}

std::string sqliteIdentifierFold(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    result.push_back(character >= 'A' && character <= 'Z'
                         ? static_cast<char>(character + ('a' - 'A'))
                         : static_cast<char>(character));
  }
  return result;
}

struct IndexColumnShape {
  int sequence = 0;
  std::optional<std::string> name;
  bool descending = false;
  std::string collation;
  bool key = false;

  auto operator<=>(const IndexColumnShape &) const = default;
};

struct IndexShape {
  std::string table;
  bool unique = false;
  std::string origin;
  bool partial = false;
  std::vector<IndexColumnShape> columns;

  auto operator<=>(const IndexShape &) const = default;
};

std::optional<IndexShape> indexShape(sqlite3 *database, std::string_view table,
                                     std::string_view indexName) {
  SqliteStatementHandle index;
  if (prepareSqliteStatement(
          database,
          "SELECT \"unique\",origin,partial FROM pragma_index_list(?) "
          "WHERE name=?",
          index) != SQLITE_OK ||
      !bindSqliteText(index.get(), 1, std::string(table)) ||
      !bindSqliteText(index.get(), 2, std::string(indexName)) ||
      sqlite3_step(index.get()) != SQLITE_ROW ||
      sqlite3_column_type(index.get(), 0) != SQLITE_INTEGER ||
      sqlite3_column_type(index.get(), 1) != SQLITE_TEXT ||
      sqlite3_column_type(index.get(), 2) != SQLITE_INTEGER) {
    return std::nullopt;
  }
  IndexShape result{
      .table = sqliteIdentifierFold(table),
      .unique = sqlite3_column_int(index.get(), 0) != 0,
      .origin = sqliteIdentifierFold(sqliteColumnString(index.get(), 1)),
      .partial = sqlite3_column_int(index.get(), 2) != 0,
  };
  if (sqlite3_step(index.get()) != SQLITE_DONE) {
    return std::nullopt;
  }

  SqliteStatementHandle columns;
  if (prepareSqliteStatement(database,
                             "SELECT seqno,cid,name,\"desc\",coll,\"key\" FROM "
                             "pragma_index_xinfo(?) ORDER BY seqno",
                             columns) != SQLITE_OK ||
      !bindSqliteText(columns.get(), 1, std::string(indexName))) {
    return std::nullopt;
  }
  int step = SQLITE_OK;
  while ((step = sqlite3_step(columns.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(columns.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_type(columns.get(), 1) != SQLITE_INTEGER ||
        (sqlite3_column_type(columns.get(), 2) != SQLITE_TEXT &&
         sqlite3_column_type(columns.get(), 2) != SQLITE_NULL) ||
        sqlite3_column_type(columns.get(), 3) != SQLITE_INTEGER ||
        sqlite3_column_type(columns.get(), 4) != SQLITE_TEXT ||
        sqlite3_column_type(columns.get(), 5) != SQLITE_INTEGER) {
      return std::nullopt;
    }
    std::optional<std::string> name;
    if (sqlite3_column_type(columns.get(), 2) == SQLITE_TEXT) {
      name = sqliteIdentifierFold(sqliteColumnString(columns.get(), 2));
    }
    result.columns.push_back(
        {.sequence = sqlite3_column_int(columns.get(), 0),
         .name = std::move(name),
         .descending = sqlite3_column_int(columns.get(), 3) != 0,
         .collation =
             sqliteIdentifierFold(sqliteColumnString(columns.get(), 4)),
         .key = sqlite3_column_int(columns.get(), 5) != 0});
  }
  if (step != SQLITE_DONE || result.columns.empty()) {
    return std::nullopt;
  }
  return result;
}

std::optional<IndexShape> namedIndexShape(sqlite3 *database,
                                          std::string_view indexName) {
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          database,
          "SELECT tbl_name FROM sqlite_master WHERE type='index' AND name=?",
          statement) != SQLITE_OK ||
      !bindSqliteText(statement.get(), 1, std::string(indexName)) ||
      sqlite3_step(statement.get()) != SQLITE_ROW ||
      sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT) {
    return std::nullopt;
  }
  const std::string table = sqliteColumnString(statement.get(), 0);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    return std::nullopt;
  }
  return indexShape(database, table, indexName);
}

std::optional<std::vector<IndexShape>> uniqueKeyShapes(sqlite3 *database,
                                                       std::string_view table) {
  SqliteStatementHandle indexes;
  if (prepareSqliteStatement(
          database,
          "SELECT name FROM pragma_index_list(?) WHERE \"unique\"=1 "
          "ORDER BY name",
          indexes) != SQLITE_OK ||
      !bindSqliteText(indexes.get(), 1, std::string(table))) {
    return std::nullopt;
  }
  std::vector<IndexShape> result;
  int step = SQLITE_OK;
  while ((step = sqlite3_step(indexes.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(indexes.get(), 0) != SQLITE_TEXT) {
      return std::nullopt;
    }
    const std::string indexName = sqliteColumnString(indexes.get(), 0);
    auto shape = indexShape(database, table, indexName);
    if (!shape.has_value()) {
      return std::nullopt;
    }
    result.push_back(std::move(*shape));
  }
  if (step != SQLITE_DONE) {
    return std::nullopt;
  }
  std::ranges::sort(result);
  return result;
}

std::optional<std::vector<std::string>>
foreignKeyShapes(sqlite3 *database, std::string_view table) {
  SqliteStatementHandle statement;
  const std::string query =
      "SELECT id,seq,\"table\",\"from\",\"to\",on_update,on_delete,match "
      "FROM pragma_foreign_key_list('" +
      std::string(table) + "') ORDER BY id,seq";
  if (prepareSqliteStatement(database, query.c_str(), statement) != SQLITE_OK) {
    return std::nullopt;
  }
  std::vector<std::string> result;
  int step = SQLITE_OK;
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 1) != SQLITE_INTEGER) {
      return std::nullopt;
    }
    std::string shape = std::to_string(sqlite3_column_int(statement.get(), 0));
    shape += ":" + std::to_string(sqlite3_column_int(statement.get(), 1));
    for (int column = 2; column < 8; ++column) {
      if (sqlite3_column_type(statement.get(), column) != SQLITE_TEXT) {
        return std::nullopt;
      }
      shape += ":" + sqliteColumnString(statement.get(), column);
    }
    result.push_back(std::move(shape));
  }
  return step == SQLITE_DONE
             ? std::optional<std::vector<std::string>>(std::move(result))
             : std::nullopt;
}

enum class SchemaTokenKind { Word, QuotedIdentifier, Number, String, Symbol };

struct SchemaToken {
  SchemaTokenKind kind = SchemaTokenKind::Symbol;
  std::string text;
};

std::optional<std::vector<SchemaToken>>
tokenizeSchemaSql(std::string_view sql) {
  std::vector<SchemaToken> result;
  for (std::size_t index = 0; index < sql.size();) {
    const unsigned char character = static_cast<unsigned char>(sql[index]);
    if (std::isspace(character) != 0) {
      ++index;
      continue;
    }
    if (character == '-' && index + 1 < sql.size() && sql[index + 1] == '-') {
      index += 2;
      while (index < sql.size() && sql[index] != '\n' && sql[index] != '\r') {
        ++index;
      }
      continue;
    }
    if (character == '/' && index + 1 < sql.size() && sql[index + 1] == '*') {
      index += 2;
      const std::size_t closing = sql.find("*/", index);
      if (closing == std::string_view::npos) {
        return std::nullopt;
      }
      index = closing + 2;
      continue;
    }
    if (character == '"' || character == '`' || character == '[') {
      const char opening = static_cast<char>(character);
      const char closing = opening == '[' ? ']' : opening;
      std::string identifier;
      bool closed = false;
      for (++index; index < sql.size(); ++index) {
        if (sql[index] == closing) {
          if (opening != '[' && index + 1 < sql.size() &&
              sql[index + 1] == closing) {
            identifier.push_back(closing);
            ++index;
            continue;
          }
          ++index;
          closed = true;
          break;
        }
        identifier.push_back(sql[index]);
      }
      if (!closed) {
        return std::nullopt;
      }
      result.push_back({.kind = SchemaTokenKind::QuotedIdentifier,
                        .text = sqliteIdentifierFold(identifier)});
      continue;
    }
    if (character == '\'') {
      std::string value;
      bool closed = false;
      for (++index; index < sql.size(); ++index) {
        if (sql[index] == '\'') {
          if (index + 1 < sql.size() && sql[index + 1] == '\'') {
            value.push_back('\'');
            ++index;
            continue;
          }
          ++index;
          closed = true;
          break;
        }
        value.push_back(sql[index]);
      }
      if (!closed) {
        return std::nullopt;
      }
      result.push_back(
          {.kind = SchemaTokenKind::String, .text = std::move(value)});
      continue;
    }
    if (std::isdigit(character) != 0) {
      const std::size_t begin = index++;
      while (index < sql.size() &&
             std::isdigit(static_cast<unsigned char>(sql[index])) != 0) {
        ++index;
      }
      result.push_back({.kind = SchemaTokenKind::Number,
                        .text = std::string(sql.substr(begin, index - begin))});
      continue;
    }
    if (std::isalpha(character) != 0 || character == '_' || character == '$') {
      const std::size_t begin = index++;
      while (index < sql.size() &&
             (std::isalnum(static_cast<unsigned char>(sql[index])) != 0 ||
              sql[index] == '_' || sql[index] == '$')) {
        ++index;
      }
      result.push_back(
          {.kind = SchemaTokenKind::Word,
           .text = sqliteIdentifierFold(sql.substr(begin, index - begin))});
      continue;
    }
    result.push_back({.kind = SchemaTokenKind::Symbol,
                      .text = std::string(1, static_cast<char>(character))});
    ++index;
  }
  return result;
}

bool schemaTokensEquivalent(const SchemaToken &actual,
                            const SchemaToken &expected) {
  if (actual.text != expected.text) {
    return false;
  }
  if (actual.kind == expected.kind) {
    return true;
  }
  return expected.kind == SchemaTokenKind::Word &&
         actual.kind == SchemaTokenKind::QuotedIdentifier &&
         sqlite3_keyword_check(expected.text.c_str(),
                               static_cast<int>(expected.text.size())) == 0;
}

bool schemaSqlContainsEquivalent(std::string_view sql,
                                 std::string_view fragment) {
  const auto sqlTokens = tokenizeSchemaSql(sql);
  const auto fragmentTokens = tokenizeSchemaSql(fragment);
  if (!sqlTokens.has_value() || !fragmentTokens.has_value() ||
      fragmentTokens->empty() || fragmentTokens->size() > sqlTokens->size()) {
    return false;
  }
  for (std::size_t begin = 0;
       begin + fragmentTokens->size() <= sqlTokens->size(); ++begin) {
    bool matches = true;
    for (std::size_t offset = 0; offset < fragmentTokens->size(); ++offset) {
      if (!schemaTokensEquivalent((*sqlTokens)[begin + offset],
                                  (*fragmentTokens)[offset])) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return true;
    }
  }
  return false;
}

bool tableSqlContains(sqlite3 *database, std::string_view table,
                      std::string_view fragment) {
  const auto sql = schemaObjectSql(database, "table", table);
  return sql.has_value() && schemaSqlContainsEquivalent(*sql, fragment);
}

bool validateCompactReplayCriticalChecks(sqlite3 *database, int version) {
  const std::array<std::pair<std::string_view, std::string_view>, 9> required{
      std::pair{"replay_files",
                "CHECK((chart_result_id IS NOT NULL)!=(course_result_id IS "
                "NOT NULL))"},
      std::pair{"replay_files", "CHECK(history_index>=0)"},
      std::pair{"replay_files", "CHECK(length(content_sha256)=64)"},
      std::pair{"replay_files", "CHECK(compressed_size>0)"},
      std::pair{"replay_files", "CHECK(codec_version=2)"},
      std::pair{"replay_file_reservations", "CHECK(history_index>=0)"},
      std::pair{"replay_stem_sequences", "CHECK(last_history_index>=0)"},
      std::pair{"ir_outbox", "CHECK(local_result_ready IN (0,1))"},
      std::pair{"ir_outbox", "CHECK(next_request_user_intent IN (0,1))"},
  };
  if (!std::ranges::all_of(required, [&](const auto &entry) {
        return tableSqlContains(database, entry.first, entry.second);
      })) {
    return false;
  }
  if (version >= 12 &&
      (!tableSqlContains(database, "chart_results",
                         "CHECK(adopted_gauge_type BETWEEN 0 AND 5)") ||
       !tableSqlContains(database, "course_result_stages",
                         "CHECK(adopted_gauge_type BETWEEN 0 AND 5)"))) {
    return false;
  }
  return version < 13 ||
         (tableSqlContains(database, "replay_file_reservations",
                           "length(finalized_content_sha256)=64") &&
          tableSqlContains(database, "replay_file_reservations",
                           "finalized_compressed_size>0"));
}

bool validateCompactReplayRelationalShapes(sqlite3 *database, int version) {
  sqlite3 *templateDatabase = nullptr;
  if (sqlite3_open(":memory:", &templateDatabase) != SQLITE_OK ||
      templateDatabase == nullptr) {
    if (templateDatabase != nullptr) {
      sqlite3_close(templateDatabase);
    }
    return false;
  }
  const bool templateCreated = createCompactReplaySchema11(templateDatabase);
  constexpr std::array<std::string_view, 11> tables{
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
  constexpr std::array<std::string_view, 15> indexes{
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
  bool valid = templateCreated;
  for (std::string_view table : tables) {
    if (!valid) {
      break;
    }
    const auto actualUnique = uniqueKeyShapes(database, table);
    const auto expectedUnique = uniqueKeyShapes(templateDatabase, table);
    const auto actualForeignKeys = foreignKeyShapes(database, table);
    const auto expectedForeignKeys = foreignKeyShapes(templateDatabase, table);
    valid = actualUnique && expectedUnique &&
            *actualUnique == *expectedUnique && actualForeignKeys &&
            expectedForeignKeys && *actualForeignKeys == *expectedForeignKeys;
  }
  for (std::string_view index : indexes) {
    if (!valid) {
      break;
    }
    const auto actual = namedIndexShape(database, index);
    const auto expected = namedIndexShape(templateDatabase, index);
    valid = actual && expected && *actual == *expected;
  }
  valid = valid && validateCompactReplayCriticalChecks(database, version);
  sqlite3_close(templateDatabase);
  if (!valid) {
    SDL_Log("Refusing replay database with incompatible relational schema");
  }
  return valid;
}

bool validateCompactReplaySchemaObjects(sqlite3 *database, int version) {
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
  return validateCompactReplayRelationalShapes(database, version);
}

bool validateCompactReplaySchema11(sqlite3 *database) {
  return validateCompactReplaySchemaObjects(database, 11) &&
         validateCompactReplayColumnShapes(database, 11);
}

bool validateCompactReplaySchema12(sqlite3 *database) {
  return validateCompactReplaySchemaObjects(database, 12) &&
         validateCompactReplayColumnShapes(database, 12) &&
         namedTableColumnExists(database, "chart_results",
                                "adopted_gauge_type") &&
         namedTableColumnExists(database, "course_result_stages",
                                "adopted_gauge_type");
}

bool validateCompactReplaySchema13(sqlite3 *database) {
  return validateCompactReplaySchemaObjects(database, 13) &&
         validateCompactReplayColumnShapes(database, 13) &&
         namedTableColumnExists(database, "replay_file_reservations",
                                "finalized_content_sha256") &&
         namedTableColumnExists(database, "replay_file_reservations",
                                "finalized_compressed_size");
}

bool validateCompactReplaySchema14(sqlite3 *database) {
  return validateCompactReplaySchemaObjects(database, 14) &&
         validateCompactReplayColumnShapes(database, 14) &&
         namedTableColumnExists(database, "course_results", "entry_facts_json");
}

bool backfillAdoptedGaugeTypes(sqlite3 *database, std::string_view table) {
  const std::string selectSql =
      "SELECT rowid,provenance_json FROM " + std::string(table);
  SqliteStatementHandle rows;
  if (prepareSqliteStatement(database, selectSql.c_str(), rows) != SQLITE_OK) {
    return false;
  }
  std::vector<std::pair<sqlite3_int64, int>> updates;
  int step = SQLITE_OK;
  while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(rows.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_type(rows.get(), 1) != SQLITE_TEXT) {
      return false;
    }
    const auto *raw = sqlite3_column_text(rows.get(), 1);
    const int bytes = sqlite3_column_bytes(rows.get(), 1);
    const std::string provenanceJson(
        raw == nullptr ? "" : reinterpret_cast<const char *>(raw),
        static_cast<std::size_t>(std::max(0, bytes)));
    std::string diagnostic;
    const auto provenance =
        deserializeScoreProvenance(provenanceJson, diagnostic);
    if (!provenance.has_value()) {
      return false;
    }
    updates.emplace_back(sqlite3_column_int64(rows.get(), 0),
                         gaugeTypeIndex(provenance->gaugeType));
  }
  if (step != SQLITE_DONE) {
    return false;
  }
  rows.reset();

  const std::string updateSql = "UPDATE " + std::string(table) +
                                " SET adopted_gauge_type=? WHERE rowid=?";
  for (const auto &[rowId, gaugeType] : updates) {
    SqliteStatementHandle update;
    if (prepareSqliteStatement(database, updateSql.c_str(), update) !=
            SQLITE_OK ||
        sqlite3_bind_int(update.get(), 1, gaugeType) != SQLITE_OK ||
        sqlite3_bind_int64(update.get(), 2, rowId) != SQLITE_OK ||
        sqlite3_step(update.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
      return false;
    }
  }
  return true;
}

bool migrateCompactReplaySchema11To12(sqlite3 *database) {
  if (!validateCompactReplaySchema11(database) ||
      namedTableColumnExists(database, "chart_results",
                             "adopted_gauge_type") ||
      namedTableColumnExists(database, "course_result_stages",
                             "adopted_gauge_type")) {
    return false;
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active() ||
      !execSql(database,
               "ALTER TABLE chart_results ADD COLUMN adopted_gauge_type "
               "INTEGER NOT NULL DEFAULT 2 CHECK(adopted_gauge_type BETWEEN "
               "0 AND 5)",
               "adding chart adopted gauge type") ||
      !execSql(database,
               "ALTER TABLE course_result_stages ADD COLUMN "
               "adopted_gauge_type INTEGER NOT NULL DEFAULT 2 "
               "CHECK(adopted_gauge_type BETWEEN 0 AND 5)",
               "adding course-stage adopted gauge type") ||
      !backfillAdoptedGaugeTypes(database, "chart_results") ||
      !backfillAdoptedGaugeTypes(database, "course_result_stages") ||
      !setDatabaseUserVersion(database, 12)) {
    return false;
  }
  return transaction.commit(transactionError);
}

bool migrateCompactReplaySchema12To13(sqlite3 *database) {
  if (!validateCompactReplaySchema12(database) ||
      namedTableColumnExists(database, "replay_file_reservations",
                             "finalized_content_sha256") ||
      namedTableColumnExists(database, "replay_file_reservations",
                             "finalized_compressed_size")) {
    return false;
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active() ||
      !execSql(database,
               "ALTER TABLE replay_file_reservations ADD COLUMN "
               "finalized_content_sha256 TEXT CHECK("
               "finalized_content_sha256 IS NULL OR "
               "length(finalized_content_sha256)=64)",
               "adding finalized replay checksum") ||
      !execSql(database,
               "ALTER TABLE replay_file_reservations ADD COLUMN "
               "finalized_compressed_size INTEGER CHECK("
               "finalized_compressed_size IS NULL OR "
               "finalized_compressed_size>0)",
               "adding finalized replay size") ||
      !setDatabaseUserVersion(database, 13)) {
    return false;
  }
  return transaction.commit(transactionError);
}

bool backfillCourseEntryFacts(sqlite3 *database) {
  SqliteStatementHandle courses;
  if (prepareSqliteStatement(
          database,
          "SELECT id,total_charts,completed_charts FROM course_results "
          "ORDER BY id",
          courses) != SQLITE_OK) {
    return false;
  }
  std::vector<std::pair<sqlite3_int64, std::string>> updates;
  int courseStep = SQLITE_OK;
  while ((courseStep = sqlite3_step(courses.get())) == SQLITE_ROW) {
    const sqlite3_int64 courseId = sqlite3_column_int64(courses.get(), 0);
    const int totalCharts = sqlite3_column_int(courses.get(), 1);
    const int completedCharts = sqlite3_column_int(courses.get(), 2);
    if (courseId <= 0 || totalCharts <= 0 || totalCharts > 256 ||
        completedCharts <= 0 || completedCharts > totalCharts) {
      return false;
    }
    std::vector<int> totalNotes(static_cast<std::size_t>(totalCharts), 0);
    SqliteStatementHandle stages;
    if (prepareSqliteStatement(
            database,
            "SELECT stage_index,max_score FROM course_result_stages WHERE "
            "course_result_id=? ORDER BY stage_index",
            stages) != SQLITE_OK ||
        sqlite3_bind_int64(stages.get(), 1, courseId) != SQLITE_OK) {
      return false;
    }
    int expectedIndex = 0;
    int stageStep = SQLITE_OK;
    while ((stageStep = sqlite3_step(stages.get())) == SQLITE_ROW) {
      const int stageIndex = sqlite3_column_int(stages.get(), 0);
      const int maxScore = sqlite3_column_int(stages.get(), 1);
      if (stageIndex != expectedIndex || expectedIndex >= completedCharts ||
          maxScore <= 0 || maxScore % 2 != 0) {
        return false;
      }
      totalNotes[static_cast<std::size_t>(expectedIndex)] = maxScore / 2;
      ++expectedIndex;
    }
    if (stageStep != SQLITE_DONE || expectedIndex != completedCharts) {
      return false;
    }
    std::string json = "[";
    for (std::size_t index = 0; index < totalNotes.size(); ++index) {
      if (index != 0) {
        json.push_back(',');
      }
      json += "[" + std::to_string(totalNotes[index]) + ",0]";
    }
    json.push_back(']');
    updates.emplace_back(courseId, std::move(json));
  }
  if (courseStep != SQLITE_DONE) {
    return false;
  }
  courses.reset();
  for (const auto &[courseId, json] : updates) {
    SqliteStatementHandle update;
    if (prepareSqliteStatement(
            database,
            "UPDATE course_results SET entry_facts_json=? WHERE id=?",
            update) != SQLITE_OK ||
        !bindSqliteText(update.get(), 1, json) ||
        sqlite3_bind_int64(update.get(), 2, courseId) != SQLITE_OK ||
        sqlite3_step(update.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
      return false;
    }
  }
  return true;
}

bool migrateCompactReplaySchema13To14(sqlite3 *database) {
  if (!validateCompactReplaySchema13(database) ||
      namedTableColumnExists(database, "course_results", "entry_facts_json")) {
    return false;
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active() ||
      !execSql(database,
               "ALTER TABLE course_results ADD COLUMN entry_facts_json "
               "TEXT NOT NULL DEFAULT '[]'",
               "adding course entry facts") ||
      !backfillCourseEntryFacts(database) ||
      !setDatabaseUserVersion(database, 14)) {
    return false;
  }
  return transaction.commit(transactionError);
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
      "clear_type INTEGER NOT NULL,adopted_gauge_type INTEGER NOT NULL,"
      "gauge_history_json TEXT NOT NULL,"
      "judgement_timing_json TEXT,provenance_json TEXT NOT NULL,"
      "result_fingerprint TEXT NOT NULL,played_at_unix_ms INTEGER NOT NULL,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "CHECK(adopted_gauge_type BETWEEN 0 AND 5))",
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
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "entry_facts_json TEXT NOT NULL)",
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
      "clear_type INTEGER NOT NULL,adopted_gauge_type INTEGER NOT NULL,"
      "gauge_history_json TEXT NOT NULL,"
      "judgement_timing_json TEXT,provenance_json TEXT NOT NULL,"
      "PRIMARY KEY(course_result_id,stage_index),"
      "CHECK(adopted_gauge_type BETWEEN 0 AND 5),"
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
      "created_at_unix_ms INTEGER NOT NULL,finalized_content_sha256 TEXT,"
      "finalized_compressed_size INTEGER,CHECK(history_index>=0),"
      "CHECK((finalized_content_sha256 IS NULL AND "
      "finalized_compressed_size IS NULL) OR "
      "(length(finalized_content_sha256)=64 AND "
      "finalized_compressed_size>0)),"
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
    return validateCompactReplaySchema14(database);
  }
  if (*version == 13) {
    return migrateCompactReplaySchema13To14(database) &&
           validateCompactReplaySchema14(database);
  }
  if (*version == 12) {
    return migrateCompactReplaySchema12To13(database) &&
           migrateCompactReplaySchema13To14(database) &&
           validateCompactReplaySchema14(database);
  }
  if (*version == 11) {
    return migrateCompactReplaySchema11To12(database) &&
           migrateCompactReplaySchema12To13(database) &&
           migrateCompactReplaySchema13To14(database) &&
           validateCompactReplaySchema14(database);
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
  const auto resolveMetadata = replay_repository_detail::
      makeChartDatabaseReplayMetadataResolver(chartDatabasePath);
  const auto outcome = replay_repository_detail::migrateReplaySchema10To11(
      database, profileRoot, codec, fileStore, {}, resolveMetadata);
  if (outcome.status !=
          replay_repository_detail::ReplayMigrationOutcome::Status::Migrated &&
      outcome.status != replay_repository_detail::ReplayMigrationOutcome::
                            Status::AlreadyCurrent) {
    SDL_Log("Replay database migration failed: %s",
            outcome.diagnostic.c_str());
    return false;
  }
  std::string migratedVersionError;
  const auto migratedVersion =
      readSqliteUserVersion(database, migratedVersionError);
  if (!migratedVersion.has_value()) {
    return false;
  }
  if (*migratedVersion == 11 &&
      !migrateCompactReplaySchema11To12(database)) {
    return false;
  }
  std::string compactVersionError;
  const auto compactVersion =
      readSqliteUserVersion(database, compactVersionError);
  if (!compactVersion.has_value()) {
    return false;
  }
  if (*compactVersion == 12 &&
      !migrateCompactReplaySchema12To13(database)) {
    return false;
  }
  std::string finalizedVersionError;
  const auto finalizedVersion =
      readSqliteUserVersion(database, finalizedVersionError);
  if (!finalizedVersion.has_value()) {
    return false;
  }
  if (*finalizedVersion == 13 &&
      !migrateCompactReplaySchema13To14(database)) {
    return false;
  }
  return validateCompactReplaySchema14(database);
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
