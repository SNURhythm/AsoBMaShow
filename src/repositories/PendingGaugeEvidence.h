#pragma once
#include "SqliteRAII.h"
#include "../FileChecksum.h"
#include "../scene/play/GaugeEvidence.h"
#include <bit>
#include <limits>

namespace pending_gauge_evidence {
// Only in-flight saves own these numbers. Acknowledging the parent score write
// removes them; completed result history deliberately has no gauge payload.
inline constexpr const char *kTableSql =
    "CREATE TABLE modern_pending_chart_gauge_evidence("
    "attempt_id TEXT PRIMARY KEY NOT NULL REFERENCES modern_pending_chart_score_writes(attempt_id) ON DELETE CASCADE,"
    "passed_notes INTEGER NOT NULL,survival_mask INTEGER NOT NULL,"
    "death0 INTEGER NOT NULL,death1 INTEGER NOT NULL,death2 INTEGER NOT NULL,"
    "death3 INTEGER NOT NULL,death4 INTEGER NOT NULL,death5 INTEGER NOT NULL,"
    "death6 INTEGER NOT NULL,death7 INTEGER NOT NULL,death8 INTEGER NOT NULL,"
    "value0 REAL NOT NULL,value1 REAL NOT NULL,value2 REAL NOT NULL,"
    "value3 REAL NOT NULL,value4 REAL NOT NULL,value5 REAL NOT NULL,"
    "value6 REAL NOT NULL,value7 REAL NOT NULL,value8 REAL NOT NULL,"
    "evidence_fingerprint TEXT NOT NULL)";

inline std::string fingerprint(std::string_view attempt, const GaugeEvidence &evidence) {
  std::string bytes("pending-gauge-v1:");
  bytes += attempt;
  const auto append = [&](std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
      bytes.push_back(static_cast<char>((value >> shift) & 0xff));
  };
  append(evidence.passedNotes);
  append(evidence.survivalMask);
  for (int death : evidence.deathPassedNotes) append(static_cast<std::uint32_t>(death));
  for (float value : evidence.values) append(std::bit_cast<std::uint32_t>(value == 0 ? 0.0f : value));
  return file_checksum::sha256(bytes);
}

inline bool insert(sqlite3 *db, std::string_view attempt, const GaugeEvidence &evidence) {
  if (!evidence.recorded) return true;
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(db,
      "INSERT INTO modern_pending_chart_gauge_evidence VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
      statement) != SQLITE_OK || !bindSqliteText(statement, 1, std::string(attempt))) return false;
  int index = 2;
  bool bound = sqlite3_bind_int(statement, index++, evidence.passedNotes) == SQLITE_OK;
  bound &= sqlite3_bind_int(statement, index++, evidence.survivalMask) == SQLITE_OK;
  for (int death : evidence.deathPassedNotes) bound &= sqlite3_bind_int(statement, index++, death) == SQLITE_OK;
  for (float value : evidence.values) bound &= sqlite3_bind_double(statement, index++, value) == SQLITE_OK;
  return bound && bindSqliteText(statement, index, fingerprint(attempt, evidence)) &&
         sqlite3_step(statement) == SQLITE_DONE;
}

enum class ReadStatus { Loaded, Invalid, StorageFailure };
inline ReadStatus read(sqlite3 *db, std::string_view attempt, int totalNotes, GaugeEvidence &evidence) {
  evidence = {};
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(db,
      "SELECT passed_notes,survival_mask,death0,death1,death2,death3,death4,death5,death6,death7,death8,"
      "value0,value1,value2,value3,value4,value5,value6,value7,value8,evidence_fingerprint "
      "FROM modern_pending_chart_gauge_evidence WHERE attempt_id=?", statement) != SQLITE_OK ||
      !bindSqliteText(statement, 1, std::string(attempt))) return ReadStatus::StorageFailure;
  const int rc = sqlite3_step(statement);
  if (rc == SQLITE_DONE) return ReadStatus::Loaded; // Pre-feature pending save.
  if (rc != SQLITE_ROW) return ReadStatus::StorageFailure;
  for (int i = 0; i < 11; ++i) {
    const auto value = sqlite3_column_int64(statement, i);
    if (sqlite3_column_type(statement, i) != SQLITE_INTEGER ||
        value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
      return ReadStatus::Invalid;
  }
  const int mask = sqlite3_column_int(statement, 1);
  if (mask < 0 || mask >= (1 << kGaugeTypeCount)) return ReadStatus::Invalid;
  evidence.recorded = true;
  evidence.passedNotes = sqlite3_column_int(statement, 0);
  evidence.survivalMask = static_cast<std::uint16_t>(mask);
  for (std::size_t i = 0; i < kGaugeTypeCount; ++i) {
    evidence.deathPassedNotes[i] = sqlite3_column_int(statement, 2 + i);
    const int type = sqlite3_column_type(statement, 11 + i);
    if (type != SQLITE_FLOAT && type != SQLITE_INTEGER) return ReadStatus::Invalid;
    const double value = sqlite3_column_double(statement, 11 + i);
    evidence.values[i] = static_cast<float>(value);
    if (static_cast<double>(evidence.values[i]) != value) return ReadStatus::Invalid;
  }
  if (!evidence.valid(totalNotes) || sqlite3_column_type(statement, 20) != SQLITE_TEXT ||
      sqliteColumnTextView(statement, 20) != fingerprint(attempt, evidence)) return ReadStatus::Invalid;
  return sqlite3_step(statement) == SQLITE_DONE ? ReadStatus::Loaded : ReadStatus::StorageFailure;
}
}
