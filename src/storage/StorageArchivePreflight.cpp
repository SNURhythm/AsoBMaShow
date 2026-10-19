#include "StoragePreflight.h"
#include "StorageArchiveDb.h"
#include "StoragePathIndex.h"
#include "../ArchiveFile.h"
#include "../ArchiveSourceIdentity.h"
#include "../Uuid.h"

namespace storage_management {
namespace fs = std::filesystem;
using namespace archive_detail;
namespace {
struct Scratch {
  fs::path path;
  ~Scratch() { if (!path.empty()) { std::error_code error; fs::remove_all(path, error); } }
};
std::string pathText(const fs::path &path) { const auto text = path.generic_u8string(); return {text.begin(), text.end()}; }
std::string lower(std::string text) {
  for (auto &c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return text;
}
void checkContainer(const StorageArchiveIO &source, std::uint64_t bytes, const std::string &extension) {
  if (extension == ".tar") {
    require(bytes >= 512 && source.Read(257, 6) == std::string("ustar\0", 6), "Only uncompressed USTAR supports in-place updates");
    return;
  }
  // ZIP64 and self-extracting ZIPs need not start at offset zero. The bounded
  // end record is sufficient for a review; Apply verifies every physical span.
  const auto tailSize = std::min<std::uint64_t>(bytes, 65557);
  require(tailSize >= 22, "Archive is too short");
  const auto tail = source.Read(bytes - tailSize, tailSize);
  for (std::size_t at = tail.size() - 22;; --at) {
    if (number(tail, at, 4) == 0x06054b50 && number(tail, at + 20, 2) == tail.size() - at - 22) {
      require(!number(tail, at + 4, 2) && !number(tail, at + 6, 2) &&
          number(tail, at + 8, 2) == number(tail, at + 10, 2), "Split ZIP archives cannot be updated in place");
      return;
    }
    if (!at) break;
  }
  throw StorageArchiveError(StorageReason::UnsupportedFormat, "Cannot locate ZIP end record");
}
void indexEntries(Database &db, const fs::path &path, Checkpoint check, StorageScanMetrics *metrics) {
  // Disposable name/collision index, not a durable update manifest. Reuse one
  // insert and one streaming cursor, with bounded SQLite memory for huge packs.
  db.exec("PRAGMA synchronous=OFF; PRAGMA journal_mode=OFF;"
      "CREATE TABLE entries(id INTEGER PRIMARY KEY,name TEXT NOT NULL COLLATE NOCASE UNIQUE,"
      " bytes INTEGER NOT NULL,directory INTEGER NOT NULL,stem TEXT NOT NULL,media INTEGER NOT NULL);"
      "CREATE INDEX entry_stems ON entries(stem,media); BEGIN;");
  Statement insert(db.value, "INSERT INTO entries(name,bytes,directory,stem,media) VALUES(?,?,?,?,?)");
  std::string error;
  if (metrics) ++metrics->archiveIndexVisits;
  const bool complete = archive_file::visitIndexEntries(path, [&](const auto &entry) {
    active(check);
    require(!entry.solid, "Solid archives cannot be updated in place");
    const auto name = pathText(entry.path);
    require(!name.empty() && !entry.path.is_absolute() && entry.path.lexically_normal() == entry.path &&
        name.find('\0') == std::string::npos && name.find('\\') == std::string::npos && name.find(':') == std::string::npos,
        "Unsafe archive filename", StorageReason::UnsafePath);
    for (const auto &part : entry.path) require(part != ".." && part != ".", "Unsafe archive path", StorageReason::UnsafePath);
    auto stem = entry.path; stem.replace_extension();
    insert.reset(); insert.text(1, name); insert.bind(2, entry.size); insert.bind(3, entry.directory);
    insert.text(4, lower(pathText(stem)));
    insert.bind(5, static_cast<unsigned>(asobmshow::chart_assets::ClassifyMediaPath(entry.path))); insert.step();
    if (metrics) ++metrics->archiveEntries;
    return true;
  }, &error, check);
  active(check);
  if (!complete) throw StorageArchiveError(StorageReason::IncompleteInventory,
      error.empty() ? "Archive index is unavailable" : error);
  db.exec("COMMIT;");
}
}
void AppendStorageArchivePreflight(StoragePreflight &result, Checkpoint check, StorageScanMetrics *metrics) {
  std::map<fs::path, std::vector<std::size_t>> batches;
  for (std::size_t i = 0; i < result.groups.size(); ++i) {
    auto &group = result.groups[i]; if (group.sourceKind != StorageSourceKind::Archive) continue;
    fs::path archive, inner;
    if (!result.request.pathAccessRoots.empty() && StoragePathIndex(result.request.pathAccessRoots).Ancestors(group.root).empty())
      group.reason = {StorageReason::UnsafePath, "Archive is outside authorized storage roots"};
    else if (!FindStorageArchive(group.root, archive, inner, result.request.pathAccessRoots))
      group.reason = {StorageReason::UnsupportedSource, "Archive is not a writable local file"};
    else if (!result.request.pathAccessRoots.empty() && StoragePathIndex(result.request.pathAccessRoots).Ancestors(archive).empty())
      group.reason = {StorageReason::UnsafePath, "Archive is outside authorized storage roots"};
    else { group.root = inner.empty() ? archive : archive / inner; batches[archive].push_back(i); }
  }
  for (const auto &[path, positions] : batches) {
    try {
      active(check);
      const auto extension = lower(path.extension().string());
      require(extension == ".zip" || extension == ".tar", "This archive format does not support bounded in-place updates");
      std::string error;
      const auto identity = ReadFileIdentity(path, error, result.request.pathAccessRoots);
      require(identity.has_value(), "Unsafe archive source", StorageReason::UnsafePath);
      StorageArchiveIO source(path, result.request.pathAccessRoots);
      const auto sourceIdentity = archive_source_identity::KeyForPath(path);
      require(!sourceIdentity.empty(), "Cannot identify archive source", StorageReason::StaleInput);
      // Reject unsupported container families cheaply. Full entry/layout checks
      // run in UpdateStorageArchive before encoding, journaling or any write.
      checkContainer(source, identity->bytes, extension);
      auto root = result.request.scanScratchRoot;
      if (root.empty()) root = fs::canonical(fs::temp_directory_path()) / "asobmashow-storage-archive-scan";
      fs::create_directories(root);
      const auto &privateRoot = result.request.scanAccessRoot;
      const std::vector<fs::path> scratchAccess = privateRoot.empty() ? std::vector<fs::path>{} : std::vector<fs::path>{privateRoot};
      require(IsSafeLocalPath(root, error, scratchAccess), "Unsafe archive scan directory", StorageReason::UnsafePath);
      Scratch scratch;
      const auto privatePath = root / uuid::generateV4();
      require(fs::create_directory(privatePath), "Cannot create archive scan directory", StorageReason::IoError);
      scratch.path = privatePath; fs::permissions(privatePath, fs::perms::owner_all, fs::perm_options::replace);
      Database db(privatePath / "review.sqlite");
      indexEntries(db, path, check, metrics);
      StorageArchiveSummary summary{.path = path, .identity = *identity, .sourceIdentity = sourceIdentity};
      std::vector<GroupStorageMeasurement> measured;
      for (const auto position : positions) {
        auto row = result.groups[position]; row.totalBytes = 0;
        row.sourceIdentity = sourceIdentity;
        measured.push_back(std::move(row));
        auto inner = result.groups[position].root.lexically_relative(path);
        if (inner == ".") inner.clear(); summary.innerRoots.push_back(std::move(inner));
      }
      const StoragePathIndex selected(summary.innerRoots, StoragePathComparison::CaseSensitive);
      std::uint64_t total = 0, skippedCount = 0;
      std::vector<FileResult> skipped;
      Statement entries(db.value, "SELECT id,name,bytes,directory FROM entries ORDER BY id");
      Statement collisions(db.value, candidateCollisionSql);
      while (entries.step()) {
        active(check); if (entries.integer(3)) continue;
        StorageArchiveEntry entry{.id = entries.integer(0), .bytes = entries.integer(2), .name = entries.bytes(1)};
        const auto inner = fs::u8path(entry.name);
        const auto matches = selected.Ancestors(inner); if (matches.empty()) continue;
        total = plus(total, entry.bytes);
        const auto kind = asobmshow::chart_assets::ClassifyMediaPath(inner);
        for (const auto position : matches) {
          auto &row = measured[position]; *row.totalBytes = plus(*row.totalBytes, entry.bytes);
          if (kind == asobmshow::chart_assets::MediaKind::Audio) row.audioBytes = plus(row.audioBytes, entry.bytes);
          if (kind == asobmshow::chart_assets::MediaKind::Video) row.videoBytes = plus(row.videoBytes, entry.bytes);
          if (kind == asobmshow::chart_assets::MediaKind::Image) row.imageBytes = plus(row.imageBytes, entry.bytes);
        }
        StorageDiagnostic reason;
        if (archiveCandidate(entry, result.request.policy, reason, collisions)) {
          ++summary.eligibleCount; summary.eligibleBytes = plus(summary.eligibleBytes, entry.bytes);
        } else if (reason.code != StorageReason::None) {
          ++skippedCount;
          if (skipped.size() < 100) skipped.push_back({.source = path / inner, .reason = reason});
        }
      }
      source.CheckPath();
      require(ReadFileIdentity(path, error, result.request.pathAccessRoots) == identity,
          "Archive changed during review", StorageReason::StaleInput);
      for (std::size_t i = 0; i < positions.size(); ++i) result.groups[positions[i]] = std::move(measured[i]);
      summary.innerRoots = selected.MinimalRoots();
      result.eligibleCount = plus(result.eligibleCount, summary.eligibleCount);
      result.eligibleBytes = plus(result.eligibleBytes, summary.eligibleBytes);
      result.skippedCount = plus(result.skippedCount, skippedCount);
      for (auto &sample : skipped) if (result.skips.size() < 100) result.skips.push_back(std::move(sample));
      if (result.totalBytes) *result.totalBytes = plus(*result.totalBytes, total);
      result.archives.push_back(std::move(summary));
    } catch (const StorageArchiveError &error) {
      for (const auto position : positions) result.groups[position].reason = {error.code, error.what()};
      if (error.code == StorageReason::Cancelled) { result.cancelled = true; result.complete = false; break; }
    } catch (const std::exception &error) {
      for (const auto position : positions) result.groups[position].reason = {StorageReason::IoError, error.what()};
    }
  }
}
}
