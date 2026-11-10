#include "DownloadPublication.h"
#include "DownloadPublicationPaths.h"
#include "../AtomicFile.h"
#include "../../yoga/lib/nlohmann/json.hpp"
#include <array>
#include <algorithm>
#include <fstream>
#include <optional>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace downloads {
namespace {
using json = nlohmann::json;
using Identity = std::array<std::uint64_t, 3>;
std::optional<Identity> identity(const std::filesystem::path &path) {
#ifdef _WIN32
  const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return {};
  BY_HANDLE_FILE_INFORMATION info{};
  const bool valid = GetFileInformationByHandle(handle, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
  CloseHandle(handle);
  if (!valid) return {};
  return Identity{info.dwVolumeSerialNumber,
      (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1ULL : 0ULL};
#else
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0 || (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode))) return {};
  return Identity{static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino),
                  S_ISDIR(status.st_mode) ? 1ULL : 0ULL};
#endif
}
std::string utf8(const std::filesystem::path &path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}
std::filesystem::path readPath(const json &j, const char *key) {
  const auto value = j.at(key).get<std::string>();
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
bool safeTransaction(const std::filesystem::path &transaction, const std::filesystem::path &parent) {
  if (parent.empty() || transaction.parent_path() != parent || transaction.filename().string().size() != 36) return false;
  std::error_code error;
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(parent, error))) return false;
  error.clear();
  return !std::filesystem::is_symlink(std::filesystem::symlink_status(transaction, error));
}
bool save(const std::filesystem::path &path, const json &record) {
  const auto text = record.dump();
  if (text.size() > 16384) return false;
  const auto operations = atomic_file::privateFileOperations();
  std::string error;
  return atomic_file::writeWithoutBackup(path, std::as_bytes(std::span(text.data(), text.size())), error, &operations);
}
json load(const std::filesystem::path &path) {
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)) || std::filesystem::file_size(path) > 16384)
    throw std::runtime_error("Invalid publication journal.");
  std::ifstream input(path, std::ios::binary);
  return json::parse(input);
}
// Flush copied payloads and directory entries without reading file contents.
// Do not follow preserved symlinks into unrelated library data.
bool syncTree(const std::filesystem::path &path, std::string &error) {
  const auto status = std::filesystem::symlink_status(path);
  if (std::filesystem::is_regular_file(status)) return atomic_file::syncFile(path, error);
  if (std::filesystem::is_symlink(status)) return true;
  if (!std::filesystem::is_directory(status)) { error = "Unsupported publication entry."; return false; }
  for (const auto &entry : std::filesystem::directory_iterator(path))
    if (!syncTree(entry.path(), error)) return false;
  return atomic_file::syncDirectory(path, error);
}
std::vector<std::filesystem::path> readRemovals(const json &record, const std::filesystem::path &destination) {
  std::vector<std::filesystem::path> result;
  auto root = destination.parent_path();
  if (root.filename() == "_archives") root = root.parent_path();
  if (record.contains("removedPaths")) for (const auto &item : record.at("removedPaths")) {
    const auto value = item.get<std::string>();
    const auto path = std::filesystem::path(std::u8string(value.begin(), value.end()));
    if (path != path.lexically_normal() || path == destination ||
        path.filename() == ".asobmashow-transactions" || path.filename() == "_archives" ||
        (path.parent_path() != root && path.parent_path() != root / "_archives"))
      throw std::runtime_error("Invalid removed variant path.");
    result.push_back(path);
  }
  return result;
}
bool renameSync(const std::filesystem::path &from, const std::filesystem::path &to,
                const PublicationJournal::DirectorySync &syncDirectory) {
  std::string error;
  return atomic_file::renameDurably(from, to, error) &&
      syncDirectory(to.parent_path(), error) &&
      (from.parent_path() == to.parent_path() || syncDirectory(from.parent_path(), error));
}
}
PublicationJournal::PublicationJournal(std::filesystem::path journal, std::filesystem::path destination,
    DirectorySync syncDirectory, std::filesystem::path transactionRoot)
    : journal_(std::move(journal)), destination_(std::move(destination)),
      transactionRoot_(std::move(transactionRoot)),
      syncDirectory_(syncDirectory ? std::move(syncDirectory) : atomic_file::syncDirectory) {
  if (transactionRoot_.empty()) {
    auto library = destination_.parent_path();
    if (library.filename() == "_archives") library = library.parent_path();
    transactionRoot_ = publicationTransactionRoot(library);
  }
}
bool PublicationJournal::beginCopy(const std::filesystem::path &transaction) {
  if (!safeTransaction(transaction, transactionRoot_)) return false;
  const auto old = identity(destination_);
  return save(journal_, {{"version", 1}, {"destination", utf8(destination_)},
      {"transaction", utf8(transaction)}, {"phase", "copy"}, {"identity", Identity{}},
      {"oldIdentity", old ? json(*old) : json(nullptr)}});
}
void PublicationJournal::rename(const std::filesystem::path &from, const std::filesystem::path &to,
                                 std::error_code &error) {
  error.clear();
  bool durabilityPending = false;
  try {
    const auto sourceIdentity = identity(from);
    const bool backup = from == destination_ && to.filename() == "backup";
    const bool install = to == destination_ && from.filename() == "commit";
    const bool restore = to == destination_ && from.filename() == "backup";
    const auto transaction = backup ? to.parent_path() : from.parent_path();
    if (!sourceIdentity || !(backup || install || restore) || !safeTransaction(transaction, transactionRoot_)) {
      error = std::make_error_code(std::errc::invalid_argument);
      return;
    }
    if (backup || install) {
      // The private transaction tree and public destination may both be new.
      // Persist both branches through their common ancestor before moving data.
      const auto transactionDirectory = std::filesystem::canonical(transaction);
      const auto destinationDirectory = std::filesystem::canonical(destination_.parent_path());
      auto common = transactionDirectory.parent_path();
      while (common != common.parent_path()) {
        const auto relative = destinationDirectory.lexically_relative(common);
        if (!relative.empty() && *relative.begin() != "..") break;
        common = common.parent_path();
      }
      auto library = destinationDirectory;
      if (library.filename() == "_archives") library = library.parent_path();
      // With in-library staging, BMSSEARCH itself may have just been created.
      // Its entry lives in its parent, which must also be durable.
      if (common == library) common = common.parent_path();
      std::string message;
      for (auto directory : {transactionDirectory, destinationDirectory}) {
        for (;;) {
          if (!syncDirectory_(directory, message))
            throw std::runtime_error("Could not sync publication ancestry: " + message);
          if (directory == common || directory == directory.parent_path()) break;
          directory = directory.parent_path();
        }
      }
    }
    json record = {{"version", 1}, {"destination", utf8(destination_)},
                   {"transaction", utf8(transaction)}, {"phase", backup ? "backup" : install ? "install" : "restore"},
                   {"identity", *sourceIdentity}, {"oldIdentity", nullptr}};
    if (backup) record["oldIdentity"] = *sourceIdentity;
    else if (std::filesystem::exists(journal_)) {
      const auto previous = load(journal_);
      if (readPath(previous, "destination") != destination_ || readPath(previous, "transaction") != transaction)
        throw std::runtime_error("Publication identity changed.");
      record["oldIdentity"] = previous.at("oldIdentity");
      if (previous.contains("removedPaths")) record["removedPaths"] = previous.at("removedPaths");
    }
    if (install) {
      std::string message;
      if (!syncTree(from, message)) throw std::runtime_error("Could not sync publication: " + message);
    } else if (std::filesystem::is_regular_file(from)) {
      std::string message;
      if (!atomic_file::syncFile(from, message)) throw std::runtime_error("Could not sync publication.");
    }
    if (!save(journal_, record)) error = std::make_error_code(std::errc::io_error);
    else if (!renameSync(from, to, syncDirectory_)) {
      if (identity(to) == sourceIdentity) durabilityPending = true;
      else error = std::make_error_code(std::errc::io_error);
    }
  } catch (...) { error = std::make_error_code(std::errc::io_error); }
  // An ordinary rename error permits the legacy publisher to roll back and
  // clean up. After the move, neither success nor that cleanup is safe until
  // directory durability is known. Unwind with the journal and backup intact.
  if (durabilityPending) throw std::runtime_error("Publication directory sync failed. Retry to recover.");
}
bool PublicationJournal::recordRemovals(const std::vector<std::filesystem::path> &paths, std::string &error) {
  try {
    auto record = load(journal_);
    if (readPath(record, "destination") != destination_ || record.at("phase") != "install" ||
        identity(destination_) != record.at("identity").get<Identity>())
      throw std::runtime_error("Publication identity changed.");
    auto removed = readRemovals(record, destination_);
    removed.insert(removed.end(), paths.begin(), paths.end());
    std::sort(removed.begin(), removed.end());
    removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
    record["removedPaths"] = json::array();
    for (const auto &path : removed) record["removedPaths"].push_back(utf8(path));
    (void)readRemovals(record, destination_);
    if (save(journal_, record)) return true;
  } catch (...) {}
  error = "Could not save older download paths before removing them.";
  return false;
}
std::vector<std::filesystem::path> PublicationJournal::removedPaths() const {
  const auto record = load(journal_);
  if (readPath(record, "destination") != destination_) throw std::runtime_error("Publication identity changed.");
  return readRemovals(record, destination_);
}
PublicationRecovery PublicationJournal::recover() {
  try {
    if (!std::filesystem::exists(journal_)) return PublicationRecovery::Retry;
    const auto record = load(journal_);
    const auto transaction = readPath(record, "transaction");
    if (record.at("version") != 1 || readPath(record, "destination") != destination_ ||
        !safeTransaction(transaction, transactionRoot_)) return PublicationRecovery::Conflict;
    const auto phase = record.at("phase").get<std::string>();
    const auto source = record.at("identity").get<Identity>();
    const auto destination = identity(destination_);
    if (phase == "install" && destination == source) {
      std::string message;
      if (!syncDirectory_(destination_.parent_path(), message) ||
          (std::filesystem::exists(transaction) && !syncDirectory_(transaction, message)))
        return PublicationRecovery::Conflict;
      std::error_code error;
      std::filesystem::remove_all(transaction, error);
      return error ? PublicationRecovery::Conflict : PublicationRecovery::Installed;
    }
    const auto old = record.at("oldIdentity").is_null()
        ? std::optional<Identity>{} : std::optional(record.at("oldIdentity").get<Identity>());
    if (old) {
      if (!destination && identity(transaction / "backup") == old) {
        if (!renameSync(transaction / "backup", destination_, syncDirectory_)) return PublicationRecovery::Conflict;
      } else if (destination != old) return PublicationRecovery::Conflict;
      // A previous recovery may have restored the name but failed its fsync.
      std::string message;
      if (!syncDirectory_(destination_.parent_path(), message)) return PublicationRecovery::Conflict;
    } else if (destination) return PublicationRecovery::Conflict;
    if (phase != "copy" && phase != "backup" && phase != "install" && phase != "restore") return PublicationRecovery::Conflict;
    std::error_code error;
    std::filesystem::remove_all(transaction, error);
    if (error) return PublicationRecovery::Conflict;
    std::filesystem::remove(journal_, error);
    return error ? PublicationRecovery::Conflict : PublicationRecovery::Retry;
  } catch (...) { return PublicationRecovery::Conflict; }
}
} // namespace downloads
