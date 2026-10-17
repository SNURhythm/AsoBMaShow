#include "ChartResourceInventory.h"

#include <algorithm>
#include <set>
#include <system_error>
#include <tuple>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace storage_management {
namespace fs = std::filesystem;
using asobmshow::chart_assets::MediaKind;
namespace {
bool checkpoint(const Checkpoint &check) { return !check || check(); }

bool hasPermissions(fs::perms permissions, fs::perms mask) {
  return permissions == fs::perms::unknown || (permissions & mask) != fs::perms::none;
}
bool readableDirectory(fs::perms permissions) {
  return hasPermissions(permissions, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read) &&
         hasPermissions(permissions, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec);
}
bool writable(fs::perms permissions) {
  return hasPermissions(permissions, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write);
}
std::u8string stemKey(const fs::path &path) {
  auto stem = path.stem().u8string();
  for (auto &ch : stem) if (ch >= u8'A' && ch <= u8'Z') ch += 32;
  return stem;
}
void skip(StorageInventory &inventory, const fs::path &path, StorageReason reason,
          std::string detail) {
  inventory.skips.push_back({.source = path, .reason = {reason, std::move(detail)}});
}
} // namespace

bool IsWithinFolder(const fs::path &path, const fs::path &folder) {
  auto child = path.begin();
  for (auto parent = folder.begin(); parent != folder.end(); ++parent, ++child) {
    if (child == path.end() || *child != *parent) return false;
  }
  return true;
}

bool IsSafeLocalPath(const fs::path &path, std::string &error) {
  if (!path.is_absolute()) { error = "Expected an absolute local path"; return false; }
  auto current = path.root_path();
  for (const auto &part : path.relative_path()) {
    if (part == "..") { error = "Parent traversal is unsupported"; return false; }
    current /= part;
    std::error_code ec;
    const auto status = fs::symlink_status(current, ec);
    if (ec || !fs::exists(status) || fs::is_symlink(status)) {
      error = ec ? ec.message() : "Symlink or missing path component";
      return false;
    }
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      error = "Reparse points are unsupported";
      return false;
    }
#endif
  }
  return true;
}

std::optional<FileIdentity> ReadFileIdentity(const fs::path &path, std::string &error) {
  if (!IsSafeLocalPath(path, error)) return {};
  FileIdentity result;
#ifdef _WIN32
  const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) { error = "Cannot inspect file identity"; return {}; }
  BY_HANDLE_FILE_INFORMATION info{};
  const bool ok = GetFileInformationByHandle(handle, &info);
  CloseHandle(handle);
  if (!ok || info.nNumberOfLinks != 1 ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
    error = "Unsupported file type or hard link"; return {};
  }
  result.device = info.dwVolumeSerialNumber;
  result.fileId = (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  result.bytes = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#else
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
    error = "Unsupported file type, hard link, or inaccessible file"; return {};
  }
  result.device = static_cast<std::uint64_t>(info.st_dev);
  result.fileId = static_cast<std::uint64_t>(info.st_ino);
  result.bytes = static_cast<std::uint64_t>(info.st_size);
#endif
  std::error_code ec;
  result.modified = fs::last_write_time(path, ec);
  if (ec) { error = ec.message(); return {}; }
  return result;
}

StorageInventory BuildInventory(const InventoryRequest &request, Checkpoint check,
                                ProgressCallback progress) {
  StorageInventory inventory;
  inventory.request = request;
  inventory.totalBytes = 0;
  std::vector<fs::path> roots;
  std::set<fs::path> unavailable;
  for (const auto &group : request.groups) {
    GroupStorageMeasurement measurement;
    measurement.groupId = group.groupId;
    std::error_code ec;
    auto root = fs::absolute(group.chartPathFallback ? group.path.parent_path() : group.path, ec);
    std::string error;
    if (!group.writableLocal) {
      measurement.reason = {StorageReason::UnsupportedSource, "Extract/import to a writable local folder first"};
    } else if (ec || !IsSafeLocalPath(root, error)) {
      measurement.reason = {StorageReason::UnsafePath, ec ? ec.message() : error};
    } else {
      root = root.lexically_normal();
      while (root != root.root_path() && root.filename().empty()) root = root.parent_path();
      const auto status = fs::status(root, ec);
      if (ec || !fs::is_directory(status) || !readableDirectory(status.permissions()) || !writable(status.permissions())) {
        measurement.reason = {StorageReason::UnsupportedSource, "Folder is not readable and writable"};
      } else {
        roots.push_back(root);
        measurement.totalBytes = 0;
      }
    }
    measurement.root = root;
    if (!measurement.totalBytes) inventory.complete = false;
    inventory.groups.push_back(std::move(measurement));
  }
  std::ranges::sort(roots);
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  std::vector<fs::path> scanned;
  for (const auto &root : roots) {
    if (std::ranges::any_of(scanned, [&](const auto &prior) { return IsWithinFolder(root, prior); })) continue;
    scanned.push_back(root);
    std::vector<fs::path> pending{root};
    while (!pending.empty()) {
      if (!checkpoint(check)) { inventory.cancelled = true; break; }
      auto directory = std::move(pending.back());
      pending.pop_back();
      std::error_code ec;
      const auto permissions = fs::status(directory, ec).permissions();
      if (ec || !readableDirectory(permissions)) {
        unavailable.insert(root); break;
      }
      const auto modified = fs::last_write_time(directory, ec);
      if (ec) { unavailable.insert(root); break; }
      inventory.directories[directory] = modified;
      fs::directory_iterator it(directory, ec), end;
      if (ec) { unavailable.insert(root); break; }
      for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (!checkpoint(check)) { inventory.cancelled = true; break; }
        const auto path = it->path();
        inventory.entries.insert(path);
        const auto status = it->symlink_status(ec);
        if (ec) break;
        std::string error;
        if (fs::is_symlink(status) || !IsSafeLocalPath(path, error)) {
          skip(inventory, path, StorageReason::UnsafePath, "Symlink or unsafe path");
        } else if (fs::is_directory(status)) {
          pending.push_back(path);
        } else if (auto identity = ReadFileIdentity(path, error)) {
          inventory.files.emplace(path, *identity);
        } else {
          skip(inventory, path, StorageReason::UnsafePath, std::move(error));
        }
      }
      if (ec) { unavailable.insert(root); break; }
      if (inventory.cancelled) break;
    }
    if (inventory.cancelled) break;
  }
  for (auto &group : inventory.groups) {
    if (inventory.cancelled || std::ranges::any_of(unavailable, [&](const auto &root) {
          return IsWithinFolder(group.root, root) || IsWithinFolder(root, group.root);
        })) {
      group.totalBytes.reset();
      group.reason = {inventory.cancelled ? StorageReason::Cancelled : StorageReason::IncompleteInventory,
                      "Folder traversal did not complete"};
      inventory.complete = false;
    }
  }

  std::map<std::tuple<fs::path, std::u8string, bool>, std::size_t> stems;
  for (const auto &path : inventory.entries) {
    if (inventory.directories.contains(path)) continue;
    const auto kind = asobmshow::chart_assets::ClassifyMediaPath(path);
    if (kind != MediaKind::Other)
      ++stems[{path.parent_path(), stemKey(path), kind == MediaKind::Audio}];
  }
  for (const auto &[path, identity] : inventory.files) {
    if (!checkpoint(check)) { inventory.cancelled = true; inventory.complete = false; break; }
    const auto kind = asobmshow::chart_assets::ClassifyMediaPath(path);
    std::vector<std::string> groups;
    for (auto &group : inventory.groups) {
      if (!group.totalBytes || !IsWithinFolder(path, group.root)) continue;
      groups.push_back(group.groupId);
      *group.totalBytes += identity.bytes;
      if (kind == MediaKind::Audio) group.audioBytes += identity.bytes;
      if (kind == MediaKind::Video) group.videoBytes += identity.bytes;
      if (kind == MediaKind::Image) group.imageBytes += identity.bytes;
    }
    if (groups.empty()) continue;
    *inventory.totalBytes += identity.bytes;
    AssetCandidate candidate{.source = path, .destination = path, .kind = kind,
                             .groups = std::move(groups), .identity = identity};
    const auto extension = asobmshow::chart_assets::LowerMediaExtension(path);
    if (!IsValid(request.policy)) continue;
    if (kind == MediaKind::Audio && extension == "wav" && request.policy.audio == AudioAction::LosslessFlac) {
      candidate.operation = AssetOperation::LosslessFlac;
      candidate.destination.replace_extension("flac");
    } else if (kind == MediaKind::Audio && (extension == "wav" || extension == "flac") &&
               request.policy.audio == AudioAction::LossyVorbis) {
      candidate.operation = AssetOperation::LossyVorbis;
      candidate.destination.replace_extension("ogg");
    } else if (kind == MediaKind::Video && request.policy.bga != BgaAction::Keep) {
      candidate.operation = request.policy.bga == BgaAction::Delete ? AssetOperation::Delete : AssetOperation::CompressH264;
      if (extension != "mp4") candidate.destination.replace_extension("mp4");
    } else continue;
    std::error_code ec;
    bool destinationBlocked = false;
    if (candidate.operation != AssetOperation::Delete && candidate.destination != path) {
      const auto status = fs::symlink_status(candidate.destination, ec);
      destinationBlocked = fs::exists(status) ||
                           (ec && ec != std::errc::no_such_file_or_directory);
      ec.clear();
    }
    if (!writable(fs::status(path.parent_path(), ec).permissions()) || ec) {
      skip(inventory, path, StorageReason::UnsupportedSource, "Parent folder is not writable");
    } else if (candidate.operation != AssetOperation::Delete &&
               (destinationBlocked || stems[{path.parent_path(), stemKey(path), kind == MediaKind::Audio}] > 1)) {
      skip(inventory, path, StorageReason::Collision, "Same-stem media alternatives are ambiguous");
    } else inventory.candidates.push_back(std::move(candidate));
  }
  if (inventory.cancelled) {
    inventory.candidates.clear();
    for (auto &group : inventory.groups) group.totalBytes.reset();
  }
  if (!inventory.complete) inventory.totalBytes.reset();
  if (progress) progress({.phase = StoragePhase::Inventory,
                          .filesDone = inventory.files.size(), .filesTotal = inventory.files.size()});
  return inventory;
}

bool RevalidateInventory(const StorageInventory &inventory, Checkpoint check, std::string &error) {
  if (inventory.cancelled) { error = "Inventory cancelled"; return false; }
  for (const auto &[path, expected] : inventory.files) {
    if (!checkpoint(check)) { error = "Cancelled"; return false; }
    const auto current = ReadFileIdentity(path, error);
    if (!current || *current != expected) { error = "File changed since inventory"; return false; }
  }
  for (const auto &[path, expected] : inventory.directories) {
    if (!checkpoint(check)) { error = "Cancelled"; return false; }
    if (!IsSafeLocalPath(path, error)) return false;
    std::error_code ec;
    if (fs::last_write_time(path, ec) != expected || ec) {
      error = "Directory changed since inventory"; return false;
    }
  }
  return true;
}

} // namespace storage_management
