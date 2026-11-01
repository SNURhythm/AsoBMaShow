#include "DownloadPublication.h"
#include "../AtomicFile.h"
#include "../../yoga/lib/nlohmann/json.hpp"
#include <array>
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
bool safeTransaction(const std::filesystem::path &transaction, const std::filesystem::path &destination) {
  auto library = destination.parent_path();
  if (library.filename() == "_archives") library = library.parent_path();
  const auto parent = library / ".asobmashow-transactions";
  if (transaction.parent_path() != parent || transaction.filename().string().size() != 36) return false;
  std::error_code error;
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(parent, error))) return false;
  error.clear();
  return !std::filesystem::is_symlink(std::filesystem::symlink_status(transaction, error));
}
bool save(const std::filesystem::path &path, const json &record) {
  const auto text = record.dump();
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
bool renameSync(const std::filesystem::path &from, const std::filesystem::path &to) {
  std::string error;
  return atomic_file::renameDurably(from, to, error) &&
      atomic_file::syncDirectory(to.parent_path(), error) &&
      (from.parent_path() == to.parent_path() || atomic_file::syncDirectory(from.parent_path(), error));
}
}
PublicationJournal::PublicationJournal(std::filesystem::path journal, std::filesystem::path destination)
    : journal_(std::move(journal)), destination_(std::move(destination)) {}
bool PublicationJournal::beginCopy(const std::filesystem::path &transaction) {
  if (!safeTransaction(transaction, destination_)) return false;
  const auto old = identity(destination_);
  return save(journal_, {{"version", 1}, {"destination", utf8(destination_)},
      {"transaction", utf8(transaction)}, {"phase", "copy"}, {"identity", Identity{}},
      {"oldIdentity", old ? json(*old) : json(nullptr)}});
}
void PublicationJournal::rename(const std::filesystem::path &from, const std::filesystem::path &to,
                                 std::error_code &error) {
  error.clear();
  try {
    const auto sourceIdentity = identity(from);
    const bool backup = from == destination_ && to.filename() == "backup";
    const bool install = to == destination_ && from.filename() == "commit";
    const bool restore = to == destination_ && from.filename() == "backup";
    const auto transaction = backup ? to.parent_path() : from.parent_path();
    if (!sourceIdentity || !(backup || install || restore) || !safeTransaction(transaction, destination_)) {
      error = std::make_error_code(std::errc::invalid_argument);
      return;
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
    }
    if (std::filesystem::is_regular_file(from)) {
      std::string message;
      if (!atomic_file::syncFile(from, message)) throw std::runtime_error("Could not sync publication.");
    }
    // A post-rename directory-sync failure must not tell the legacy publisher
    // that the rename never happened: it could discard the only old backup.
    if (!save(journal_, record) || (!renameSync(from, to) && identity(to) != sourceIdentity))
      error = std::make_error_code(std::errc::io_error);
  } catch (...) { error = std::make_error_code(std::errc::io_error); }
}
PublicationRecovery PublicationJournal::recover() {
  try {
    if (!std::filesystem::exists(journal_)) return PublicationRecovery::Retry;
    const auto record = load(journal_);
    const auto transaction = readPath(record, "transaction");
    if (record.at("version") != 1 || readPath(record, "destination") != destination_ ||
        !safeTransaction(transaction, destination_)) return PublicationRecovery::Conflict;
    const auto phase = record.at("phase").get<std::string>();
    const auto source = record.at("identity").get<Identity>();
    const auto destination = identity(destination_);
    if (phase == "install" && destination == source) {
      std::error_code ignored;
      std::filesystem::remove_all(transaction, ignored);
      return PublicationRecovery::Installed;
    }
    const auto old = record.at("oldIdentity").is_null()
        ? std::optional<Identity>{} : std::optional(record.at("oldIdentity").get<Identity>());
    if (old) {
      if (!destination && identity(transaction / "backup") == old) {
        if (!renameSync(transaction / "backup", destination_)) return PublicationRecovery::Conflict;
      } else if (destination != old) return PublicationRecovery::Conflict;
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
