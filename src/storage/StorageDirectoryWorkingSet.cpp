#include "StorageDirectoryInventory.h"
#include "../audio/ChartVisualAssetResolver.h"
#include "../FileChecksum.h"
#include "../Uuid.h"
#include "../sqlite3.h"
#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <stdexcept>

namespace storage_management {
namespace fs = std::filesystem;
namespace {
using Key = std::u8string;
using Stem = std::pair<Key, bool>;
using asobmshow::chart_assets::MediaKind;
Key key(const fs::path &path) { return path.generic_u8string(); }
Stem stem(const fs::path &path) {
  auto value = path.stem().u8string();
  for (auto &c : value) if (c >= u8'A' && c <= u8'Z') c += u8'a' - u8'A';
  return {std::move(value), asobmshow::chart_assets::ClassifyMediaPath(path) == MediaKind::Audio};
}
bool media(const StorageDirectoryEntry &entry) {
  return entry.kind != StorageEntryKind::Directory &&
      asobmshow::chart_assets::ClassifyMediaPath(entry.name) != MediaKind::Other;
}
void number(std::string &bytes, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<char>(value >> (i * 8)));
}
std::string payload(const StorageDirectoryEntry &entry) {
  std::string bytes;
  number(bytes, static_cast<unsigned>(entry.kind)); number(bytes, entry.bytes);
  number(bytes, static_cast<unsigned>(entry.permissions)); number(bytes, entry.identity.has_value());
  if (entry.identity) {
    const auto &id = *entry.identity;
    const auto seconds = std::chrono::floor<std::chrono::seconds>(id.modified.time_since_epoch());
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(id.modified.time_since_epoch() - seconds);
    number(bytes, id.device); number(bytes, id.fileId);
    number(bytes, std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(seconds.count())));
    number(bytes, nanos.count());
  }
  number(bytes, entry.chartChange.has_value());
  number(bytes, entry.chartChange ? (*entry.chartChange)[0] : 0);
  number(bytes, entry.chartChange ? (*entry.chartChange)[1] : 0);
  return bytes;
}
StorageDirectoryEntry decode(sqlite3_stmt *statement) {
  const auto *name = static_cast<const char8_t *>(sqlite3_column_blob(statement, 0));
  const auto nameSize = sqlite3_column_bytes(statement, 0);
  const auto *data = static_cast<const unsigned char *>(sqlite3_column_blob(statement, 1));
  const auto size = sqlite3_column_bytes(statement, 1);
  if (!name || nameSize <= 0 || !data || (size != 56 && size != 88))
    throw std::runtime_error("Invalid directory metadata record");
  const auto read = [&](int offset) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= std::uint64_t(data[offset + i]) << (i * 8);
    return value;
  };
  StorageDirectoryEntry entry{.name = Key(name, name + nameSize), .kind = static_cast<StorageEntryKind>(read(0)),
      .bytes = read(8), .permissions = static_cast<fs::perms>(read(16))};
  if (read(24)) {
    if (size != 88) throw std::runtime_error("Truncated directory identity");
    const auto seconds = std::chrono::seconds(std::bit_cast<std::int64_t>(read(48)));
    const auto nanos = std::chrono::nanoseconds(read(56));
    entry.identity = FileIdentity{.bytes = entry.bytes,
        .modified = fs::file_time_type(std::chrono::duration_cast<fs::file_time_type::duration>(seconds) +
            std::chrono::duration_cast<fs::file_time_type::duration>(nanos)),
        .device = read(32), .fileId = read(40)};
  }
  if (read(size - 24)) entry.chartChange = StorageChartChangeStamp{read(size - 16), read(size - 8)};
  return entry;
}
bool validName(const fs::path &path) {
  const auto value = key(path);
  return !value.empty() && path == path.filename() && path != "." && path != ".." &&
      value.find(u8'\0') == Key::npos && !path.has_root_path();
}
std::size_t entryBytes(const StorageDirectoryEntry &entry) {
  return sizeof(entry) + 128 + key(entry.name).size() * 3;
}
}
struct StorageDirectoryInventory::Impl {
  fs::path directory, scratch;
  StorageDirectoryLimits limits;
  std::map<Key, StorageDirectoryEntry> memory;
  std::map<Stem, std::uint64_t> stems;
  std::size_t count = 0, bytes = 0;
  sqlite3 *db = nullptr;
  std::string failure;
  enum Statement { Insert, Upsert, Remove, Find, Next, Visit, Count, StatementCount };
  std::array<sqlite3_stmt *, StatementCount> statements{};
  ~Impl() {
    for (auto *statement : statements) sqlite3_finalize(statement);
    if (db) sqlite3_close(db);
    if (!scratch.empty()) {
      std::error_code ignored;
      fs::remove(scratch / "directory.sqlite", ignored);
      fs::remove(scratch, ignored); // Preserve unexpected files in our private directory.
    }
  }
  [[noreturn]] void fail() const { throw std::runtime_error(!failure.empty() ? failure : db ? sqlite3_errmsg(db) : "Cannot open directory metadata index"); }
  void sql(const char *text) { if (sqlite3_exec(db, text, nullptr, nullptr, nullptr) != SQLITE_OK) fail(); }
  sqlite3_stmt *reset(Statement which) const {
    if (!failure.empty() || !statements[which]) fail();
    auto *statement = statements[which];
    sqlite3_reset(statement); sqlite3_clear_bindings(statement);
    return statement;
  }
  void bind(sqlite3_stmt *statement, int position, const void *data, std::size_t size) const {
    if (sqlite3_bind_blob64(statement, position, data, size, SQLITE_TRANSIENT) != SQLITE_OK) fail();
  }
  void bind(sqlite3_stmt *statement, int position, const Key &value) const { bind(statement, position, value.data(), value.size()); }
  void done(sqlite3_stmt *statement) const {
    const auto result = sqlite3_step(statement);
    sqlite3_reset(statement);
    if (result != SQLITE_DONE) fail();
  }
  void put(const StorageDirectoryEntry &entry, bool replace) {
    auto *statement = reset(replace ? Upsert : Insert);
    const auto name = key(entry.name);
    const auto stemKey = stem(entry.name);
    const auto data = payload(entry);
    bind(statement, 1, name); bind(statement, 2, data.data(), data.size()); bind(statement, 3, stemKey.first);
    sqlite3_bind_int(statement, 4, stemKey.second); sqlite3_bind_int(statement, 5, media(entry));
    done(statement);
  }
  void spill() {
    try {
    const auto root = limits.scratchRoot.empty() ? fs::temp_directory_path() : limits.scratchRoot;
    fs::create_directories(root);
    const auto created = root / ("storage-directory-" + uuid::generateV4());
    if (!fs::create_directory(created)) throw std::runtime_error("Cannot create private directory metadata index");
    scratch = created;
    fs::permissions(scratch, fs::perms::owner_all, fs::perm_options::replace);
    const auto databasePath = (scratch / "directory.sqlite").u8string();
    if (sqlite3_open_v2(reinterpret_cast<const char *>(databasePath.c_str()), &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) fail();
    sql("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA cache_size=-8192;"
        "CREATE TABLE entries(name BLOB PRIMARY KEY, payload BLOB NOT NULL, stem BLOB NOT NULL, audio INTEGER NOT NULL, media INTEGER NOT NULL) WITHOUT ROWID;"
        "CREATE INDEX stems ON entries(stem,audio) WHERE media=1; BEGIN;");
    constexpr const char *queries[] = {
      "INSERT INTO entries VALUES(?1,?2,?3,?4,?5)",
      "INSERT INTO entries VALUES(?1,?2,?3,?4,?5) ON CONFLICT(name) DO UPDATE SET payload=excluded.payload,stem=excluded.stem,audio=excluded.audio,media=excluded.media",
      "DELETE FROM entries WHERE name=?1", "SELECT name,payload FROM entries WHERE name=?1",
      "SELECT name,payload FROM entries WHERE name>?1 ORDER BY name LIMIT 1",
      "SELECT name,payload FROM entries ORDER BY name", "SELECT count(*) FROM entries WHERE stem=?1 AND audio=?2 AND media=1"
    };
    for (std::size_t i = 0; i < statements.size(); ++i)
      if (sqlite3_prepare_v2(db, queries[i], -1, &statements[i], nullptr) != SQLITE_OK) fail();
    for (const auto &[name, entry] : memory) put(entry, false);
    memory.clear(); stems.clear(); bytes = 0;
    } catch (const std::exception &e) { failure = e.what(); throw; }
  }
  std::optional<StorageDirectoryEntry> get(const fs::path &name, bool next) const {
    if (!failure.empty()) fail();
    if (!db) {
      const auto found = next ? memory.upper_bound(key(name)) : memory.find(key(name));
      return found == memory.end() ? std::nullopt : std::optional(found->second);
    }
    auto *statement = reset(next ? Next : Find);
    bind(statement, 1, key(name));
    const auto status = sqlite3_step(statement);
    std::optional<StorageDirectoryEntry> result;
    if (status == SQLITE_ROW) result = decode(statement);
    sqlite3_reset(statement);
    if (status != SQLITE_ROW && status != SQLITE_DONE) fail();
    return result;
  }
  void subtract(const StorageDirectoryEntry &entry) {
    bytes -= entryBytes(entry);
    if (media(entry)) {
      auto found = stems.find(stem(entry.name));
      if (--found->second == 0) stems.erase(found);
    }
  }
  void add(const StorageDirectoryEntry &entry, bool replace) {
    if (!failure.empty()) fail();
    if (!validName(entry.name) || (entry.identity && entry.identity->bytes != entry.bytes))
      throw std::runtime_error("Invalid directory entry metadata");
    const auto old = replace ? get(entry.name, false) : std::optional<StorageDirectoryEntry>{};
    if (!db && (count + !old > limits.records || entryBytes(entry) > limits.bytes - std::min(bytes, limits.bytes))) spill();
    if (db) put(entry, replace);
    else {
      const auto [found, inserted] = memory.try_emplace(key(entry.name), entry);
      if (!inserted) {
        if (!replace) throw std::runtime_error("Duplicate directory entry");
        subtract(found->second); found->second = entry;
      }
      bytes += entryBytes(entry);
      if (media(entry)) ++stems[stem(entry.name)];
    }
    if (!old) ++count;
  }
};
StorageDirectoryInventory::StorageDirectoryInventory(fs::path path, StorageDirectoryLimits limits)
    : impl_(std::make_unique<Impl>()) { impl_->directory = std::move(path); impl_->limits = std::move(limits); }
StorageDirectoryInventory::~StorageDirectoryInventory() = default;
bool StorageDirectoryInventory::Add(const StorageDirectoryEntry &entry, std::string &error) {
  error.clear(); try { impl_->add(entry, false); return true; } catch (const std::exception &e) { error = e.what(); return false; }
}
bool StorageDirectoryInventory::Set(const StorageDirectoryEntry &entry, std::string &error) {
  error.clear(); try { impl_->add(entry, true); return true; } catch (const std::exception &e) { error = e.what(); return false; }
}
bool StorageDirectoryInventory::Remove(const fs::path &name, std::string &error) {
  error.clear();
  try {
    const auto old = impl_->get(name, false);
    if (!old) return true;
    if (impl_->db) { auto *statement = impl_->reset(Impl::Remove); impl_->bind(statement, 1, key(name)); impl_->done(statement); }
    else { impl_->subtract(*old); impl_->memory.erase(key(name)); }
    --impl_->count; return true;
  } catch (const std::exception &e) { error = e.what(); return false; }
}
std::optional<StorageDirectoryEntry> StorageDirectoryInventory::Find(const fs::path &name, std::string &error) const {
  error.clear(); try { return impl_->get(name, false); } catch (const std::exception &e) { error = e.what(); return {}; }
}
std::optional<StorageDirectoryEntry> StorageDirectoryInventory::Next(const fs::path &after, std::string &error) const {
  error.clear(); try { return impl_->get(after, true); } catch (const std::exception &e) { error = e.what(); return {}; }
}
bool StorageDirectoryInventory::Visit(Checkpoint check, EntryVisitor visitor, std::string &error) const {
  error.clear();
  try {
    if (!impl_->failure.empty()) impl_->fail();
    const auto visit = [&](const auto &entry) {
      if (check && !check()) throw std::runtime_error("Cancelled");
      if (!visitor(entry)) throw std::runtime_error("Directory metadata visit interrupted");
    };
    if (!impl_->db) { for (const auto &[name, entry] : impl_->memory) visit(entry); }
    else {
      auto *statement = impl_->reset(Impl::Visit);
      try {
        int status;
        while ((status = sqlite3_step(statement)) == SQLITE_ROW) visit(decode(statement));
        sqlite3_reset(statement);
        if (status != SQLITE_DONE) impl_->fail();
      } catch (...) { sqlite3_reset(statement); throw; }
    }
    return true;
  } catch (const std::exception &e) { error = e.what(); return false; }
}
std::uint64_t StorageDirectoryInventory::StemCount(const fs::path &name, bool audio, std::string &error) const {
  error.clear();
  try {
    auto value = stem(name); value.second = audio;
    if (!impl_->db) { const auto found = impl_->stems.find(value); return found == impl_->stems.end() ? 0 : found->second; }
    auto *statement = impl_->reset(Impl::Count);
    impl_->bind(statement, 1, value.first); sqlite3_bind_int(statement, 2, audio);
    if (sqlite3_step(statement) != SQLITE_ROW) impl_->fail();
    const auto result = sqlite3_column_int64(statement, 0); sqlite3_reset(statement);
    return result;
  } catch (const std::exception &e) { error = e.what(); return 0; }
}
std::string StorageDirectoryInventory::Fingerprint(Checkpoint check, std::string &error) const {
  file_checksum::Sha256 hash;
  if (!Visit(std::move(check), [&](const auto &entry) {
    const auto name = key(entry.name); std::string record;
    number(record, name.size()); record.append(reinterpret_cast<const char *>(name.data()), name.size());
    record += payload(entry);
    hash.update(std::as_bytes(std::span(record.data(), record.size()))); return true;
  }, error)) return {};
  return hash.finalHex();
}
std::size_t StorageDirectoryInventory::Size() const { return impl_->count; }
bool StorageDirectoryInventory::Spilled() const { return impl_->db != nullptr; }
const fs::path &StorageDirectoryInventory::Directory() const { return impl_->directory; }
}
