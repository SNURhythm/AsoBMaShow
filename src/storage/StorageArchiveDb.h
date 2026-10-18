#pragma once
#include "StorageArchive.h"
#include "../sqlite3.h"
#include <limits>

namespace storage_management::archive_detail {
inline void require(bool condition, const char *message, StorageReason reason = StorageReason::UnsupportedFormat) {
  if (!condition) throw StorageArchiveError(reason, message);
}
inline void active(const Checkpoint &check) {
  require(!check || check(), "Archive operation cancelled", StorageReason::Cancelled);
}
inline std::uint64_t plus(std::uint64_t a, std::uint64_t b) {
  require(a <= std::uint64_t(INT64_MAX) && b <= std::uint64_t(INT64_MAX) - a, "Archive size overflow"); return a + b;
}
inline std::uint64_t number(std::string_view bytes, std::size_t offset, std::size_t size) {
  require(size <= 8 && offset <= bytes.size() && size <= bytes.size() - offset, "Truncated archive metadata");
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size; ++i) value |= std::uint64_t(static_cast<unsigned char>(bytes[offset + i])) << (i * 8);
  return value;
}
inline void put(std::string &bytes, std::size_t offset, std::uint64_t value, std::size_t size) {
  require(offset <= bytes.size() && size <= bytes.size() - offset, "Invalid archive metadata write");
  for (std::size_t i = 0; i < size; ++i) bytes[offset + i] = static_cast<char>(value >> (i * 8));
}
struct Statement {
  sqlite3_stmt *value = nullptr;
  Statement(sqlite3 *db, const char *sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK)
      throw StorageArchiveError(StorageReason::IoError, sqlite3_errmsg(db));
  }
  ~Statement() { sqlite3_finalize(value); }
  Statement(const Statement &) = delete;
  void reset() {
    require(sqlite3_reset(value) == SQLITE_OK && sqlite3_clear_bindings(value) == SQLITE_OK,
        "Cannot reset archive metadata query", StorageReason::IoError);
  }
  void bind(int slot, std::string_view text) {
    require(text.size() <= INT_MAX && sqlite3_bind_blob(value, slot, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT) == SQLITE_OK,
        "Cannot store archive metadata", StorageReason::IoError);
  }
  void text(int slot, std::string_view text) {
    require(text.size() <= INT_MAX && sqlite3_bind_text(value, slot, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT) == SQLITE_OK,
        "Cannot store archive name", StorageReason::IoError);
  }
  void bind(int slot, std::uint64_t n) {
    require(n <= INT64_MAX && sqlite3_bind_int64(value, slot, static_cast<sqlite3_int64>(n)) == SQLITE_OK,
        "Cannot store archive offset", StorageReason::IoError);
  }
  bool step() {
    const auto rc = sqlite3_step(value);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw StorageArchiveError(StorageReason::IoError, sqlite3_errmsg(sqlite3_db_handle(value)));
  }
  std::uint64_t integer(int i) const {
    const auto n = sqlite3_column_int64(value, i);
    require(n >= 0, "Negative archive metadata integer"); return static_cast<std::uint64_t>(n);
  }
  std::string bytes(int i) const {
    const auto size = sqlite3_column_bytes(value, i);
    const auto *data = static_cast<const char *>(sqlite3_column_blob(value, i));
    return size ? std::string(data, size) : std::string{};
  }
};
// Both the lightweight review index and the physical update manifest use these
// columns. Stop after two matches: only ambiguity, not the full count, matters.
inline constexpr auto candidateCollisionSql = "SELECT count(*) FROM (SELECT 1 FROM entries WHERE (stem=? AND media=?) OR (name=? AND id!=?) LIMIT 2)";
inline constexpr auto nextArchiveEntrySql = "SELECT id,offset,span,data_offset,packed,bytes,crc,method,flags,directory,name,raw_name,metadata FROM entries WHERE offset > ? ORDER BY offset LIMIT 1";
// Keyset queries are reset after copying each row, before callers mutate the
// manifest. Their owner keeps them within one database open/close lifetime.
std::optional<StorageArchiveEntry> nextArchiveEntry(Statement &, std::optional<std::uint64_t> after = {});
std::optional<AssetCandidate> archiveCandidate(const StorageArchiveEntry &, const StoragePolicy &,
    StorageDiagnostic &, Statement &collisions);
struct Database {
  sqlite3 *value = nullptr;
  explicit Database(const std::filesystem::path &path, bool create = true) {
    const auto utf8 = path.u8string(); const std::string name(utf8.begin(), utf8.end());
    if (sqlite3_open_v2(name.c_str(), &value, SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0) | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
      const std::string error = value ? sqlite3_errmsg(value) : "Cannot open archive manifest";
      sqlite3_close(value); value = nullptr; throw StorageArchiveError(StorageReason::IoError, error);
    }
    try {
      sqlite3_limit(value, SQLITE_LIMIT_LENGTH, 4 * 1024 * 1024);
      exec("PRAGMA trusted_schema=OFF; PRAGMA cache_size=-4096; PRAGMA temp_store=FILE; PRAGMA synchronous=FULL; PRAGMA journal_mode=DELETE;");
    } catch (...) { sqlite3_close(value); value = nullptr; throw; }
  }
  ~Database() { sqlite3_close(value); }
  Database(const Database &) = delete;
  void exec(const char *sql) {
    if (sqlite3_exec(value, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
      throw StorageArchiveError(StorageReason::IoError, sqlite3_errmsg(value));
  }
};
}
