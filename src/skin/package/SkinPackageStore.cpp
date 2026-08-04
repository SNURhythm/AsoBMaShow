#include "SkinPackageStore.h"

#include "../../AtomicFile.h"
#include "../../FileChecksum.h"
#include "../../VersionedJson.h"
#include "SkinPathPolicy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace skin {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

struct PublicationJournal {
  std::string operationId;
  std::string phase;
  SkinPackageId package;
  std::string destinationDirectory;
  std::string visibleStagingToken;
  std::string visibleBackupToken;
  bool oldPresent = true;
  std::string oldTreeDigest;
  std::string newTreeDigest;
  std::string oldRevisionDigest;
  std::string newRevisionDigest;
  std::string revisionStagingToken;
  std::string catalogFileName;
  std::string catalogStagingToken;
  std::string catalogBackupToken;
  std::uint64_t oldCatalogGeneration = 0;
  std::uint64_t newCatalogGeneration = 0;
  std::uint64_t oldSourceGeneration = 0;
  std::uint64_t newSourceGeneration = 0;
  std::string oldCatalogDigest;
  std::string newCatalogDigest;
};

struct RemovalJournal {
  std::string operationId;
  SkinPackageId package;
  std::string retainedToken;
  std::string oldTreeDigest;
  std::string catalogStagingToken;
  std::string catalogBackupToken;
  std::uint64_t oldCatalogGeneration = 0;
  std::uint64_t newCatalogGeneration = 0;
  std::uint64_t oldSourceGeneration = 0;
  std::uint64_t newSourceGeneration = 0;
  std::string oldCatalogDigest;
  std::string newCatalogDigest;
};

SkinDiagnostic storeDiagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

bool getString(const Json &object, std::string_view name, std::string &value) {
  const auto iterator = object.find(std::string(name));
  if (iterator == object.end() || !iterator->is_string()) {
    return false;
  }
  value = iterator->get<std::string>();
  return true;
}

bool safeToken(std::string_view token) {
  return !token.empty() && token != "." && token != ".." &&
         token.find('/') == std::string_view::npos &&
         token.find('\\') == std::string_view::npos &&
         token.size() <= SkinPackagePolicy::maxPathBytes;
}

bool lowercaseSha256(std::string_view digest) {
  return digest.size() == 64 &&
         std::ranges::all_of(digest, [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

std::optional<PublicationJournal>
loadJournal(const fs::path &path, std::vector<SkinDiagnostic> &diagnostics) {
  const auto loaded = versioned_json::loadAndMigrate(path, 1, {});
  if (loaded.status == versioned_json::LoadStatus::Missing) {
    return std::nullopt;
  }
  if (loaded.status != versioned_json::LoadStatus::Loaded) {
    diagnostics.push_back(
        storeDiagnostic("skin_package_journal_invalid",
                        "skin package publication journal is malformed"));
    return std::nullopt;
  }
  const Json &root = loaded.document;
  const auto package = root.find("package");
  const auto visible = root.find("visible");
  const auto revision = root.find("revision");
  const auto catalog = root.find("catalog");
  std::string operation;
  PublicationJournal result;
  std::string packageDirectory;
  std::string packageCollisionKey;
  if (!getString(root, "operation", operation) ||
      operation != "replace-package" ||
      !getString(root, "operationId", result.operationId) ||
      !safeToken(result.operationId) ||
      !getString(root, "phase", result.phase) || package == root.end() ||
      !package->is_object() ||
      !getString(*package, "directoryName", packageDirectory) ||
      !getString(*package, "collisionKey", packageCollisionKey) ||
      visible == root.end() || !visible->is_object() ||
      revision == root.end() || !revision->is_object() ||
      catalog == root.end() || !catalog->is_object() ||
      !getString(*visible, "destinationDirectory",
                 result.destinationDirectory) ||
      !getString(*visible, "stagingToken", result.visibleStagingToken) ||
      !getString(*visible, "backupToken", result.visibleBackupToken) ||
      !getString(*visible, "newTreeDigest", result.newTreeDigest) ||
      !getString(*revision, "newDigest", result.newRevisionDigest) ||
      !getString(*revision, "stagingToken", result.revisionStagingToken) ||
      !getString(*catalog, "fileName", result.catalogFileName) ||
      !getString(*catalog, "stagingToken", result.catalogStagingToken) ||
      !getString(*catalog, "backupToken", result.catalogBackupToken) ||
      !getString(*catalog, "oldSnapshotDigest", result.oldCatalogDigest) ||
      !getString(*catalog, "newSnapshotDigest", result.newCatalogDigest)) {
    diagnostics.push_back(storeDiagnostic(
        "skin_package_journal_invalid",
        "skin package publication journal has invalid typed fields"));
    return std::nullopt;
  }
  const auto oldGeneration = catalog->find("oldGeneration");
  const auto newGeneration = catalog->find("newGeneration");
  const auto oldSourceGeneration = catalog->find("oldSourceGeneration");
  const auto newSourceGeneration = catalog->find("newSourceGeneration");
  const auto oldPresent = visible->find("oldPresent");
  const auto normalized = normalizePackageId(packageDirectory);
  if (!normalized.package ||
      normalized.package->collisionKey != packageCollisionKey ||
      result.destinationDirectory != packageDirectory ||
      !safeToken(result.visibleStagingToken) ||
      !safeToken(result.visibleBackupToken) ||
      !safeToken(result.revisionStagingToken) ||
      !safeToken(result.catalogStagingToken) ||
      !safeToken(result.catalogBackupToken) ||
      result.catalogFileName != "catalog.json" ||
      oldPresent == visible->end() || !oldPresent->is_boolean() ||
      oldGeneration == catalog->end() || !oldGeneration->is_number_unsigned() ||
      newGeneration == catalog->end() || !newGeneration->is_number_unsigned() ||
      oldSourceGeneration == catalog->end() ||
      !oldSourceGeneration->is_number_unsigned() ||
      newSourceGeneration == catalog->end() ||
      !newSourceGeneration->is_number_unsigned() ||
      !lowercaseSha256(result.newTreeDigest) ||
      result.newTreeDigest != result.newRevisionDigest ||
      !lowercaseSha256(result.oldCatalogDigest) ||
      !lowercaseSha256(result.newCatalogDigest)) {
    diagnostics.push_back(storeDiagnostic(
        "skin_package_journal_invalid",
        "skin package publication journal identity is invalid"));
    return std::nullopt;
  }
  result.package = std::move(*normalized.package);
  result.oldPresent = oldPresent->get<bool>();
  if (result.oldPresent &&
      (!getString(*visible, "oldTreeDigest", result.oldTreeDigest) ||
       !lowercaseSha256(result.oldTreeDigest))) {
    diagnostics.push_back(storeDiagnostic(
        "skin_package_journal_invalid",
        "skin package publication journal old identity is invalid"));
    return std::nullopt;
  }
  result.oldCatalogGeneration = oldGeneration->get<std::uint64_t>();
  result.newCatalogGeneration = newGeneration->get<std::uint64_t>();
  result.oldSourceGeneration = oldSourceGeneration->get<std::uint64_t>();
  result.newSourceGeneration = newSourceGeneration->get<std::uint64_t>();
  return result;
}

std::optional<RemovalJournal>
loadRemovalJournal(const fs::path &path,
                   std::vector<SkinDiagnostic> &diagnostics) {
  const auto loaded = versioned_json::loadAndMigrate(path, 1, {});
  if (loaded.status == versioned_json::LoadStatus::Missing) {
    return std::nullopt;
  }
  if (loaded.status != versioned_json::LoadStatus::Loaded ||
      !loaded.document.is_object()) {
    diagnostics.push_back(
        storeDiagnostic("skin_package_removal_journal_invalid",
                        "skin package removal journal is malformed"));
    return std::nullopt;
  }
  try {
    const Json &root = loaded.document;
    const auto package = root.find("package");
    const auto catalog = root.find("catalog");
    std::string operation;
    std::string directory;
    std::string collision;
    RemovalJournal result;
    if (!getString(root, "operation", operation) ||
        operation != "remove-package" ||
        !getString(root, "operationId", result.operationId) ||
        !safeToken(result.operationId) || package == root.end() ||
        !package->is_object() ||
        !getString(*package, "directoryName", directory) ||
        !getString(*package, "collisionKey", collision) ||
        !getString(root, "retainedToken", result.retainedToken) ||
        !safeToken(result.retainedToken) ||
        !getString(root, "oldTreeDigest", result.oldTreeDigest) ||
        !lowercaseSha256(result.oldTreeDigest) || catalog == root.end() ||
        !catalog->is_object() ||
        !getString(*catalog, "stagingToken", result.catalogStagingToken) ||
        !getString(*catalog, "backupToken", result.catalogBackupToken) ||
        !safeToken(result.catalogStagingToken) ||
        !safeToken(result.catalogBackupToken) ||
        !getString(*catalog, "oldSnapshotDigest", result.oldCatalogDigest) ||
        !getString(*catalog, "newSnapshotDigest", result.newCatalogDigest) ||
        !lowercaseSha256(result.oldCatalogDigest) ||
        !lowercaseSha256(result.newCatalogDigest)) {
      throw std::invalid_argument("invalid removal journal");
    }
    const auto normalized = normalizePackageId(directory);
    const auto oldGeneration = catalog->find("oldGeneration");
    const auto newGeneration = catalog->find("newGeneration");
    const auto oldSourceGeneration = catalog->find("oldSourceGeneration");
    const auto newSourceGeneration = catalog->find("newSourceGeneration");
    if (!normalized.package || normalized.package->collisionKey != collision ||
        oldGeneration == catalog->end() ||
        !oldGeneration->is_number_unsigned() ||
        newGeneration == catalog->end() ||
        !newGeneration->is_number_unsigned() ||
        oldSourceGeneration == catalog->end() ||
        !oldSourceGeneration->is_number_unsigned() ||
        newSourceGeneration == catalog->end() ||
        !newSourceGeneration->is_number_unsigned()) {
      throw std::invalid_argument("invalid removal identity");
    }
    result.package = std::move(*normalized.package);
    result.oldCatalogGeneration = oldGeneration->get<std::uint64_t>();
    result.newCatalogGeneration = newGeneration->get<std::uint64_t>();
    result.oldSourceGeneration = oldSourceGeneration->get<std::uint64_t>();
    result.newSourceGeneration = newSourceGeneration->get<std::uint64_t>();
    return result;
  } catch (...) {
    diagnostics.push_back(storeDiagnostic(
        "skin_package_removal_journal_invalid",
        "skin package removal journal has invalid typed fields"));
    return std::nullopt;
  }
}

bool treeDigestMatches(const fs::path &path, const SkinPackageId &package,
                       std::string_view expected, const SkinStorageRoots &roots,
                       const SkinAliasDetector &aliases) {
  std::error_code error;
  if (!fs::is_directory(path, error) || error) {
    return false;
  }
  SkinTreeSnapshotter snapshotter(roots, aliases);
  auto snapshot = snapshotter.snapshot(path, package, {}, {});
  return snapshot.prepared &&
         snapshot.prepared->revision().lowercaseSha256 == expected;
}

std::optional<std::string>
treeDigest(const fs::path &path, const SkinPackageId &package,
           const SkinStorageRoots &roots, const SkinAliasDetector &aliases,
           std::stop_token stop, std::vector<SkinDiagnostic> &diagnostics) {
  SkinTreeSnapshotter snapshotter(roots, aliases);
  auto snapshot = snapshotter.snapshot(path, package, stop, {});
  diagnostics.insert(diagnostics.end(),
                     std::make_move_iterator(snapshot.diagnostics.begin()),
                     std::make_move_iterator(snapshot.diagnostics.end()));
  if (!snapshot.prepared) {
    return std::nullopt;
  }
  return snapshot.prepared->revision().lowercaseSha256;
}

struct TreeMetadataRecord {
  std::string relativePath;
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::uint64_t size = 0;
  std::int64_t modifiedSeconds = 0;
  std::int64_t modifiedNanoseconds = 0;
  std::int64_t changedSeconds = 0;
  std::int64_t changedNanoseconds = 0;
  bool directory = false;
  auto operator<=>(const TreeMetadataRecord &) const = default;
};

std::optional<std::vector<TreeMetadataRecord>>
treeMetadataManifest(const fs::path &root, const SkinAliasDetector &aliases) {
  std::vector<TreeMetadataRecord> records;
  const auto append = [&](const fs::path &path, std::string relative,
                          std::vector<TreeMetadataRecord> &target) -> bool {
    if (aliases.inspectNoFollow(path) != SkinRejectedLinkKind::None) {
      return false;
    }
#if defined(_WIN32)
    HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION information{};
    FILE_ATTRIBUTE_TAG_INFO tags{};
    FILE_BASIC_INFO basic{};
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &information) ||
        !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tags,
                                      sizeof(tags)) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                      sizeof(basic)) ||
        (tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
      }
      return false;
    }
    CloseHandle(handle);
    target.push_back(TreeMetadataRecord{
        .relativePath = std::move(relative),
        .device = information.dwVolumeSerialNumber,
        .inode =
            (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) |
            information.nFileIndexLow,
        .size = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
                information.nFileSizeLow,
        .modifiedSeconds = information.ftLastWriteTime.dwHighDateTime,
        .modifiedNanoseconds = information.ftLastWriteTime.dwLowDateTime,
        .changedSeconds = basic.ChangeTime.HighPart,
        .changedNanoseconds = basic.ChangeTime.LowPart,
        .directory = (tags.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
#else
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0 ||
        (!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode))) {
      return false;
    }
    target.push_back(TreeMetadataRecord{
        .relativePath = std::move(relative),
        .device = static_cast<std::uint64_t>(status.st_dev),
        .inode = static_cast<std::uint64_t>(status.st_ino),
        .size = S_ISREG(status.st_mode)
                    ? static_cast<std::uint64_t>(status.st_size)
                    : 0,
#if defined(__APPLE__)
        .modifiedSeconds = status.st_mtimespec.tv_sec,
        .modifiedNanoseconds = status.st_mtimespec.tv_nsec,
        .changedSeconds = status.st_ctimespec.tv_sec,
        .changedNanoseconds = status.st_ctimespec.tv_nsec,
#else
        .modifiedSeconds = status.st_mtim.tv_sec,
        .modifiedNanoseconds = status.st_mtim.tv_nsec,
        .changedSeconds = status.st_ctim.tv_sec,
        .changedNanoseconds = status.st_ctim.tv_nsec,
#endif
        .directory = S_ISDIR(status.st_mode)});
#endif
    return true;
  };
  if (!append(root, {}, records) || !records.front().directory) {
    return std::nullopt;
  }
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; ++iterator) {
    const fs::path relative = iterator->path().lexically_relative(root);
    const auto utf8 = relative.generic_u8string();
    const std::string relativeText(reinterpret_cast<const char *>(utf8.data()),
                                   utf8.size());
    if (!append(iterator->path(), relativeText, records) ||
        records.size() > SkinPackagePolicy::maxFiles + 1) {
      return std::nullopt;
    }
  }
  if (error) {
    return std::nullopt;
  }
  std::ranges::sort(records, {}, &TreeMetadataRecord::relativePath);
  return records;
}

#if defined(_WIN32)
bool ensureDirectoryNoFollow(const fs::path &directory) {
  try {
    std::error_code error;
    const fs::path absolute = fs::absolute(directory, error).lexically_normal();
    if (error || !absolute.is_absolute() || absolute.root_path().empty()) {
      return false;
    }
    fs::path current = absolute.root_path();
    std::vector<HANDLE> retained;
    HANDLE root = CreateFileW(
        current.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (root == INVALID_HANDLE_VALUE) {
      return false;
    }
    retained.push_back(root);
    const fs::path relative = absolute.lexically_relative(current);
    for (const fs::path &component : relative) {
      if (component.empty() || component == "." || component == "..") {
        for (HANDLE handle : retained) {
          CloseHandle(handle);
        }
        return false;
      }
      current /= component;
      if (!CreateDirectoryW(current.c_str(), nullptr)) {
        const DWORD createError = GetLastError();
        if (createError != ERROR_ALREADY_EXISTS) {
          for (HANDLE retainedHandle : retained) {
            CloseHandle(retainedHandle);
          }
          return false;
        }
      }
      HANDLE handle = CreateFileW(
          current.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      FILE_ATTRIBUTE_TAG_INFO attributes{};
      const bool valid =
          handle != INVALID_HANDLE_VALUE &&
          GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                       &attributes, sizeof(attributes)) &&
          (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
          (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      if (!valid) {
        if (handle != INVALID_HANDLE_VALUE) {
          CloseHandle(handle);
        }
        for (HANDLE retainedHandle : retained) {
          CloseHandle(retainedHandle);
        }
        return false;
      }
      retained.push_back(handle);
    }
    for (HANDLE handle : retained) {
      CloseHandle(handle);
    }
    return true;
  } catch (...) {
    return false;
  }
}

class RetainedTreeCapability {
public:
  RetainedTreeCapability(const RetainedTreeCapability &) = delete;
  RetainedTreeCapability &operator=(const RetainedTreeCapability &) = delete;
  RetainedTreeCapability(RetainedTreeCapability &&other) noexcept
      : path_(std::move(other.path_)), parents_(std::move(other.parents_)),
        root_(std::exchange(other.root_, INVALID_HANDLE_VALUE)),
        existed_(other.existed_) {}
  ~RetainedTreeCapability() {
    if (root_ != INVALID_HANDLE_VALUE) {
      CloseHandle(root_);
    }
    closeAll(parents_);
  }

  static std::optional<RetainedTreeCapability> issue(const fs::path &path) {
    std::vector<HANDLE> parents;
    if (!openDirectoryChain(path.parent_path(), parents)) {
      const DWORD attributes = GetFileAttributesW(path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES &&
          (GetLastError() == ERROR_FILE_NOT_FOUND ||
           GetLastError() == ERROR_PATH_NOT_FOUND)) {
        return RetainedTreeCapability(path, {}, INVALID_HANDLE_VALUE, false);
      }
      return std::nullopt;
    }
    HANDLE root = CreateFileW(
        path.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (root == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return RetainedTreeCapability(path, std::move(parents), root, false);
      }
      closeAll(parents);
      return std::nullopt;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(root, FileAttributeTagInfo, &attributes,
                                      sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      CloseHandle(root);
      closeAll(parents);
      return std::nullopt;
    }
    return RetainedTreeCapability(path, std::move(parents), root, true);
  }

  bool existed() const noexcept { return existed_; }
  const fs::path &currentPath() const noexcept { return path_; }
  bool matchesIssuedIdentity() const noexcept {
    if (!existed_) {
      HANDLE probe =
          CreateFileW(path_.c_str(), FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        return false;
      }
      return GetLastError() == ERROR_FILE_NOT_FOUND ||
             GetLastError() == ERROR_PATH_NOT_FOUND;
    }
    HANDLE probe = CreateFileW(
        path_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION retainedIdentity{};
    BY_HANDLE_FILE_INFORMATION pathIdentity{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    const bool matches =
        root_ != INVALID_HANDLE_VALUE && probe != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(root_, &retainedIdentity) &&
        GetFileInformationByHandle(probe, &pathIdentity) &&
        GetFileInformationByHandleEx(probe, FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) &&
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        retainedIdentity.dwVolumeSerialNumber ==
            pathIdentity.dwVolumeSerialNumber &&
        retainedIdentity.nFileIndexHigh == pathIdentity.nFileIndexHigh &&
        retainedIdentity.nFileIndexLow == pathIdentity.nFileIndexLow;
    if (probe != INVALID_HANDLE_VALUE) {
      CloseHandle(probe);
    }
    return matches;
  }

  bool renameTo(const fs::path &destination) noexcept {
    try {
      if (!existed_ || !matchesIssuedIdentity()) {
        return false;
      }
      std::vector<HANDLE> destinationParents;
      if (!openDirectoryChain(destination.parent_path(), destinationParents)) {
        return false;
      }
      fs::path retainedDestination = destination;
      const std::wstring leaf = retainedDestination.filename().native();
      const std::size_t leafBytes = leaf.size() * sizeof(wchar_t);
      std::vector<std::byte> storage(offsetof(FILE_RENAME_INFO, FileName) +
                                     leafBytes);
      auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
      rename->ReplaceIfExists = FALSE;
      rename->RootDirectory = destinationParents.back();
      rename->FileNameLength = static_cast<DWORD>(leafBytes);
      std::memcpy(rename->FileName, leaf.data(), leafBytes);
      const bool renamed =
          SetFileInformationByHandle(root_, FileRenameInfo, rename,
                                     static_cast<DWORD>(storage.size())) != 0;
      if (!renamed) {
        closeAll(destinationParents);
        return false;
      }
      HANDLE destinationProbe = CreateFileW(
          retainedDestination.c_str(), FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      BY_HANDLE_FILE_INFORMATION retainedIdentity{};
      BY_HANDLE_FILE_INFORMATION destinationIdentity{};
      FILE_ATTRIBUTE_TAG_INFO destinationTags{};
      const bool destinationMatches =
          destinationProbe != INVALID_HANDLE_VALUE &&
          GetFileInformationByHandle(root_, &retainedIdentity) &&
          GetFileInformationByHandle(destinationProbe, &destinationIdentity) &&
          GetFileInformationByHandleEx(destinationProbe, FileAttributeTagInfo,
                                       &destinationTags,
                                       sizeof(destinationTags)) &&
          (destinationTags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ==
              0 &&
          retainedIdentity.dwVolumeSerialNumber ==
              destinationIdentity.dwVolumeSerialNumber &&
          retainedIdentity.nFileIndexHigh ==
              destinationIdentity.nFileIndexHigh &&
          retainedIdentity.nFileIndexLow == destinationIdentity.nFileIndexLow;
      if (destinationProbe != INVALID_HANDLE_VALUE) {
        CloseHandle(destinationProbe);
      }
      closeAll(parents_);
      parents_ = std::move(destinationParents);
      path_ = std::move(retainedDestination);
      return destinationMatches;
    } catch (...) {
      return false;
    }
  }

  bool removeTreeExact() noexcept {
    try {
      if (!existed_ || !matchesIssuedIdentity() ||
          !clearDirectoryExact(path_, root_) || !markDelete(root_)) {
        return false;
      }
      CloseHandle(root_);
      root_ = INVALID_HANDLE_VALUE;
      existed_ = false;
      const DWORD attributes = GetFileAttributesW(path_.c_str());
      const DWORD error = GetLastError();
      return attributes == INVALID_FILE_ATTRIBUTES &&
             (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
    } catch (...) {
      return false;
    }
  }

private:
  RetainedTreeCapability(fs::path path, std::vector<HANDLE> parents,
                         HANDLE root, bool existed)
      : path_(std::move(path)), parents_(std::move(parents)), root_(root),
        existed_(existed) {}

  static void closeAll(std::vector<HANDLE> &handles) noexcept {
    for (HANDLE handle : handles) {
      if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
      }
    }
    handles.clear();
  }

  static bool openDirectoryChain(const fs::path &path,
                                 std::vector<HANDLE> &handles) {
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error).lexically_normal();
    if (error || !absolute.is_absolute()) {
      return false;
    }
    fs::path current = absolute.root_path();
    const fs::path relative = absolute.lexically_relative(current);
    HANDLE root = CreateFileW(
        current.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (root == INVALID_HANDLE_VALUE) {
      return false;
    }
    handles.push_back(root);
    for (const fs::path &component : relative) {
      current /= component;
      HANDLE next = CreateFileW(
          current.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      FILE_ATTRIBUTE_TAG_INFO attributes{};
      if (next == INVALID_HANDLE_VALUE ||
          !GetFileInformationByHandleEx(next, FileAttributeTagInfo, &attributes,
                                        sizeof(attributes)) ||
          (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
          (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        if (next != INVALID_HANDLE_VALUE) {
          CloseHandle(next);
        }
        closeAll(handles);
        return false;
      }
      handles.push_back(next);
    }
    return true;
  }

  static bool markDelete(HANDLE handle) noexcept {
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                     sizeof(basic))) {
      basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
      if (basic.FileAttributes == 0) {
        basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
      }
      (void)SetFileInformationByHandle(handle, FileBasicInfo, &basic,
                                       sizeof(basic));
    }
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                      sizeof(disposition)) != 0;
  }

  static bool clearDirectoryExact(const fs::path &path,
                                  HANDLE retainedDirectory) {
    FILE_ATTRIBUTE_TAG_INFO retainedTags{};
    if (retainedDirectory == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandleEx(retainedDirectory, FileAttributeTagInfo,
                                      &retainedTags, sizeof(retainedTags)) ||
        (retainedTags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (retainedTags.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return false;
    }
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((path / L"*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
      return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    bool cleared = true;
    do {
      const std::wstring_view name(entry.cFileName);
      if (name == L"." || name == L"..") {
        continue;
      }
      const fs::path childPath = path / entry.cFileName;
      HANDLE child = CreateFileW(
          childPath.c_str(),
          DELETE | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      FILE_ATTRIBUTE_TAG_INFO childTags{};
      if (child == INVALID_HANDLE_VALUE ||
          !GetFileInformationByHandleEx(child, FileAttributeTagInfo, &childTags,
                                        sizeof(childTags))) {
        if (child != INVALID_HANDLE_VALUE) {
          CloseHandle(child);
        }
        cleared = false;
        continue;
      }
      const bool directory =
          (childTags.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      const bool reparse =
          (childTags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
      const bool childCleared =
          (reparse || !directory || clearDirectoryExact(childPath, child)) &&
          markDelete(child);
      CloseHandle(child);
      if (!childCleared) {
        cleared = false;
      }
    } while (FindNextFileW(search, &entry));
    const DWORD enumerationError = GetLastError();
    FindClose(search);
    return cleared && enumerationError == ERROR_NO_MORE_FILES;
  }

  fs::path path_;
  std::vector<HANDLE> parents_;
  HANDLE root_ = INVALID_HANDLE_VALUE;
  bool existed_ = false;
};
#else
class UniqueDirectoryDescriptor {
public:
  UniqueDirectoryDescriptor() = default;
  explicit UniqueDirectoryDescriptor(int value) : value_(value) {}
  UniqueDirectoryDescriptor(const UniqueDirectoryDescriptor &) = delete;
  UniqueDirectoryDescriptor &
  operator=(const UniqueDirectoryDescriptor &) = delete;
  UniqueDirectoryDescriptor(UniqueDirectoryDescriptor &&other) noexcept
      : value_(std::exchange(other.value_, -1)) {}
  UniqueDirectoryDescriptor &
  operator=(UniqueDirectoryDescriptor &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.value_, -1));
    }
    return *this;
  }
  ~UniqueDirectoryDescriptor() { reset(); }
  int get() const noexcept { return value_; }
  explicit operator bool() const noexcept { return value_ >= 0; }

private:
  void reset(int value = -1) noexcept {
    if (value_ >= 0) {
      ::close(value_);
    }
    value_ = value;
  }
  int value_ = -1;
};

std::optional<UniqueDirectoryDescriptor>
openDirectoryNoFollow(const fs::path &directory) {
  std::error_code error;
  const fs::path absolute = fs::absolute(directory, error).lexically_normal();
  if (error || !absolute.is_absolute() || absolute.root_path().empty()) {
    return std::nullopt;
  }
  UniqueDirectoryDescriptor current(
      ::open(absolute.root_path().c_str(),
             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!current) {
    return std::nullopt;
  }
  const fs::path relative = absolute.lexically_relative(absolute.root_path());
  for (const fs::path &component : relative) {
    if (component.empty() || component == "." || component == "..") {
      return std::nullopt;
    }
    UniqueDirectoryDescriptor next(
        ::openat(current.get(), component.c_str(),
                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!next) {
      return std::nullopt;
    }
    current = std::move(next);
  }
  return current;
}

bool ensureDirectoryNoFollow(const fs::path &directory) {
  std::error_code error;
  const fs::path absolute = fs::absolute(directory, error).lexically_normal();
  if (error || !absolute.is_absolute() || absolute.root_path().empty()) {
    return false;
  }
  UniqueDirectoryDescriptor current(
      ::open(absolute.root_path().c_str(),
             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!current) {
    return false;
  }
  const fs::path relative = absolute.lexically_relative(absolute.root_path());
  for (const fs::path &component : relative) {
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    int next = ::openat(current.get(), component.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next < 0 && errno == ENOENT &&
        ::mkdirat(current.get(), component.c_str(), 0700) == 0) {
      next = ::openat(current.get(), component.c_str(),
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (next < 0) {
      return false;
    }
    current = UniqueDirectoryDescriptor(next);
  }
  return true;
}

bool clearDirectoryDescriptor(int directory) {
  if (::fchmod(directory, 0700) != 0) {
    return false;
  }
  const int duplicate = ::dup(directory);
  DIR *stream = duplicate >= 0 ? ::fdopendir(duplicate) : nullptr;
  if (!stream) {
    if (duplicate >= 0) {
      ::close(duplicate);
    }
    return false;
  }
  bool cleared = true;
  while (const dirent *entry = ::readdir(stream)) {
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    struct stat status{};
    if (::fstatat(directory, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) !=
        0) {
      cleared = false;
      continue;
    }
    if (S_ISDIR(status.st_mode)) {
      const int child =
          ::openat(directory, entry->d_name,
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (child < 0 || !clearDirectoryDescriptor(child) ||
          ::unlinkat(directory, entry->d_name, AT_REMOVEDIR) != 0) {
        cleared = false;
      }
      if (child >= 0) {
        ::close(child);
      }
    } else if (::unlinkat(directory, entry->d_name, 0) != 0) {
      cleared = false;
    }
  }
  ::closedir(stream);
  return cleared;
}

bool removeDirectoryTreeNoFollow(const fs::path &path) {
  auto parent = openDirectoryNoFollow(path.parent_path());
  const fs::path leaf = path.filename();
  if (!parent || leaf.empty() || leaf == "." || leaf == ".." ||
      leaf.native().find('/') != std::string::npos) {
    return false;
  }
  const int root = ::openat(parent->get(), path.filename().c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (root < 0) {
    return errno == ENOENT;
  }
  const bool cleared = clearDirectoryDescriptor(root);
  ::close(root);
  return cleared &&
         ::unlinkat(parent->get(), path.filename().c_str(), AT_REMOVEDIR) ==
             0 &&
         ::fsync(parent->get()) == 0;
}

bool safeLeaf(const fs::path &path) {
  const fs::path leaf = path.filename();
  return !leaf.empty() && leaf != "." && leaf != ".." &&
         leaf.native().find('/') == std::string::npos;
}

class RetainedTreeCapability {
public:
  static std::optional<RetainedTreeCapability> issue(const fs::path &path) {
    if (!safeLeaf(path)) {
      return std::nullopt;
    }
    auto parent = openDirectoryNoFollow(path.parent_path());
    if (!parent) {
      std::error_code error;
      const auto status = fs::symlink_status(path, error);
      if (status.type() == fs::file_type::not_found ||
          error == std::errc::no_such_file_or_directory) {
        return RetainedTreeCapability(path);
      }
      return std::nullopt;
    }
    struct stat status{};
    if (::fstatat(parent->get(), path.filename().c_str(), &status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) {
        return RetainedTreeCapability(path, std::move(*parent));
      }
      return std::nullopt;
    }
    if (!S_ISDIR(status.st_mode)) {
      return std::nullopt;
    }
    UniqueDirectoryDescriptor root(
        ::openat(parent->get(), path.filename().c_str(),
                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat rootStatus{};
    if (!root || ::fstat(root.get(), &rootStatus) != 0 ||
        rootStatus.st_dev != status.st_dev ||
        rootStatus.st_ino != status.st_ino) {
      return std::nullopt;
    }
    return RetainedTreeCapability(path, std::move(*parent), std::move(root),
                                  status.st_dev, status.st_ino);
  }

  bool existed() const noexcept { return existed_; }
  const fs::path &currentPath() const noexcept { return path_; }

  bool matchesIssuedIdentity() const noexcept {
    if (!parent_) {
      std::error_code error;
      const auto status = fs::symlink_status(path_, error);
      return status.type() == fs::file_type::not_found ||
             error == std::errc::no_such_file_or_directory;
    }
    struct stat current{};
    if (::fstatat(parent_.get(), path_.filename().c_str(), &current,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      return !existed_ && errno == ENOENT;
    }
    if (!existed_ || !S_ISDIR(current.st_mode)) {
      return false;
    }
    struct stat opened{};
    return ::fstat(root_.get(), &opened) == 0 && opened.st_dev == device_ &&
           opened.st_ino == inode_ && current.st_dev == device_ &&
           current.st_ino == inode_;
  }

  bool renameTo(const fs::path &destination) noexcept {
    try {
      if (!existed_ || !matchesIssuedIdentity() || !safeLeaf(destination)) {
        return false;
      }
      fs::path retainedDestination = destination;
      auto destinationParent =
          openDirectoryNoFollow(retainedDestination.parent_path());
      if (!destinationParent) {
        return false;
      }
#if defined(__APPLE__)
      int result = ::renameatx_np(
          parent_.get(), path_.filename().c_str(), destinationParent->get(),
          retainedDestination.filename().c_str(), RENAME_EXCL);
#elif defined(__linux__)
      int result = static_cast<int>(
          ::syscall(SYS_renameat2, parent_.get(), path_.filename().c_str(),
                    destinationParent->get(),
                    retainedDestination.filename().c_str(), RENAME_NOREPLACE));
#else
      int result = -1;
#endif
      if (result != 0 && errno == EACCES && ::fchmod(root_.get(), 0700) == 0) {
#if defined(__APPLE__)
        result = ::renameatx_np(
            parent_.get(), path_.filename().c_str(), destinationParent->get(),
            retainedDestination.filename().c_str(), RENAME_EXCL);
#elif defined(__linux__)
        result = static_cast<int>(::syscall(
            SYS_renameat2, parent_.get(), path_.filename().c_str(),
            destinationParent->get(), retainedDestination.filename().c_str(),
            RENAME_NOREPLACE));
#endif
      }
      if (result != 0) {
        return false;
      }
      struct stat retainedStatus{};
      struct stat destinationStatus{};
      const bool destinationMatches =
          ::fstat(root_.get(), &retainedStatus) == 0 &&
          ::fstatat(destinationParent->get(),
                    retainedDestination.filename().c_str(), &destinationStatus,
                    AT_SYMLINK_NOFOLLOW) == 0 &&
          S_ISDIR(destinationStatus.st_mode) &&
          retainedStatus.st_dev == destinationStatus.st_dev &&
          retainedStatus.st_ino == destinationStatus.st_ino &&
          retainedStatus.st_dev == device_ && retainedStatus.st_ino == inode_;
      const bool sourceSynced = ::fsync(parent_.get()) == 0;
      const bool destinationSynced =
          path_.parent_path() == retainedDestination.parent_path() ||
          ::fsync(destinationParent->get()) == 0;
      parent_ = std::move(*destinationParent);
      path_ = std::move(retainedDestination);
      return destinationMatches && sourceSynced && destinationSynced;
    } catch (...) {
      return false;
    }
  }

private:
  explicit RetainedTreeCapability(fs::path path) : path_(std::move(path)) {}
  RetainedTreeCapability(fs::path path, UniqueDirectoryDescriptor parent)
      : path_(std::move(path)), parent_(std::move(parent)) {}
  RetainedTreeCapability(fs::path path, UniqueDirectoryDescriptor parent,
                         UniqueDirectoryDescriptor root, dev_t device,
                         ino_t inode)
      : path_(std::move(path)), parent_(std::move(parent)),
        root_(std::move(root)), device_(device), inode_(inode), existed_(true) {
  }

  fs::path path_;
  UniqueDirectoryDescriptor parent_;
  UniqueDirectoryDescriptor root_;
  dev_t device_ = 0;
  ino_t inode_ = 0;
  bool existed_ = false;
};
#endif

bool renameTreeNoReplace(const fs::path &source, const fs::path &destination) {
#if defined(_WIN32)
  auto capability = RetainedTreeCapability::issue(source);
  return capability && capability->existed() &&
         capability->renameTo(destination);
#else
  if (!safeLeaf(source) || !safeLeaf(destination)) {
    return false;
  }
  auto sourceParent = openDirectoryNoFollow(source.parent_path());
  auto destinationParent = openDirectoryNoFollow(destination.parent_path());
  if (!sourceParent || !destinationParent) {
    return false;
  }
  struct stat sourceStatus{};
  if (::fstatat(sourceParent->get(), source.filename().c_str(), &sourceStatus,
                AT_SYMLINK_NOFOLLOW) != 0) {
    return false;
  }
#if defined(__APPLE__)
  int renameResult = ::renameatx_np(
      sourceParent->get(), source.filename().c_str(), destinationParent->get(),
      destination.filename().c_str(), RENAME_EXCL);
  if (renameResult != 0 && errno == EACCES) {
    const int sourceDirectory =
        ::openat(sourceParent->get(), source.filename().c_str(),
                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (sourceDirectory >= 0) {
      const bool madeRenamable = ::fchmod(sourceDirectory, 0700) == 0;
      ::close(sourceDirectory);
      if (madeRenamable) {
        renameResult =
            ::renameatx_np(sourceParent->get(), source.filename().c_str(),
                           destinationParent->get(),
                           destination.filename().c_str(), RENAME_EXCL);
      }
    }
  }
  if (renameResult != 0) {
    return false;
  }
#elif defined(__linux__)
  int renameResult = static_cast<int>(
      ::syscall(SYS_renameat2, sourceParent->get(), source.filename().c_str(),
                destinationParent->get(), destination.filename().c_str(),
                RENAME_NOREPLACE));
  if (renameResult != 0 && errno == EACCES && S_ISDIR(sourceStatus.st_mode)) {
    const int sourceDirectory =
        ::openat(sourceParent->get(), source.filename().c_str(),
                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (sourceDirectory >= 0) {
      const bool madeRenamable = ::fchmod(sourceDirectory, 0700) == 0;
      ::close(sourceDirectory);
      if (madeRenamable) {
        renameResult = static_cast<int>(
            ::syscall(SYS_renameat2, sourceParent->get(),
                      source.filename().c_str(), destinationParent->get(),
                      destination.filename().c_str(), RENAME_NOREPLACE));
      }
    }
  }
  if (renameResult != 0) {
    return false;
  }
#else
  return false;
#endif
  const bool sourceSynced = ::fsync(sourceParent->get()) == 0;
  const bool destinationSynced =
      source.parent_path() == destination.parent_path() ||
      ::fsync(destinationParent->get()) == 0;
  return sourceSynced && destinationSynced;
#endif
}

bool quarantineTree(const fs::path &path, const fs::path &quarantineRoot,
                    std::string_view operationId, std::string_view label) {
  std::error_code error;
  const fs::file_status status = fs::symlink_status(path, error);
  if (status.type() == fs::file_type::not_found ||
      error == std::errc::no_such_file_or_directory) {
    return true;
  }
  if (error) {
    return false;
  }
  if (!ensureDirectoryNoFollow(quarantineRoot)) {
    return false;
  }
  static std::atomic_uint64_t serial{0};
  const fs::path destination =
      quarantineRoot / (std::string(operationId) + "-" + std::string(label) +
                        "-" + std::to_string(++serial));
  return renameTreeNoReplace(path, destination);
}

bool quarantineTree(RetainedTreeCapability &capability,
                    const fs::path &quarantineRoot,
                    std::string_view operationId, std::string_view label) {
  if (!capability.existed()) {
    return true;
  }
  if (!ensureDirectoryNoFollow(quarantineRoot)) {
    return false;
  }
  static std::atomic_uint64_t serial{0};
  const fs::path destination =
      quarantineRoot / (std::string(operationId) + "-" + std::string(label) +
                        "-" + std::to_string(++serial));
  return capability.renameTo(destination);
}

std::string nextOperationId() {
  static std::atomic_uint64_t serial{0};
  return "publish-" + std::to_string(++serial);
}

bool writeJournal(const fs::path &path, const PublicationJournal &journal,
                  std::vector<SkinDiagnostic> &diagnostics) {
  OrderedJson root = OrderedJson::object();
  root["schemaVersion"] = 1;
  root["operation"] = "replace-package";
  root["operationId"] = journal.operationId;
  root["phase"] = journal.phase;
  root["package"] =
      OrderedJson{{"directoryName", journal.package.directoryName},
                  {"collisionKey", journal.package.collisionKey}};
  root["visible"] =
      OrderedJson{{"destinationDirectory", journal.destinationDirectory},
                  {"stagingToken", journal.visibleStagingToken},
                  {"backupToken", journal.visibleBackupToken},
                  {"oldPresent", journal.oldPresent},
                  {"newTreeDigest", journal.newTreeDigest}};
  if (journal.oldPresent) {
    root["visible"]["oldTreeDigest"] = journal.oldTreeDigest;
  }
  root["revision"] =
      OrderedJson{{"newDigest", journal.newRevisionDigest},
                  {"stagingToken", journal.revisionStagingToken}};
  root["catalog"] =
      OrderedJson{{"fileName", journal.catalogFileName},
                  {"stagingToken", journal.catalogStagingToken},
                  {"backupToken", journal.catalogBackupToken},
                  {"oldGeneration", journal.oldCatalogGeneration},
                  {"newGeneration", journal.newCatalogGeneration},
                  {"oldSourceGeneration", journal.oldSourceGeneration},
                  {"newSourceGeneration", journal.newSourceGeneration},
                  {"oldSnapshotDigest", journal.oldCatalogDigest},
                  {"newSnapshotDigest", journal.newCatalogDigest}};
  const std::string bytes = root.dump() + "\n";
  const auto operations = atomic_file::privateFileOperations();
  std::string error;
  if (!atomic_file::writeWithBackup(path, std::as_bytes(std::span(bytes)),
                                    error, &operations)) {
    diagnostics.push_back(storeDiagnostic(
        "skin_package_journal_write_failed",
        "unable to durably update skin package publication journal"));
    return false;
  }
  return true;
}

bool writeRemovalJournal(const fs::path &path, const RemovalJournal &journal,
                         std::vector<SkinDiagnostic> &diagnostics) {
  OrderedJson root = OrderedJson::object();
  root["schemaVersion"] = 1;
  root["operation"] = "remove-package";
  root["operationId"] = journal.operationId;
  root["package"] =
      OrderedJson{{"directoryName", journal.package.directoryName},
                  {"collisionKey", journal.package.collisionKey}};
  root["retainedToken"] = journal.retainedToken;
  root["oldTreeDigest"] = journal.oldTreeDigest;
  root["catalog"] =
      OrderedJson{{"stagingToken", journal.catalogStagingToken},
                  {"backupToken", journal.catalogBackupToken},
                  {"oldGeneration", journal.oldCatalogGeneration},
                  {"newGeneration", journal.newCatalogGeneration},
                  {"oldSourceGeneration", journal.oldSourceGeneration},
                  {"newSourceGeneration", journal.newSourceGeneration},
                  {"oldSnapshotDigest", journal.oldCatalogDigest},
                  {"newSnapshotDigest", journal.newCatalogDigest}};
  const std::string bytes = root.dump() + "\n";
  const auto operations = atomic_file::privateFileOperations();
  std::string error;
  if (!atomic_file::writeWithBackup(path, std::as_bytes(std::span(bytes)),
                                    error, &operations)) {
    diagnostics.push_back(storeDiagnostic(
        "skin_package_removal_journal_write_failed",
        "unable to durably update skin package removal journal"));
    return false;
  }
  return true;
}

struct VerifiedCatalogBytes {
  std::string bytes;
  std::string digest;
};

std::optional<VerifiedCatalogBytes>
readCatalogCapability(const fs::path &path) {
  constexpr std::uint64_t maximumBytes = 32ULL * 1024 * 1024;
#if defined(_WIN32)
  HANDLE handle = CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  FILE_ATTRIBUTE_TAG_INFO tags{};
  LARGE_INTEGER size{};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tags,
                                    sizeof(tags)) ||
      (tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      (tags.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      GetFileType(handle) != FILE_TYPE_DISK || !GetFileSizeEx(handle, &size) ||
      size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > maximumBytes) {
    CloseHandle(handle);
    return std::nullopt;
  }
  std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    DWORD amount = 0;
    if (!ReadFile(handle, bytes.data() + offset,
                  static_cast<DWORD>(
                      std::min<std::size_t>(bytes.size() - offset, MAXDWORD)),
                  &amount, nullptr) ||
        amount == 0) {
      CloseHandle(handle);
      return std::nullopt;
    }
    offset += amount;
  }
  CloseHandle(handle);
#else
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return std::nullopt;
  }
  struct stat status{};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_nlink != 1 || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > maximumBytes) {
    ::close(descriptor);
    return std::nullopt;
  }
  std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t amount =
        ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (amount < 0 && errno == EINTR) {
      continue;
    }
    if (amount <= 0) {
      ::close(descriptor);
      return std::nullopt;
    }
    offset += static_cast<std::size_t>(amount);
  }
  char extra = 0;
  const ssize_t extraAmount = ::read(descriptor, &extra, 1);
  ::close(descriptor);
  if (extraAmount != 0) {
    return std::nullopt;
  }
#endif
  const std::string digest = file_checksum::sha256(bytes);
  return VerifiedCatalogBytes{.bytes = std::move(bytes), .digest = digest};
}

bool installCatalogBytes(std::string_view bytes, const fs::path &destination,
                         std::vector<SkinDiagnostic> &diagnostics) {
  const auto operations = atomic_file::privateFileOperations();
  std::string error;
  if (!atomic_file::writeWithBackup(
          destination, std::as_bytes(std::span(bytes)), error, &operations)) {
    diagnostics.push_back(
        storeDiagnostic("skin_package_catalog_repair_failed",
                        "unable to restore verified skin catalog metadata"));
    return false;
  }
  return true;
}

std::string revisionKey(const SkinPackageId &package, std::string_view digest) {
  return package.collisionKey + ":" + std::string(digest);
}

std::string activationKey(const SkinProfileId &profile,
                          const SkinEntryId &entry,
                          std::string_view configurationDigest) {
  std::string result;
  result.reserve(profile.opaque.size() + entry.collisionKey.size() +
                 configurationDigest.size() + 2);
  result.append(profile.opaque);
  result.push_back('\0');
  result.append(entry.collisionKey);
  result.push_back('\0');
  result.append(configurationDigest);
  return result;
}

class LockedFlagReset {
public:
  LockedFlagReset(std::mutex &mutex, bool &flag) noexcept
      : mutex_(mutex), flag_(flag) {}
  LockedFlagReset(const LockedFlagReset &) = delete;
  ~LockedFlagReset() { reset(); }
  void dismiss() noexcept { active_ = false; }
  void reset() noexcept {
    if (!active_) {
      return;
    }
    std::scoped_lock lock(mutex_);
    flag_ = false;
    active_ = false;
  }

private:
  std::mutex &mutex_;
  bool &flag_;
  bool active_ = true;
};

template <typename Callback> class ScopeRollback {
public:
  explicit ScopeRollback(Callback &callback) noexcept : callback_(callback) {}
  ScopeRollback(const ScopeRollback &) = delete;
  ~ScopeRollback() noexcept {
    if (active_) {
      callback_();
    }
  }
  void dismiss() noexcept { active_ = false; }

private:
  Callback &callback_;
  bool active_ = true;
};

void appendDiagnosticNoThrow(std::vector<SkinDiagnostic> &diagnostics,
                             std::string_view code,
                             std::string_view message) noexcept {
  try {
    diagnostics.push_back(
        storeDiagnostic(std::string(code), std::string(message)));
  } catch (...) {
  }
}

ValidatedSkinActivation cloneActivation(const ValidatedSkinActivation &value) {
  return {.revision = value.revision.clone(),
          .entry = value.entry,
          .reconciledSettings = value.reconciledSettings,
          .configurationDigest = value.configurationDigest};
}

std::optional<std::vector<SkinEntryId>>
discoverEntries(SkinRevisionReadView view, std::stop_token stop,
                std::vector<SkinDiagnostic> &diagnostics) {
  std::vector<SkinEntryId> entries;
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(view.root(), error), end;
       !error && iterator != end; ++iterator) {
    if (stop.stop_requested()) {
      return std::nullopt;
    }
    if (!iterator->is_regular_file(error)) {
      continue;
    }
    const fs::path relative = iterator->path().lexically_relative(view.root());
    if (relative.extension() != ".luaskin") {
      continue;
    }
    const auto utf8 = relative.generic_u8string();
    const std::string path(reinterpret_cast<const char *>(utf8.data()),
                           utf8.size());
    auto normalized = normalizeEntryPath(view.revision().package, path);
    if (!normalized.entry) {
      diagnostics.push_back(storeDiagnostic(
          "skin_entry_path_invalid",
          "a discovered skin entry has an invalid virtual path"));
      return std::nullopt;
    }
    entries.push_back(std::move(*normalized.entry));
  }
  if (error) {
    diagnostics.push_back(storeDiagnostic(
        "skin_entry_inventory_failed",
        "unable to inventory entries in a stable skin revision"));
    return std::nullopt;
  }
  std::ranges::sort(entries, {}, &SkinEntryId::collisionKey);
  return entries;
}

} // namespace

SkinPackageStore::SkinPackageStore(
    SkinStorageRoots roots, SkinPackageCatalog &catalog,
    SkinAliasDetector &aliases, ISkinProfileSnapshotProvider &profileSnapshots)
    : roots_(std::move(roots)), catalog_(catalog), aliases_(aliases),
      profileSnapshots_(profileSnapshots) {}

SkinRecoveryResult SkinPackageStore::recoverBeforeServiceStart() {
  std::uint8_t expected = 0;
  if (!recoveryState_.compare_exchange_strong(expected, 1)) {
    return {.disposition = expected == 1
                               ? SkinRecoveryDisposition::ConcurrentCallRejected
                               : SkinRecoveryDisposition::AlreadyRecovered};
  }
  struct FinishRecovery {
    std::atomic_uint8_t &state;
    ~FinishRecovery() { state.store(2); }
  } finish{recoveryState_};

  SkinRecoveryResult result;
  try {
    std::error_code directoryError;
    if (!ensureDirectoryNoFollow(roots_.visiblePackages) ||
        !ensureDirectoryNoFollow(roots_.privateRevisions) ||
        !ensureDirectoryNoFollow(roots_.privateCatalog)) {
      result.diagnostics.push_back(
          storeDiagnostic("skin_package_recovery_storage_unavailable",
                          "skin package storage roots are unavailable"));
      return result;
    }

    const auto hydrateRevisionLeases = [&]() -> bool {
      std::map<std::string, SkinRevisionLease, std::less<>> hydrated;
      std::map<std::string, SkinRevisionWeakPin, std::less<>> pins;
      const auto snapshot = catalog_.snapshot();
      for (const SkinCatalogEntrySnapshot &entry : snapshot->entries) {
        const std::string key =
            revisionKey(entry.entry.package, entry.revisionDigest);
        if (hydrated.contains(key)) {
          continue;
        }
        const fs::path revisionRoot =
            roots_.privateRevisions / entry.revisionDigest;
        SkinTreeSnapshotter snapshotter(roots_, aliases_);
        auto prepared =
            snapshotter.snapshot(revisionRoot, entry.entry.package, {}, {});
        if (!prepared.prepared ||
            prepared.prepared->revision().lowercaseSha256 !=
                entry.revisionDigest) {
          result.diagnostics.push_back(storeDiagnostic(
              "skin_package_recovery_revision_missing",
              "cataloged skin revision is unavailable or invalid"));
          return false;
        }
        std::string publicationError;
        auto lease = std::move(*prepared.prepared).publish(publicationError);
        if (!lease) {
          result.diagnostics.push_back(
              storeDiagnostic("skin_package_recovery_revision_lease_failed",
                              "cataloged skin revision cannot be retained"));
          return false;
        }
        pins.emplace(key, lease->weakPin());
        hydrated.emplace(key, std::move(*lease));
      }
      std::scoped_lock lock(stateMutex_);
      revisionLeases_ = std::move(hydrated);
      revisionPins_ = std::move(pins);
      stateCatalogGeneration_ = snapshot->catalogGeneration;
      stateSourceGeneration_ = snapshot->sourceGeneration;
      return true;
    };

    const fs::path journalPath =
        roots_.privateCatalog / "publication-journal.json";
    const fs::path removalJournalPath =
        roots_.privateCatalog / "removal-journal.json";
    std::error_code existenceError;
    const bool journalExists = fs::exists(journalPath, existenceError);
    if (existenceError) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_recovery_journal_unavailable",
          "skin package publication journal cannot be inspected"));
      return result;
    }
    const bool removalJournalExists =
        fs::exists(removalJournalPath, existenceError);
    if (existenceError || (journalExists && removalJournalExists)) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_recovery_journal_conflict",
          "skin package transaction journals are inconsistent"));
      return result;
    }
    if (removalJournalExists) {
      auto journal = loadRemovalJournal(removalJournalPath, result.diagnostics);
      if (!journal) {
        return result;
      }
      const fs::path visible =
          roots_.visiblePackages / journal->package.directoryName;
      const fs::path retained =
          roots_.visiblePackages.parent_path() / ".skin-removal-staging" /
          journal->retainedToken / journal->package.directoryName;
      const fs::path catalogFile = roots_.privateCatalog / "catalog.json";
      const fs::path catalogStaging = roots_.privateCatalog /
                                      ".removal-staging" /
                                      journal->catalogStagingToken;
      const fs::path catalogBackup = roots_.privateCatalog /
                                     ".removal-backups" /
                                     journal->catalogBackupToken;
      auto visibleCapability = RetainedTreeCapability::issue(visible);
      auto retainedCapability = RetainedTreeCapability::issue(retained);
      if (!visibleCapability || !retainedCapability) {
        result.diagnostics.push_back(storeDiagnostic(
            "skin_package_removal_recovery_capability_failed",
            "skin package removal recovery could not retain exact tree "
            "identities"));
        return result;
      }
      const bool visibleOld =
          treeDigestMatches(visible, journal->package, journal->oldTreeDigest,
                            roots_, aliases_) &&
          visibleCapability->matchesIssuedIdentity();
      const bool retainedOld =
          treeDigestMatches(retained, journal->package, journal->oldTreeDigest,
                            roots_, aliases_) &&
          retainedCapability->matchesIssuedIdentity();
      std::error_code visibleError;
      const auto visibleStatus = fs::symlink_status(visible, visibleError);
      const bool visibleMissing =
          visibleStatus.type() == fs::file_type::not_found ||
          visibleError == std::errc::no_such_file_or_directory;
      const auto finalCatalog = readCatalogCapability(catalogFile);
      const auto stagedCatalog = readCatalogCapability(catalogStaging);
      const auto backupCatalog = readCatalogCapability(catalogBackup);
      const VerifiedCatalogBytes *newCatalog =
          finalCatalog && finalCatalog->digest == journal->newCatalogDigest
              ? &*finalCatalog
          : stagedCatalog && stagedCatalog->digest == journal->newCatalogDigest
              ? &*stagedCatalog
              : nullptr;
      const VerifiedCatalogBytes *oldCatalog =
          finalCatalog && finalCatalog->digest == journal->oldCatalogDigest
              ? &*finalCatalog
          : backupCatalog && backupCatalog->digest == journal->oldCatalogDigest
              ? &*backupCatalog
              : nullptr;
      const auto validateCatalogGeneration =
          [&](const VerifiedCatalogBytes *candidate,
              std::uint64_t expectedCatalogGeneration,
              std::uint64_t expectedSourceGeneration)
          -> std::optional<SkinPackageCatalogSnapshot> {
        if (!candidate) {
          return std::nullopt;
        }
        SkinPackageCatalogSnapshot decoded;
        if (!catalog_.decodeSnapshotBytes(candidate->bytes, decoded) ||
            decoded.catalogGeneration != expectedCatalogGeneration ||
            decoded.sourceGeneration != expectedSourceGeneration) {
          return std::nullopt;
        }
        std::set<std::string, std::less<>> verified;
        for (const SkinCatalogEntrySnapshot &entry : decoded.entries) {
          const std::string key =
              revisionKey(entry.entry.package, entry.revisionDigest);
          if (verified.insert(key).second &&
              !treeDigestMatches(roots_.privateRevisions / entry.revisionDigest,
                                 entry.entry.package, entry.revisionDigest,
                                 roots_, aliases_)) {
            return std::nullopt;
          }
        }
        return decoded;
      };
      const auto decodedOldCatalog =
          validateCatalogGeneration(oldCatalog, journal->oldCatalogGeneration,
                                    journal->oldSourceGeneration);
      const auto decodedNewCatalog =
          validateCatalogGeneration(newCatalog, journal->newCatalogGeneration,
                                    journal->newSourceGeneration);
      const bool oldComplete =
          (visibleOld || retainedOld) && decodedOldCatalog.has_value();
      const bool newComplete = visibleMissing && decodedNewCatalog.has_value();
      if (!oldComplete && !newComplete) {
        result.diagnostics.push_back(storeDiagnostic(
            "skin_package_removal_recovery_incomplete",
            "skin package removal has no complete recoverable generation"));
        return result;
      }
      const bool selectNew =
          newComplete && ((finalCatalog &&
                           finalCatalog->digest == journal->newCatalogDigest) ||
                          !oldComplete);
      if (!selectNew && !visibleOld &&
          (!retainedOld || !retainedCapability->renameTo(visible))) {
        result.diagnostics.push_back(storeDiagnostic(
            "skin_package_removal_recovery_visible_failed",
            "unable to restore the removed visible skin package"));
        return result;
      }
      const VerifiedCatalogBytes &selected =
          selectNew ? *newCatalog : *oldCatalog;
      if ((!finalCatalog || finalCatalog->digest != selected.digest) &&
          !installCatalogBytes(selected.bytes, catalogFile,
                               result.diagnostics)) {
        return result;
      }
      if (!catalog_.recover()) {
        result.diagnostics.push_back(storeDiagnostic(
            "skin_package_removal_recovery_catalog_failed",
            "restored removal catalog cannot be decoded safely"));
        return result;
      }
      const auto recovered = catalog_.snapshot();
      const bool packagePresent = std::ranges::any_of(
          recovered->packages, [&](const SkinPackageId &package) {
            return package == journal->package;
          });
      const SkinPackageCatalogSnapshot &selectedDecoded =
          selectNew ? *decodedNewCatalog : *decodedOldCatalog;
      const bool selectedPackagePresent = std::ranges::any_of(
          selectedDecoded.packages, [&](const SkinPackageId &package) {
            return package == journal->package;
          });
      const std::uint64_t generation = selectNew
                                           ? journal->newCatalogGeneration
                                           : journal->oldCatalogGeneration;
      const std::uint64_t sourceGeneration = selectNew
                                                 ? journal->newSourceGeneration
                                                 : journal->oldSourceGeneration;
      if (recovered->catalogGeneration != generation ||
          recovered->sourceGeneration != sourceGeneration ||
          packagePresent != selectedPackagePresent ||
          (selectNew && packagePresent) || !hydrateRevisionLeases()) {
        result.diagnostics.push_back(storeDiagnostic(
            "skin_package_removal_recovery_catalog_failed",
            "restored removal catalog has inconsistent typed identity"));
        return result;
      }
      if (selectNew &&
          !quarantineTree(*retainedCapability,
                          roots_.visiblePackages.parent_path() /
                              ".skin-recovery-quarantine",
                          journal->operationId, "recovered-removal")) {
        return result;
      }
      const bool stagingClean = quarantineTree(
          catalogStaging, roots_.privateCatalog / ".recovery-quarantine",
          journal->operationId, "removal-catalog-staging");
      const bool backupClean = quarantineTree(
          catalogBackup, roots_.privateCatalog / ".recovery-quarantine",
          journal->operationId, "removal-catalog-backup");
      fs::remove(removalJournalPath, directoryError);
      std::string syncError;
      if (!stagingClean || !backupClean || directoryError ||
          !atomic_file::syncDirectory(roots_.privateCatalog, syncError)) {
        result.diagnostics.push_back(storeDiagnostic(
            "skin_package_removal_recovery_cleanup_failed",
            "unable to finalize recovered skin package removal"));
        return result;
      }
      result.disposition = SkinRecoveryDisposition::Recovered;
      recoverySucceeded_.store(true);
      return result;
    }
    if (!journalExists) {
      if (!catalog_.recover()) {
        result.diagnostics.push_back(
            storeDiagnostic("skin_package_recovery_catalog_failed",
                            "private skin catalog metadata is malformed"));
        return result;
      }
      if (!hydrateRevisionLeases()) {
        return result;
      }
      recoverySucceeded_.store(true);
      result.disposition = SkinRecoveryDisposition::Recovered;
      return result;
    }

    auto journal = loadJournal(journalPath, result.diagnostics);
    if (!journal) {
      return result;
    }
    const fs::path visible =
        roots_.visiblePackages / journal->destinationDirectory;
    const fs::path visibleStaging = roots_.visiblePackages.parent_path() /
                                    ".skin-import-staging" /
                                    journal->visibleStagingToken;
    const fs::path visibleBackup =
        roots_.visiblePackages.parent_path() / ".skin-publication-backups" /
        journal->visibleBackupToken / journal->destinationDirectory;
    const fs::path newRevision =
        roots_.privateRevisions / journal->newRevisionDigest;
    const fs::path revisionStaging =
        roots_.privateRevisions / ".staging" / journal->revisionStagingToken;
    const fs::path catalogFile =
        roots_.privateCatalog / journal->catalogFileName;
    const fs::path catalogStaging = roots_.privateCatalog /
                                    ".publication-staging" /
                                    journal->catalogStagingToken;
    const fs::path catalogBackup = roots_.privateCatalog /
                                   ".publication-backups" /
                                   journal->catalogBackupToken;

    auto visibleCapability = RetainedTreeCapability::issue(visible);
    auto backupCapability = RetainedTreeCapability::issue(visibleBackup);
    auto stagingCapability = RetainedTreeCapability::issue(visibleStaging);
    auto newRevisionCapability = RetainedTreeCapability::issue(newRevision);
    if (!visibleCapability || !backupCapability || !stagingCapability ||
        !newRevisionCapability) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_recovery_capability_failed",
          "skin package recovery could not retain exact tree identities"));
      return result;
    }
    const bool visibleIsOld =
        journal->oldPresent && visibleCapability->existed() &&
        treeDigestMatches(visible, journal->package, journal->oldTreeDigest,
                          roots_, aliases_) &&
        visibleCapability->matchesIssuedIdentity();
    const bool visibleIsNew =
        visibleCapability->existed() &&
        treeDigestMatches(visible, journal->package, journal->newTreeDigest,
                          roots_, aliases_) &&
        visibleCapability->matchesIssuedIdentity();
    const bool backupIsOld =
        journal->oldPresent && backupCapability->existed() &&
        treeDigestMatches(visibleBackup, journal->package,
                          journal->oldTreeDigest, roots_, aliases_) &&
        backupCapability->matchesIssuedIdentity();
    const bool stagingIsNew =
        stagingCapability->existed() &&
        treeDigestMatches(visibleStaging, journal->package,
                          journal->newTreeDigest, roots_, aliases_) &&
        stagingCapability->matchesIssuedIdentity();
    const bool newRevisionValid =
        newRevisionCapability->existed() &&
        treeDigestMatches(newRevision, journal->package,
                          journal->newRevisionDigest, roots_, aliases_) &&
        newRevisionCapability->matchesIssuedIdentity();
    std::error_code visibleStatusError;
    const auto visibleStatus = fs::symlink_status(visible, visibleStatusError);
    const bool visibleMissing =
        visibleStatus.type() == fs::file_type::not_found ||
        visibleStatusError == std::errc::no_such_file_or_directory;

    const auto finalCatalog = readCatalogCapability(catalogFile);
    const auto stagedCatalog = readCatalogCapability(catalogStaging);
    const auto backupCatalog = readCatalogCapability(catalogBackup);
    const VerifiedCatalogBytes *newCatalog =
        finalCatalog && finalCatalog->digest == journal->newCatalogDigest
            ? &*finalCatalog
        : stagedCatalog && stagedCatalog->digest == journal->newCatalogDigest
            ? &*stagedCatalog
            : nullptr;
    const VerifiedCatalogBytes *oldCatalog =
        finalCatalog && finalCatalog->digest == journal->oldCatalogDigest
            ? &*finalCatalog
        : backupCatalog && backupCatalog->digest == journal->oldCatalogDigest
            ? &*backupCatalog
            : nullptr;
    const auto validateCatalogGeneration =
        [&](const VerifiedCatalogBytes *candidate,
            std::uint64_t expectedCatalogGeneration,
            std::uint64_t expectedSourceGeneration)
        -> std::optional<SkinPackageCatalogSnapshot> {
      if (!candidate) {
        return std::nullopt;
      }
      SkinPackageCatalogSnapshot decoded;
      if (!catalog_.decodeSnapshotBytes(candidate->bytes, decoded) ||
          decoded.catalogGeneration != expectedCatalogGeneration ||
          decoded.sourceGeneration != expectedSourceGeneration) {
        return std::nullopt;
      }
      std::set<std::string, std::less<>> verified;
      for (const SkinCatalogEntrySnapshot &entry : decoded.entries) {
        const std::string key =
            revisionKey(entry.entry.package, entry.revisionDigest);
        if (verified.insert(key).second &&
            !treeDigestMatches(roots_.privateRevisions / entry.revisionDigest,
                               entry.entry.package, entry.revisionDigest,
                               roots_, aliases_)) {
          return std::nullopt;
        }
      }
      return decoded;
    };
    const auto decodedOldCatalog =
        validateCatalogGeneration(oldCatalog, journal->oldCatalogGeneration,
                                  journal->oldSourceGeneration);
    const auto decodedNewCatalog =
        validateCatalogGeneration(newCatalog, journal->newCatalogGeneration,
                                  journal->newSourceGeneration);
    const bool oldVisibleComplete = journal->oldPresent
                                        ? (visibleIsOld || backupIsOld)
                                        : (visibleMissing || visibleIsNew);
    const bool oldComplete = oldVisibleComplete && decodedOldCatalog;
    const bool newComplete =
        (visibleIsNew || stagingIsNew) && newRevisionValid && decodedNewCatalog;
    if (!oldComplete && !newComplete) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_recovery_incomplete",
          "skin package publication has no complete recoverable generation"));
      return result;
    }
    const bool selectNew =
        newComplete && ((visibleIsNew && newRevisionValid) || !oldComplete);

    if (!newRevisionValid) {
      std::error_code newRevisionError;
      const auto newRevisionStatus =
          fs::symlink_status(newRevision, newRevisionError);
      if (newRevisionStatus.type() != fs::file_type::not_found &&
          !quarantineTree(*newRevisionCapability,
                          roots_.privateRevisions / ".recovery-quarantine",
                          journal->operationId, "invalid-revision")) {
        result.diagnostics.push_back(storeDiagnostic(
            "skin_package_recovery_cleanup_failed",
            "invalid journal-owned skin revision cannot be quarantined"));
        return result;
      }
    }

    if (!selectNew && journal->oldPresent && !visibleIsOld) {
      if ((!visibleMissing &&
           !quarantineTree(*visibleCapability,
                           roots_.visiblePackages.parent_path() /
                               ".skin-recovery-quarantine",
                           journal->operationId, "rolled-back-visible")) ||
          !backupCapability->renameTo(visible)) {
        result.diagnostics.push_back(storeDiagnostic(
            "skin_package_recovery_visible_failed",
            "unable to restore the previous visible skin package"));
        return result;
      }
    } else if (!selectNew && !journal->oldPresent && !visibleMissing) {
      if (!visibleIsNew ||
          !quarantineTree(*visibleCapability,
                          roots_.visiblePackages.parent_path() /
                              ".skin-recovery-quarantine",
                          journal->operationId,
                          "rolled-back-created-visible")) {
        result.diagnostics.push_back(storeDiagnostic(
            "skin_package_recovery_visible_failed",
            "unable to restore absence before skin package creation"));
        return result;
      }
    } else if (selectNew && !visibleIsNew) {
      if ((!visibleMissing &&
           !quarantineTree(*visibleCapability,
                           roots_.visiblePackages.parent_path() /
                               ".skin-recovery-quarantine",
                           journal->operationId, "replaced-visible")) ||
          !stagingIsNew || !stagingCapability->renameTo(visible)) {
        result.diagnostics.push_back(storeDiagnostic(
            "skin_package_recovery_visible_failed",
            "unable to complete visible skin package publication"));
        return result;
      }
    }

    const VerifiedCatalogBytes &selectedCatalog =
        selectNew ? *newCatalog : *oldCatalog;
    const bool finalCatalogSelected =
        finalCatalog && finalCatalog->digest == selectedCatalog.digest;
    if (!finalCatalogSelected &&
        !installCatalogBytes(selectedCatalog.bytes, catalogFile,
                             result.diagnostics)) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_recovery_catalog_incomplete",
          "skin package publication has no digest-matched catalog generation"));
      return result;
    }
    const std::string &selectedCatalogDigest =
        selectNew ? journal->newCatalogDigest : journal->oldCatalogDigest;
    const auto installedCatalog = readCatalogCapability(catalogFile);
    if (!installedCatalog ||
        installedCatalog->digest != selectedCatalogDigest) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_recovery_catalog_incomplete",
          "restored skin catalog bytes do not match the journal identity"));
      return result;
    }
    if (!catalog_.recover()) {
      result.diagnostics.push_back(
          storeDiagnostic("skin_package_recovery_catalog_failed",
                          "restored skin catalog cannot be decoded safely"));
      return result;
    }
    const auto recoveredCatalog = catalog_.snapshot();
    const std::uint64_t selectedGeneration =
        selectNew ? journal->newCatalogGeneration
                  : journal->oldCatalogGeneration;
    const std::uint64_t selectedSourceGeneration =
        selectNew ? journal->newSourceGeneration : journal->oldSourceGeneration;
    const bool selectedPackagePresent = std::ranges::any_of(
        recoveredCatalog->packages, [&](const SkinPackageId &package) {
          return package == journal->package;
        });
    const bool oldPackagePresent =
        decodedOldCatalog &&
        std::ranges::any_of(decodedOldCatalog->packages,
                            [&](const SkinPackageId &package) {
                              return package == journal->package;
                            });
    if (recoveredCatalog->catalogGeneration != selectedGeneration ||
        recoveredCatalog->sourceGeneration != selectedSourceGeneration ||
        (selectNew && !selectedPackagePresent) ||
        (!selectNew && selectedPackagePresent != oldPackagePresent)) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_recovery_catalog_identity_mismatch",
          "digest-matched skin catalog has inconsistent typed identity"));
      return result;
    }
    if (!hydrateRevisionLeases()) {
      return result;
    }

    const fs::path visibleQuarantine =
        roots_.visiblePackages.parent_path() / ".skin-recovery-quarantine";
    const fs::path revisionQuarantine =
        roots_.privateRevisions / ".recovery-quarantine";
    const fs::path catalogQuarantine =
        roots_.privateCatalog / ".recovery-quarantine";
    const bool visibleStagingCleaned =
        stagingCapability->currentPath() == visibleStaging
            ? quarantineTree(*stagingCapability, visibleQuarantine,
                             journal->operationId, "visible-staging")
            : quarantineTree(visibleStaging, visibleQuarantine,
                             journal->operationId, "visible-staging");
    const bool visibleBackupCleaned =
        backupCapability->currentPath() == visibleBackup
            ? quarantineTree(*backupCapability, visibleQuarantine,
                             journal->operationId, "visible-backup")
            : quarantineTree(visibleBackup, visibleQuarantine,
                             journal->operationId, "visible-backup");
    const bool revisionStagingCleaned =
        quarantineTree(revisionStaging, revisionQuarantine,
                       journal->operationId, "revision-staging");
    const bool catalogStagingCleaned =
        quarantineTree(catalogStaging, catalogQuarantine, journal->operationId,
                       "catalog-staging");
    const bool catalogBackupCleaned =
        quarantineTree(catalogBackup, catalogQuarantine, journal->operationId,
                       "catalog-backup");
    if (!visibleStagingCleaned || !visibleBackupCleaned ||
        !revisionStagingCleaned || !catalogStagingCleaned ||
        !catalogBackupCleaned) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_recovery_cleanup_failed",
          "unable to clean journal-owned skin package staging"));
      return result;
    }
    fs::remove(journalPath, directoryError);
    std::string syncError;
    if (directoryError ||
        !atomic_file::syncDirectory(roots_.privateCatalog, syncError)) {
      result.diagnostics.push_back(
          storeDiagnostic("skin_package_recovery_journal_cleanup_failed",
                          "unable to finalize skin package journal recovery"));
      return result;
    }
    result.disposition = SkinRecoveryDisposition::Recovered;
    recoverySucceeded_.store(true);
    return result;
  } catch (...) {
    result.disposition = SkinRecoveryDisposition::Failed;
    result.diagnostics.push_back(storeDiagnostic(
        "skin_package_recovery_exception",
        "skin package recovery failed without exposing filesystem details"));
    return result;
  }
}

bool SkinPackageStore::operationServiceReady() const noexcept {
  return recoveryState_.load() == 2 && recoverySucceeded_.load() &&
         !poisoned_.load();
}

PreparePackageResult SkinPackageStore::prepareArchive(
    const std::filesystem::path &zip, const SkinPackageId &package,
    std::stop_token stop, SkinProgressCallback progress) {
  return SkinArchiveImporter(roots_, aliases_)
      .prepareArchive(zip, package, stop, std::move(progress));
}

PreparePackageResult SkinPackageStore::prepareFolder(
    const std::filesystem::path &folder, const SkinPackageId &package,
    std::stop_token stop, SkinProgressCallback progress) {
  return SkinArchiveImporter(roots_, aliases_)
      .prepareFolder(folder, package, stop, std::move(progress));
}

PublishPackageResult SkinPackageStore::publish(
    PreparedPackage &&prepared, PackageCollisionPolicy collisionPolicy,
    ProfileInventorySnapshot inventory, SkinEntryValidator &validator,
    std::stop_token stop, SkinProgressCallback progress) {
  PublishPackageResult result;
  result.package = prepared.packageId();
  result.entries.assign(prepared.entries().begin(), prepared.entries().end());
  if (poisoned_.load() || stop.stop_requested()) {
    if (poisoned_.load()) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_store_poisoned",
          "skin package storage requires a successful restart recovery"));
    }
    return result;
  }

  std::vector<SkinCatalogEntrySnapshot> validatedEntries;
  validatedEntries.reserve(prepared.entries().size());
  for (const SkinEntryId &entry : prepared.entries()) {
    if (progress) {
      try {
        progress({.phase = SkinProgressPhase::Validating});
      } catch (...) {
        result.diagnostics.push_back(
            storeDiagnostic("skin_package_progress_failed",
                            "skin package progress reporting failed"));
        return result;
      }
    }
    auto validation =
        validator.validate(prepared.readView(), entry, nullptr, stop);
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(validation.diagnostics.begin()),
        std::make_move_iterator(validation.diagnostics.end()));
    if (validation.cancelled || stop.stop_requested()) {
      return result;
    }
    SkinCatalogEntrySnapshot catalogEntry{
        .entry = entry,
        .revisionDigest = prepared.candidateRevision().lowercaseSha256,
        .validation = validation.disposition,
        .metadata = std::move(validation.metadata),
        .diagnostics = {}};
    if (!validation.configurationDigest.empty() &&
        lowercaseSha256(validation.configurationDigest)) {
      catalogEntry.validatedConfigurationDigests.push_back(
          std::move(validation.configurationDigest));
    }
    validatedEntries.push_back(std::move(catalogEntry));
  }

  for (const VersionedSkinProfileSettings &profile : inventory.profiles) {
    if (!profile.settings.selected7KeyEntry ||
        profile.settings.selected7KeyEntry->package != prepared.packageId()) {
      continue;
    }
    const SkinEntryId &selected = *profile.settings.selected7KeyEntry;
    const auto preparedEntry = std::ranges::find(prepared.entries(), selected);
    if (preparedEntry == prepared.entries().end()) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_selected_entry_missing",
          "a selected skin entry is absent from the replacement package"));
      return result;
    }
    const auto settings = profile.settings.entries.find(selected);
    const EntryProfileSettings *desired =
        settings == profile.settings.entries.end() ? nullptr
                                                   : &settings->second;
    auto validation =
        validator.validate(prepared.readView(), selected, desired, stop);
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(validation.diagnostics.begin()),
        std::make_move_iterator(validation.diagnostics.end()));
    if (validation.cancelled || stop.stop_requested()) {
      return result;
    }
    if (validation.disposition != SkinValidationDisposition::Selectable7Key ||
        !validation.reconciledSettings ||
        !lowercaseSha256(validation.configurationDigest)) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_selected_configuration_invalid",
          "a selected skin configuration rejects the replacement package"));
      return result;
    }
    auto catalogEntry = std::ranges::find_if(
        validatedEntries,
        [&selected](const SkinCatalogEntrySnapshot &candidate) {
          return candidate.entry == selected;
        });
    if (catalogEntry != validatedEntries.end() &&
        !std::ranges::contains(catalogEntry->validatedConfigurationDigests,
                               validation.configurationDigest)) {
      catalogEntry->validatedConfigurationDigests.push_back(
          std::move(validation.configurationDigest));
    }
  }

  const fs::path visible =
      roots_.visiblePackages / prepared.packageId().directoryName;
  auto visibleCapability = RetainedTreeCapability::issue(visible);
  if (!visibleCapability) {
    result.diagnostics.push_back(storeDiagnostic(
        "skin_package_destination_unavailable",
        "visible skin package destination cannot be inspected"));
    return result;
  }
  const bool destinationExists = visibleCapability->existed();
  if (destinationExists && collisionPolicy == PackageCollisionPolicy::Reject) {
    result.diagnostics.push_back(storeDiagnostic(
        "skin_package_collision",
        "a skin package with this normalized identity already exists"));
    return result;
  }

  auto oldCatalog = catalog_.snapshot();
  SkinPackageCatalogSnapshot newCatalog = *oldCatalog;
  ++newCatalog.catalogGeneration;
  ++newCatalog.sourceGeneration;
  std::erase_if(newCatalog.packages, [&](const SkinPackageId &package) {
    return package.collisionKey == prepared.packageId().collisionKey;
  });
  std::erase_if(newCatalog.entries, [&](const SkinCatalogEntrySnapshot &entry) {
    return entry.entry.package.collisionKey ==
           prepared.packageId().collisionKey;
  });
  newCatalog.packages.push_back(prepared.packageId());
  newCatalog.entries.insert(newCatalog.entries.end(),
                            std::make_move_iterator(validatedEntries.begin()),
                            std::make_move_iterator(validatedEntries.end()));

  std::string oldTreeDigest;
  std::optional<std::vector<TreeMetadataRecord>> oldTreeManifest;
  if (destinationExists) {
    oldTreeManifest = treeMetadataManifest(visible, aliases_);
    auto digest = treeDigest(visible, prepared.packageId(), roots_, aliases_,
                             stop, result.diagnostics);
    if (!oldTreeManifest || !digest) {
      return result;
    }
    oldTreeDigest = std::move(*digest);
    if (!visibleCapability->matchesIssuedIdentity() ||
        treeMetadataManifest(visible, aliases_) != oldTreeManifest) {
      result.diagnostics.push_back(
          storeDiagnostic("skin_package_source_changed",
                          "visible skin package changed during inspection"));
      return result;
    }
  }
  const std::string operationId = nextOperationId();
  const fs::path backup = roots_.visiblePackages.parent_path() /
                          ".skin-publication-backups" / operationId /
                          prepared.packageId().directoryName;
  PublicationJournal journal{
      .operationId = operationId,
      .phase = "intent-written",
      .package = prepared.packageId(),
      .destinationDirectory = prepared.packageId().directoryName,
      .visibleStagingToken = prepared.visibleStagingRoot().filename().string(),
      .visibleBackupToken = operationId,
      .oldPresent = destinationExists,
      .oldTreeDigest = oldTreeDigest,
      .newTreeDigest = prepared.candidateRevision().lowercaseSha256,
      .oldRevisionDigest = oldTreeDigest,
      .newRevisionDigest = prepared.candidateRevision().lowercaseSha256,
      .revisionStagingToken = prepared.readView().root().filename().string(),
      .catalogFileName = "catalog.json",
      .catalogStagingToken = operationId + "-new.json",
      .catalogBackupToken = operationId + "-old.json",
      .oldCatalogGeneration = oldCatalog->catalogGeneration,
      .newCatalogGeneration = newCatalog.catalogGeneration,
      .oldSourceGeneration = oldCatalog->sourceGeneration,
      .newSourceGeneration = newCatalog.sourceGeneration,
      .oldCatalogDigest = catalog_.snapshotDigest(*oldCatalog),
      .newCatalogDigest = catalog_.snapshotDigest(newCatalog)};
  const fs::path catalogStaging = roots_.privateCatalog /
                                  ".publication-staging" /
                                  journal.catalogStagingToken;
  const fs::path catalogBackup = roots_.privateCatalog /
                                 ".publication-backups" /
                                 journal.catalogBackupToken;
  const fs::path journalPath =
      roots_.privateCatalog / "publication-journal.json";

  auto fence = profileSnapshots_.tryAcquireInventoryCommitFence(inventory);
  if (!fence) {
    result.retryableInventoryRace = true;
    result.retryPrepared = std::move(prepared);
    result.diagnostics.push_back(storeDiagnostic(
        "skin_package_profile_inventory_changed",
        "profile inventory changed before skin package publication"));
    return result;
  }
  {
    std::scoped_lock lock(stateMutex_);
    if (catalogMutationInFlight_ ||
        stateCatalogGeneration_ != oldCatalog->catalogGeneration ||
        stateSourceGeneration_ != oldCatalog->sourceGeneration) {
      result.retryableInventoryRace = true;
      result.retryPrepared = std::move(prepared);
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_catalog_changed",
          "skin catalog changed before skin package publication"));
      return result;
    }
    catalogMutationInFlight_ = true;
  }
  LockedFlagReset mutationReset(stateMutex_, catalogMutationInFlight_);
  const auto clearMutation = [&] { mutationReset.reset(); };

  const bool physicalMatches =
      visibleCapability->matchesIssuedIdentity() &&
      (!destinationExists ||
       treeMetadataManifest(visible, aliases_) == oldTreeManifest);
  if (!physicalMatches || stop.stop_requested()) {
    clearMutation();
    result.retryableInventoryRace = !stop.stop_requested();
    result.retryPrepared = std::move(prepared);
    result.diagnostics.push_back(
        storeDiagnostic("skin_package_source_changed",
                        "visible skin package changed before publication"));
    return result;
  }

  std::error_code directoryError;
  if (!ensureDirectoryNoFollow(backup.parent_path()) ||
      !ensureDirectoryNoFollow(roots_.privateCatalog)) {
    directoryError = std::make_error_code(std::errc::io_error);
  }
  if (directoryError) {
    clearMutation();
    result.diagnostics.push_back(
        storeDiagnostic("skin_package_publication_storage_failed",
                        "skin package publication storage cannot be prepared"));
    return result;
  }
  if (!catalog_.writeSnapshotFile(catalogStaging, newCatalog,
                                  result.diagnostics) ||
      !catalog_.writeSnapshotFile(catalogBackup, *oldCatalog,
                                  result.diagnostics)) {
    clearMutation();
    return result;
  }
  if (!writeJournal(journalPath, journal, result.diagnostics)) {
    clearMutation();
    return result;
  }

  bool rollbackAttempted = false;
  bool rollbackSucceeded = false;
  const auto rollback = [&]() noexcept -> bool {
    if (rollbackAttempted) {
      return rollbackSucceeded;
    }
    rollbackAttempted = true;
    try {
      bool visibleRestored = true;
      const bool newVisible =
          treeDigestMatches(visible, prepared.packageId(),
                            journal.newTreeDigest, roots_, aliases_);
      if (newVisible) {
        const fs::path rollbackQuarantine =
            roots_.visiblePackages.parent_path() / ".skin-recovery-quarantine" /
            (operationId + "-in-process-rollback-visible");
        std::vector<SkinDiagnostic> relocationDiagnostics;
        visibleRestored =
            ensureDirectoryNoFollow(rollbackQuarantine.parent_path()) &&
            prepared.relocateVisibleOwnershipTo(rollbackQuarantine,
                                                relocationDiagnostics);
      }
      if (destinationExists &&
          !treeDigestMatches(visible, prepared.packageId(), oldTreeDigest,
                             roots_, aliases_)) {
        visibleRestored = treeDigestMatches(backup, prepared.packageId(),
                                            oldTreeDigest, roots_, aliases_) &&
                          visibleCapability->matchesIssuedIdentity() &&
                          visibleCapability->renameTo(visible) &&
                          visibleRestored;
      }
      if (!destinationExists) {
        std::error_code visibleError;
        const auto status = fs::symlink_status(visible, visibleError);
        visibleRestored =
            (status.type() == fs::file_type::not_found ||
             visibleError == std::errc::no_such_file_or_directory) &&
            visibleRestored;
      }

      std::vector<SkinDiagnostic> rollbackDiagnostics;
      const bool catalogRestored =
          catalog_.replaceSnapshotDurably(*oldCatalog, rollbackDiagnostics);
      const bool generationRestored = visibleRestored && catalogRestored;
      bool finalized = false;
      if (generationRestored) {
        const bool stagingClean = quarantineTree(
            catalogStaging, roots_.privateCatalog / ".recovery-quarantine",
            operationId, "in-process-catalog-staging");
        const bool backupClean = quarantineTree(
            catalogBackup, roots_.privateCatalog / ".recovery-quarantine",
            operationId, "in-process-catalog-backup");
        if (stagingClean && backupClean) {
          std::error_code removeError;
          fs::remove(journalPath, removeError);
          std::string syncError;
          finalized = !removeError && atomic_file::syncDirectory(
                                          roots_.privateCatalog, syncError);
        }
      }
      rollbackSucceeded = generationRestored && finalized;
    } catch (...) {
      rollbackSucceeded = false;
    }
    clearMutation();
    if (!rollbackSucceeded) {
      // Keep the journal and every still-useful generation copy. Restart
      // recovery is the only authority allowed to choose a complete
      // generation after an incomplete in-process rollback.
      poisoned_.store(true);
      appendDiagnosticNoThrow(
          result.diagnostics, "skin_package_publication_rollback_failed",
          "skin package publication could not restore and finalize a complete "
          "generation");
    }
    return rollbackSucceeded;
  };
  ScopeRollback rollbackGuard(rollback);

  if (destinationExists) {
    if (!visibleCapability->renameTo(backup)) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_backup_failed",
          "unable to retain the previous visible skin package"));
      rollback();
      return result;
    }
    journal.phase = "visible-backup-parent-synced";
    if (!writeJournal(journalPath, journal, result.diagnostics)) {
      rollback();
      return result;
    }
  }
  if (!prepared.renameVisibleStagingTo(visible, result.diagnostics)) {
    rollback();
    return result;
  }
  journal.phase = "visible-parent-synced";
  if (!writeJournal(journalPath, journal, result.diagnostics)) {
    rollback();
    return result;
  }

  std::string revisionError;
  auto revisionLease = prepared.publishRevision(revisionError);
  if (!revisionLease) {
    result.diagnostics.push_back(
        storeDiagnostic("skin_package_revision_publication_failed",
                        "unable to publish immutable skin package revision"));
    rollback();
    return result;
  }
  using RevisionNode = decltype(revisionLeases_)::node_type;
  using RevisionPinNode = decltype(revisionPins_)::node_type;
  RevisionNode revisionNode;
  RevisionPinNode revisionPinNode;
  try {
    const std::string key =
        revisionKey(revisionLease->revision().package,
                    revisionLease->revision().lowercaseSha256);
    decltype(revisionLeases_) reservedRevision;
    decltype(revisionPins_) reservedPin;
    reservedPin.emplace(key, revisionLease->weakPin());
    reservedRevision.emplace(key, std::move(*revisionLease));
    revisionPinNode = reservedPin.extract(reservedPin.begin());
    revisionNode = reservedRevision.extract(reservedRevision.begin());
  } catch (...) {
    rollback();
    return result;
  }
  journal.phase = "revision-parent-synced";
  if (!writeJournal(journalPath, journal, result.diagnostics)) {
    rollback();
    return result;
  }

  if (!catalog_.replaceSnapshotDurably(std::move(newCatalog),
                                       result.diagnostics)) {
    rollback();
    return result;
  }
  journal.phase = "catalog-parent-synced";
  if (!writeJournal(journalPath, journal, result.diagnostics)) {
    rollback();
    return result;
  }

  prepared.releaseVisibleOwnership();
  {
    std::scoped_lock lock(stateMutex_);
    std::erase_if(revisionLeases_, [&](const auto &item) {
      return item.second.revision().package.collisionKey ==
             prepared.packageId().collisionKey;
    });
    revisionLeases_.insert(std::move(revisionNode));
    revisionPins_.insert(std::move(revisionPinNode));
    std::erase_if(activations_, [&](const auto &item) {
      return item.second.entry.package.collisionKey ==
             prepared.packageId().collisionKey;
    });
    stateCatalogGeneration_ = oldCatalog->catalogGeneration + 1;
    stateSourceGeneration_ = oldCatalog->sourceGeneration + 1;
    catalogMutationInFlight_ = false;
  }
  mutationReset.dismiss();
  rollbackGuard.dismiss();
  const bool cleanupComplete =
      quarantineTree(*visibleCapability,
                     roots_.visiblePackages.parent_path() /
                         ".skin-recovery-quarantine",
                     operationId, "committed-backup") &&
      quarantineTree(catalogStaging,
                     roots_.privateCatalog / ".recovery-quarantine",
                     operationId, "committed-catalog-staging") &&
      quarantineTree(catalogBackup,
                     roots_.privateCatalog / ".recovery-quarantine",
                     operationId, "committed-catalog-backup");
  bool journalRemoved = false;
  if (cleanupComplete) {
    fs::remove(journalPath, directoryError);
    std::string syncError;
    journalRemoved = !directoryError && atomic_file::syncDirectory(
                                            roots_.privateCatalog, syncError);
  }
  if (!journalRemoved) {
    poisoned_.store(true);
    result.diagnostics.push_back(
        storeDiagnostic("skin_package_journal_cleanup_failed",
                        "published package retained a recovery journal"));
  }
  result.published = true;
  return result;
}

ScanPackagesResult SkinPackageStore::rescanVisibleSources(
    std::stop_token stop, SkinProgressCallback progress,
    ProfileInventorySnapshot inventory, SkinEntryValidator &validator) {
  ScanPackagesResult result;
  if (poisoned_.load()) {
    result.diagnostics.push_back(storeDiagnostic(
        "skin_package_store_poisoned",
        "skin package storage requires a successful restart recovery"));
    return result;
  }

  struct ScannedPackage {
    SkinPackageId package;
    std::optional<RetainedTreeCapability> visibleCapability;
    std::optional<std::vector<TreeMetadataRecord>> visibleManifest;
    std::optional<PreparedSkinRevision> revision;
    std::vector<SkinCatalogEntrySnapshot> entries;
    std::vector<SkinDiagnostic> diagnostics;
  };
  std::vector<ScannedPackage> scanned;
  const auto oldCatalog = catalog_.snapshot();
  std::error_code iteratorError;
  if (!ensureDirectoryNoFollow(roots_.visiblePackages)) {
    iteratorError = std::make_error_code(std::errc::io_error);
  }
  for (fs::directory_iterator iterator(roots_.visiblePackages, iteratorError),
       end;
       !iteratorError && iterator != end; ++iterator) {
    if (stop.stop_requested()) {
      result.cancelled = true;
      return result;
    }
    const auto nameUtf8 = iterator->path().filename().generic_u8string();
    const std::string directoryName(
        reinterpret_cast<const char *>(nameUtf8.data()), nameUtf8.size());
    const auto packageResult = normalizePackageId(directoryName);
    std::error_code typeError;
    if (!iterator->is_directory(typeError) || typeError ||
        aliases_.inspectNoFollow(iterator->path()) !=
            SkinRejectedLinkKind::None) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_loose_root_entry",
          "the Skins root contains a loose, linked, or unsupported entry"));
      continue;
    }
    if (!packageResult.package) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_directory_invalid",
          "a direct child of the Skins root has an invalid package name"));
      continue;
    }
    ScannedPackage work{.package = *packageResult.package};
    work.visibleCapability = RetainedTreeCapability::issue(iterator->path());
    if (!work.visibleCapability || !work.visibleCapability->existed()) {
      work.diagnostics.push_back(
          storeDiagnostic("skin_package_source_changed",
                          "a visible skin package changed before inspection"));
      scanned.push_back(std::move(work));
      continue;
    }
    work.visibleManifest = treeMetadataManifest(iterator->path(), aliases_);
    if (!work.visibleManifest) {
      work.diagnostics.push_back(storeDiagnostic(
          "skin_package_source_changed",
          "a visible skin package could not be inventoried safely"));
      scanned.push_back(std::move(work));
      continue;
    }
    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot =
        snapshotter.snapshot(iterator->path(), work.package, stop, progress);
    work.diagnostics.insert(
        work.diagnostics.end(),
        std::make_move_iterator(snapshot.diagnostics.begin()),
        std::make_move_iterator(snapshot.diagnostics.end()));
    if (snapshot.cancelled || stop.stop_requested()) {
      result.cancelled = true;
      return result;
    }
    if (!snapshot.prepared) {
      scanned.push_back(std::move(work));
      continue;
    }
    if (!work.visibleCapability->matchesIssuedIdentity() ||
        treeMetadataManifest(iterator->path(), aliases_) !=
            work.visibleManifest) {
      work.diagnostics.push_back(
          storeDiagnostic("skin_package_source_changed",
                          "a visible skin package changed during inspection"));
      scanned.push_back(std::move(work));
      continue;
    }
    auto entries =
        discoverEntries(snapshot.prepared->readView(), stop, work.diagnostics);
    if (!entries) {
      if (stop.stop_requested()) {
        result.cancelled = true;
        return result;
      }
      scanned.push_back(std::move(work));
      continue;
    }
    for (const SkinEntryId &entry : *entries) {
      auto validation = validator.validate(snapshot.prepared->readView(), entry,
                                           nullptr, stop);
      if (validation.cancelled || stop.stop_requested()) {
        result.cancelled = true;
        return result;
      }
      if (validation.disposition != SkinValidationDisposition::Selectable7Key) {
        const auto previous = std::ranges::find_if(
            oldCatalog->entries,
            [&](const SkinCatalogEntrySnapshot &candidate) {
              return candidate.entry == entry &&
                     candidate.validation ==
                         SkinValidationDisposition::Selectable7Key;
            });
        if (previous != oldCatalog->entries.end()) {
          SkinCatalogEntrySnapshot retained = *previous;
          retained.diagnostics.insert(
              retained.diagnostics.end(),
              std::make_move_iterator(validation.diagnostics.begin()),
              std::make_move_iterator(validation.diagnostics.end()));
          retained.diagnostics.push_back(storeDiagnostic(
              "skin_package_last_known_good_retained",
              "an invalid visible edit retained the last validated revision"));
          work.entries.push_back(std::move(retained));
          result.discoveredEntries.push_back(entry);
          continue;
        }
      }
      SkinCatalogEntrySnapshot catalogEntry{
          .entry = entry,
          .revisionDigest = snapshot.prepared->revision().lowercaseSha256,
          .validation = validation.disposition,
          .metadata = std::move(validation.metadata),
          .diagnostics = std::move(validation.diagnostics)};
      if (lowercaseSha256(validation.configurationDigest)) {
        catalogEntry.validatedConfigurationDigests.push_back(
            std::move(validation.configurationDigest));
      }
      work.entries.push_back(std::move(catalogEntry));
      result.discoveredEntries.push_back(entry);
    }

    for (const VersionedSkinProfileSettings &profile : inventory.profiles) {
      if (!profile.settings.selected7KeyEntry ||
          profile.settings.selected7KeyEntry->package != work.package) {
        continue;
      }
      const SkinEntryId &selected = *profile.settings.selected7KeyEntry;
      const auto selectedEntry = std::ranges::find_if(
          work.entries, [&selected](const SkinCatalogEntrySnapshot &candidate) {
            return candidate.entry == selected;
          });
      if (selectedEntry == work.entries.end()) {
        work.diagnostics.push_back(storeDiagnostic(
            "skin_package_selected_entry_missing",
            "a selected skin entry is absent from the visible package"));
        continue;
      }
      const auto desired = profile.settings.entries.find(selected);
      auto validation = validator.validate(
          snapshot.prepared->readView(), selected,
          desired == profile.settings.entries.end() ? nullptr
                                                    : &desired->second,
          stop);
      if (validation.cancelled || stop.stop_requested()) {
        result.cancelled = true;
        return result;
      }
      selectedEntry->diagnostics.insert(
          selectedEntry->diagnostics.end(),
          std::make_move_iterator(validation.diagnostics.begin()),
          std::make_move_iterator(validation.diagnostics.end()));
      if (validation.disposition == SkinValidationDisposition::Selectable7Key &&
          validation.reconciledSettings &&
          lowercaseSha256(validation.configurationDigest) &&
          !std::ranges::contains(selectedEntry->validatedConfigurationDigests,
                                 validation.configurationDigest)) {
        selectedEntry->validatedConfigurationDigests.push_back(
            std::move(validation.configurationDigest));
      } else {
        const auto previous = std::ranges::find_if(
            oldCatalog->entries,
            [&](const SkinCatalogEntrySnapshot &candidate) {
              return candidate.entry == selected &&
                     candidate.validation ==
                         SkinValidationDisposition::Selectable7Key;
            });
        if (previous != oldCatalog->entries.end()) {
          SkinCatalogEntrySnapshot retained = *previous;
          retained.diagnostics.insert(retained.diagnostics.end(),
                                      selectedEntry->diagnostics.begin(),
                                      selectedEntry->diagnostics.end());
          retained.diagnostics.push_back(storeDiagnostic(
              "skin_package_last_known_good_retained",
              "an invalid selected configuration retained the last validated "
              "revision"));
          *selectedEntry = std::move(retained);
        }
      }
    }
    work.revision = std::move(snapshot.prepared);
    scanned.push_back(std::move(work));
  }
  if (iteratorError) {
    result.diagnostics.push_back(
        storeDiagnostic("skin_package_scan_failed",
                        "unable to enumerate the visible skin package root"));
    return result;
  }

  SkinPackageCatalogSnapshot next;
  next.catalogGeneration = oldCatalog->catalogGeneration + 1;
  next.sourceGeneration = oldCatalog->sourceGeneration + 1;
  for (ScannedPackage &work : scanned) {
    next.packages.push_back(work.package);
    if (work.revision) {
      next.entries.insert(next.entries.end(), work.entries.begin(),
                          work.entries.end());
    } else {
      for (const SkinCatalogEntrySnapshot &oldEntry : oldCatalog->entries) {
        if (oldEntry.entry.package != work.package) {
          continue;
        }
        SkinCatalogEntrySnapshot retained = oldEntry;
        retained.diagnostics.insert(retained.diagnostics.end(),
                                    work.diagnostics.begin(),
                                    work.diagnostics.end());
        next.entries.push_back(std::move(retained));
      }
    }
    result.diagnostics.insert(result.diagnostics.end(),
                              work.diagnostics.begin(), work.diagnostics.end());
  }
  std::ranges::sort(next.packages, {}, &SkinPackageId::collisionKey);
  std::ranges::sort(next.entries, {},
                    [](const SkinCatalogEntrySnapshot &entry) {
                      return entry.entry.collisionKey;
                    });

  std::map<std::string, SkinRevisionLease, std::less<>> published;
  {
    std::scoped_lock lock(stateMutex_);
    for (const SkinCatalogEntrySnapshot &entry : next.entries) {
      const std::string key =
          revisionKey(entry.entry.package, entry.revisionDigest);
      const auto retained = revisionLeases_.find(key);
      if (retained != revisionLeases_.end() && !published.contains(key)) {
        published.emplace(key, retained->second.clone());
      }
    }
  }

  auto fence = profileSnapshots_.tryAcquireInventoryCommitFence(inventory);
  if (!fence) {
    result.retryableInventoryRace = true;
    result.diagnostics.push_back(storeDiagnostic(
        "skin_package_profile_inventory_changed",
        "profile inventory changed before the visible scan commit"));
    return result;
  }
  const auto current = catalog_.snapshot();
  if (current->catalogGeneration != oldCatalog->catalogGeneration ||
      current->sourceGeneration != oldCatalog->sourceGeneration) {
    result.retryableInventoryRace = true;
    result.diagnostics.push_back(
        storeDiagnostic("skin_package_catalog_changed",
                        "skin catalog changed before the visible scan commit"));
    return result;
  }
  for (ScannedPackage &work : scanned) {
    if (!work.revision) {
      continue;
    }
    if (!work.visibleCapability ||
        !work.visibleCapability->matchesIssuedIdentity() ||
        treeMetadataManifest(
            roots_.visiblePackages / work.package.directoryName, aliases_) !=
            work.visibleManifest) {
      result.retryableInventoryRace = !stop.stop_requested();
      result.cancelled = stop.stop_requested();
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_source_changed",
          "a visible skin package changed before the scan commit"));
      return result;
    }
  }

  {
    std::scoped_lock lock(stateMutex_);
    if (catalogMutationInFlight_ ||
        stateCatalogGeneration_ != oldCatalog->catalogGeneration ||
        stateSourceGeneration_ != oldCatalog->sourceGeneration) {
      result.retryableInventoryRace = true;
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_catalog_changed",
          "skin catalog changed before the visible scan commit"));
      return result;
    }
    catalogMutationInFlight_ = true;
  }
  LockedFlagReset mutationReset(stateMutex_, catalogMutationInFlight_);

  for (ScannedPackage &work : scanned) {
    if (!work.revision) {
      continue;
    }
    std::string error;
    auto lease = std::move(*work.revision).publish(error);
    if (!lease) {
      {
        std::scoped_lock lock(stateMutex_);
        catalogMutationInFlight_ = false;
      }
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_revision_publication_failed",
          "unable to publish a scanned immutable skin revision"));
      return result;
    }
    try {
      published.emplace(
          revisionKey(work.package, lease->revision().lowercaseSha256),
          std::move(*lease));
    } catch (...) {
      std::scoped_lock lock(stateMutex_);
      catalogMutationInFlight_ = false;
      return result;
    }
  }
  std::map<std::string, SkinRevisionWeakPin, std::less<>> publishedPins;
  try {
    for (const auto &[key, lease] : published) {
      publishedPins.emplace(key, lease.weakPin());
    }
  } catch (...) {
    std::scoped_lock lock(stateMutex_);
    catalogMutationInFlight_ = false;
    return result;
  }
  if (!catalog_.replaceSnapshotDurably(next, result.diagnostics)) {
    std::scoped_lock lock(stateMutex_);
    catalogMutationInFlight_ = false;
    return result;
  }
  {
    std::scoped_lock lock(stateMutex_);
    revisionLeases_.clear();
    revisionLeases_.merge(published);
    for (const auto &[key, pin] : publishedPins) {
      (void)pin;
      revisionPins_.erase(key);
    }
    revisionPins_.merge(publishedPins);
    stateCatalogGeneration_ = next.catalogGeneration;
    stateSourceGeneration_ = next.sourceGeneration;
    catalogMutationInFlight_ = false;
    std::erase_if(activations_, [&](const auto &item) {
      return !std::ranges::any_of(
          next.entries, [&](const SkinCatalogEntrySnapshot &entry) {
            return entry.entry == item.second.entry &&
                   entry.validation ==
                       SkinValidationDisposition::Selectable7Key &&
                   entry.revisionDigest ==
                       item.second.revision.revision().lowercaseSha256 &&
                   std::ranges::contains(entry.validatedConfigurationDigests,
                                         item.second.configurationDigest);
          });
    });
  }
  mutationReset.dismiss();
  result.sourceGeneration = next.sourceGeneration;
  return result;
}

PrepareActivationResult SkinPackageStore::prepareActivation(
    const VersionedSkinProfileSettings &base, const SkinEntryId &entry,
    SkinProfileSettings candidateProfileSettings, SkinEntryValidator &validator,
    std::stop_token stop) {
  PrepareActivationResult result;
  if (poisoned_.load()) {
    result.diagnostics.push_back(storeDiagnostic(
        "skin_package_store_poisoned",
        "skin package storage requires a successful restart recovery"));
    return result;
  }
  const auto catalogSnapshot = catalog_.snapshot();
  const auto catalogEntry =
      std::ranges::find_if(catalogSnapshot->entries,
                           [&entry](const SkinCatalogEntrySnapshot &candidate) {
                             return candidate.entry == entry;
                           });
  {
    std::scoped_lock lock(stateMutex_);
    for (const auto &[key, activation] : activations_) {
      const std::string prefix = base.profileId.opaque + std::string(1, '\0');
      if (key.starts_with(prefix) && activation.entry == entry) {
        result.previousActivation = cloneActivation(activation);
        break;
      }
    }
  }
  if (catalogEntry == catalogSnapshot->entries.end()) {
    result.diagnostics.push_back(storeDiagnostic(
        "skin_activation_entry_missing",
        "the requested skin entry is not present in the current catalog"));
    return result;
  }

  std::optional<SkinRevisionLease> lease;
  std::uint64_t capturedCatalogGeneration = 0;
  std::uint64_t capturedSourceGeneration = 0;
  {
    std::scoped_lock lock(stateMutex_);
    capturedCatalogGeneration = stateCatalogGeneration_;
    capturedSourceGeneration = stateSourceGeneration_;
    const auto revision = revisionLeases_.find(
        revisionKey(entry.package, catalogEntry->revisionDigest));
    if (revision != revisionLeases_.end()) {
      lease = revision->second.clone();
    }
  }
  if (!lease) {
    result.diagnostics.push_back(storeDiagnostic(
        "skin_activation_revision_missing",
        "the requested immutable skin revision is unavailable"));
    return result;
  }
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }
  candidateProfileSettings.sanitize();
  const auto desired = candidateProfileSettings.entries.find(entry);
  auto validation = validator.validate(
      lease->readView(), entry,
      desired == candidateProfileSettings.entries.end() ? nullptr
                                                        : &desired->second,
      stop);
  result.diagnostics.insert(
      result.diagnostics.end(),
      std::make_move_iterator(validation.diagnostics.begin()),
      std::make_move_iterator(validation.diagnostics.end()));
  if (validation.cancelled || stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }
  if (validation.disposition != SkinValidationDisposition::Selectable7Key ||
      !validation.reconciledSettings ||
      !lowercaseSha256(validation.configurationDigest)) {
    result.diagnostics.push_back(
        storeDiagnostic("skin_activation_configuration_invalid",
                        "the requested skin configuration is not selectable"));
    return result;
  }
  candidateProfileSettings.selected7KeyEntry = entry;
  candidateProfileSettings.entries.insert_or_assign(
      entry, *validation.reconciledSettings);
  candidateProfileSettings.sanitize();
  result.prepared = PreparedSkinActivation{
      .sourceGeneration = capturedSourceGeneration,
      .catalogGeneration = capturedCatalogGeneration,
      .expectedProfileGeneration = base.generation,
      .profileId = base.profileId,
      .activation =
          ValidatedSkinActivation{
              .revision = std::move(*lease),
              .entry = entry,
              .reconciledSettings = *validation.reconciledSettings,
              .configurationDigest = std::move(validation.configurationDigest)},
      .candidateProfileSettings = std::move(candidateProfileSettings)};
  return result;
}

CommitActivationResult SkinPackageStore::beginPreparedActivationCommit(
    PreparedSkinActivation &&prepared, ISkinProfileSettingsOwner &owner) {
  CommitActivationResult result;
  if (poisoned_.load()) {
    result.diagnostics.push_back(storeDiagnostic(
        "skin_package_store_poisoned",
        "skin package storage requires a successful restart recovery"));
    return result;
  }
  {
    std::scoped_lock lock(stateMutex_);
    if (stateSourceGeneration_ != prepared.sourceGeneration ||
        stateCatalogGeneration_ != prepared.catalogGeneration) {
      result.disposition = ActivationCommitDisposition::SourceGenerationChanged;
      result.diagnostics.push_back(
          storeDiagnostic("skin_activation_source_changed",
                          "skin source changed after activation validation"));
      return result;
    }
  }
  const VersionedSkinProfileSettings ownerSnapshot =
      owner.snapshot(prepared.profileId);
  if (ownerSnapshot.generation != prepared.expectedProfileGeneration) {
    result.disposition = ActivationCommitDisposition::ProfileGenerationChanged;
    result.profileSnapshot = ownerSnapshot;
    return result;
  }
  SkinProfileSettings ownerCandidate = prepared.candidateProfileSettings;
  const SkinProfileId profile = prepared.profileId;
  const std::uint64_t expectedProfileGeneration =
      prepared.expectedProfileGeneration;
  const std::uint64_t sourceGeneration = prepared.sourceGeneration;
  const std::uint64_t catalogGeneration = prepared.catalogGeneration;
  const auto currentCatalog = catalog_.snapshot();
  SkinPackageCatalogSnapshot catalogUpdate = *currentCatalog;
  bool catalogChanged = false;
  auto catalogEntry = std::ranges::find_if(
      catalogUpdate.entries, [&](const SkinCatalogEntrySnapshot &entry) {
        return entry.entry == prepared.activation.entry &&
               entry.revisionDigest ==
                   prepared.activation.revision.revision().lowercaseSha256;
      });
  if (catalogEntry != catalogUpdate.entries.end() &&
      !std::ranges::contains(catalogEntry->validatedConfigurationDigests,
                             prepared.activation.configurationDigest)) {
    catalogEntry->validatedConfigurationDigests.push_back(
        prepared.activation.configurationDigest);
    ++catalogUpdate.catalogGeneration;
    catalogChanged = true;
  }
  ValidatedSkinActivation terminalActivation =
      cloneActivation(prepared.activation);
  ActivationMap reservedActivation;
  const std::string key =
      activationKey(profile, prepared.activation.entry,
                    prepared.activation.configurationDigest);
  reservedActivation.emplace(key, std::move(prepared.activation));
  auto activationNode = reservedActivation.extract(reservedActivation.begin());
  std::uint64_t ticket = 0;
  {
    std::scoped_lock lock(stateMutex_);
    do {
      ++nextActivationCommitTicket_;
    } while (nextActivationCommitTicket_ == 0 ||
             pendingActivationCommits_.contains(nextActivationCommitTicket_));
    ticket = nextActivationCommitTicket_;
    pendingActivationCommits_.emplace(
        ticket, PendingActivationCommit{
                    .ownerTicket = 0,
                    .sourceGeneration = sourceGeneration,
                    .catalogGeneration = catalogGeneration,
                    .profileId = profile,
                    .activationNode = std::move(activationNode),
                    .terminalActivation = std::move(terminalActivation),
                    .catalogUpdate = std::move(catalogUpdate),
                    .catalogChanged = catalogChanged});
  }
  SkinProfileCommitResult ownerResult;
  try {
    ownerResult = owner.beginCommit(profile, expectedProfileGeneration,
                                    std::move(ownerCandidate));
  } catch (...) {
    {
      std::scoped_lock lock(stateMutex_);
      pendingActivationCommits_.erase(ticket);
    }
    appendDiagnosticNoThrow(
        result.diagnostics, "skin_activation_owner_begin_failed",
        "profile persistence rejected the activation commit before admission");
    return result;
  }
  if (ownerResult.status ==
          SkinProfileCommitResult::Status::GenerationChanged ||
      ownerResult.generationChanged) {
    {
      std::scoped_lock lock(stateMutex_);
      pendingActivationCommits_.erase(ticket);
    }
    result.disposition = ActivationCommitDisposition::ProfileGenerationChanged;
    result.profileSnapshot = std::move(ownerResult.snapshot);
    return result;
  }
  if (ownerResult.status == SkinProfileCommitResult::Status::RetryableFailure) {
    if (ownerResult.failure) {
      result.diagnostics.push_back(std::move(*ownerResult.failure));
    }
    {
      std::scoped_lock lock(stateMutex_);
      pendingActivationCommits_.erase(ticket);
    }
    result.disposition = ActivationCommitDisposition::RetainedPrevious;
    return result;
  }
  if (ownerResult.ticket == 0) {
    result.diagnostics.push_back(storeDiagnostic(
        "skin_activation_owner_ticket_invalid",
        "profile persistence did not issue a valid commit ticket"));
    {
      std::scoped_lock lock(stateMutex_);
      pendingActivationCommits_.erase(ticket);
    }
    return result;
  }
  {
    std::scoped_lock lock(stateMutex_);
    pendingActivationCommits_.at(ticket).ownerTicket = ownerResult.ticket;
  }
  result.disposition = ActivationCommitDisposition::PendingProfileSave;
  result.ticket = ticket;
  return result;
}

CommitActivationResult SkinPackageStore::pollPreparedActivationCommit(
    std::uint64_t ticket, ISkinProfileSettingsOwner &owner) {
  CommitActivationResult result;
  result.ticket = ticket;
  std::uint64_t ownerTicket = 0;
  {
    std::scoped_lock lock(stateMutex_);
    const auto pending = pendingActivationCommits_.find(ticket);
    if (pending == pendingActivationCommits_.end()) {
      result.diagnostics.push_back(
          storeDiagnostic("skin_activation_ticket_unknown",
                          "the activation commit ticket is no longer pending"));
      return result;
    }
    ownerTicket = pending->second.ownerTicket;
  }
  SkinProfileCommitResult ownerResult;
  try {
    ownerResult = owner.pollCommit(ownerTicket);
  } catch (...) {
    result.disposition = ActivationCommitDisposition::PendingProfileSave;
    appendDiagnosticNoThrow(
        result.diagnostics, "skin_activation_owner_poll_failed",
        "profile persistence status could not be read; the commit remains "
        "pending");
    return result;
  }
  if (ownerResult.status == SkinProfileCommitResult::Status::Pending) {
    result.disposition = ActivationCommitDisposition::PendingProfileSave;
    return result;
  }
  if (ownerResult.status ==
          SkinProfileCommitResult::Status::GenerationChanged ||
      ownerResult.generationChanged) {
    {
      std::scoped_lock lock(stateMutex_);
      pendingActivationCommits_.erase(ticket);
    }
    result.disposition = ActivationCommitDisposition::ProfileGenerationChanged;
    result.profileSnapshot = std::move(ownerResult.snapshot);
    owner.acknowledgeCommit(ownerTicket);
    return result;
  }
  if (ownerResult.status == SkinProfileCommitResult::Status::RetryableFailure) {
    if (ownerResult.failure) {
      result.diagnostics.push_back(std::move(*ownerResult.failure));
    }
    {
      std::scoped_lock lock(stateMutex_);
      pendingActivationCommits_.erase(ticket);
    }
    result.disposition = ActivationCommitDisposition::RetainedPrevious;
    owner.acknowledgeCommit(ownerTicket);
    return result;
  }

  bool sourceChanged = false;
  {
    std::scoped_lock lock(stateMutex_);
    const auto pending = pendingActivationCommits_.find(ticket);
    if (pending == pendingActivationCommits_.end()) {
      result.diagnostics.push_back(
          storeDiagnostic("skin_activation_ticket_unknown",
                          "the activation commit ticket is no longer pending"));
      return result;
    }
    PendingActivationCommit &commit = pending->second;
    sourceChanged = catalogMutationInFlight_ ||
                    stateSourceGeneration_ != commit.sourceGeneration ||
                    stateCatalogGeneration_ != commit.catalogGeneration;
    if (!sourceChanged && commit.catalogChanged) {
      if (!catalog_.replaceSnapshotAsyncIfGeneration(
              commit.catalogGeneration, commit.sourceGeneration,
              std::move(commit.catalogUpdate))) {
        sourceChanged = true;
      } else {
        stateCatalogGeneration_ = commit.catalogGeneration + 1;
      }
    }
    if (!sourceChanged) {
      const std::string profilePrefix =
          commit.profileId.opaque + std::string(1, '\0');
      std::erase_if(activations_, [&](const auto &item) {
        return item.first.starts_with(profilePrefix);
      });
      activations_.insert(std::move(commit.activationNode));
      result.activation = std::move(commit.terminalActivation);
    }
    pendingActivationCommits_.erase(pending);
  }
  if (sourceChanged) {
    result.disposition =
        ActivationCommitDisposition::ProfileCommittedNeedsRevalidation;
    result.profileSnapshot = std::move(ownerResult.snapshot);
  } else {
    result.disposition = ActivationCommitDisposition::ActivatedRequested;
    result.profileSnapshot = std::move(ownerResult.snapshot);
  }
  owner.acknowledgeCommit(ownerTicket);
  return result;
}

AcquireActivationResult SkinPackageStore::acquireValidatedActivation(
    const SkinProfileId &profile, const SkinEntryId &entry,
    std::string_view configurationDigest) {
  AcquireActivationResult result;
  const auto current = catalog_.snapshot();
  const auto catalogEntry = std::ranges::find_if(
      current->entries,
      [&entry, configurationDigest](const SkinCatalogEntrySnapshot &candidate) {
        return candidate.entry == entry &&
               candidate.validation ==
                   SkinValidationDisposition::Selectable7Key &&
               std::ranges::contains(candidate.validatedConfigurationDigests,
                                     configurationDigest);
      });
  if (catalogEntry == current->entries.end()) {
    return result;
  }
  std::scoped_lock lock(stateMutex_);
  if (catalogMutationInFlight_ ||
      stateCatalogGeneration_ != current->catalogGeneration ||
      stateSourceGeneration_ != current->sourceGeneration) {
    return result;
  }
  const auto activation =
      activations_.find(activationKey(profile, entry, configurationDigest));
  if (activation != activations_.end() &&
      activation->second.revision.revision().lowercaseSha256 ==
          catalogEntry->revisionDigest) {
    result.activation = cloneActivation(activation->second);
  }
  return result;
}

RemovePackageResult
SkinPackageStore::removePackage(const SkinPackageId &package,
                                std::stop_token stop) {
  RemovePackageResult result{.package = package};
  const auto normalized = normalizePackageId(package.directoryName);
  if (!normalized.package || *normalized.package != package) {
    result.diagnostics.push_back(
        storeDiagnostic("skin_package_identity_invalid",
                        "the requested skin package identity is invalid"));
    return result;
  }
  if (poisoned_.load() || stop.stop_requested()) {
    if (poisoned_.load()) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_store_poisoned",
          "skin package storage requires a successful restart recovery"));
    }
    result.cancelled = true;
    return result;
  }
  const auto oldCatalog = catalog_.snapshot();
  SkinPackageCatalogSnapshot next = *oldCatalog;
  const bool cataloged =
      std::ranges::any_of(next.packages, [&](const SkinPackageId &candidate) {
        return candidate.collisionKey == package.collisionKey;
      });
  std::erase_if(next.packages, [&](const SkinPackageId &candidate) {
    return candidate.collisionKey == package.collisionKey;
  });
  std::erase_if(next.entries, [&](const SkinCatalogEntrySnapshot &entry) {
    return entry.entry.package.collisionKey == package.collisionKey;
  });
  ++next.catalogGeneration;
  ++next.sourceGeneration;

  const fs::path visible = roots_.visiblePackages / package.directoryName;
  const std::string operation = nextOperationId();
  const fs::path retained = roots_.visiblePackages.parent_path() /
                            ".skin-removal-staging" / operation /
                            package.directoryName;
  auto visibleCapability = RetainedTreeCapability::issue(visible);
  if (!visibleCapability) {
    result.diagnostics.push_back(
        storeDiagnostic("skin_package_remove_inspection_failed",
                        "the visible skin package cannot be inspected"));
    return result;
  }
  const bool exists = visibleCapability->existed();
  std::error_code error;
  std::string oldTreeDigest;
  std::optional<std::vector<TreeMetadataRecord>> oldTreeManifest;
  if (exists) {
    oldTreeManifest = treeMetadataManifest(visible, aliases_);
    auto digest = treeDigest(visible, package, roots_, aliases_, stop,
                             result.diagnostics);
    if (!oldTreeManifest || !digest) {
      return result;
    }
    oldTreeDigest = std::move(*digest);
    if (!visibleCapability->matchesIssuedIdentity() ||
        treeMetadataManifest(visible, aliases_) != oldTreeManifest) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_source_changed",
          "visible skin package changed during removal inspection"));
      return result;
    }
  }

  {
    std::scoped_lock lock(stateMutex_);
    if (catalogMutationInFlight_ ||
        stateCatalogGeneration_ != oldCatalog->catalogGeneration ||
        stateSourceGeneration_ != oldCatalog->sourceGeneration) {
      result.diagnostics.push_back(
          storeDiagnostic("skin_package_remove_catalog_changed",
                          "skin catalog changed before package removal"));
      return result;
    }
    catalogMutationInFlight_ = true;
  }
  LockedFlagReset mutationReset(stateMutex_, catalogMutationInFlight_);
  const auto clearMutation = [&] { mutationReset.reset(); };

  bool moved = false;
  const fs::path removalJournal =
      roots_.privateCatalog / "removal-journal.json";
  const fs::path catalogStaging =
      roots_.privateCatalog / ".removal-staging" / (operation + "-new.json");
  const fs::path catalogBackup =
      roots_.privateCatalog / ".removal-backups" / (operation + "-old.json");
  RemovalJournal journal{.operationId = operation,
                         .package = package,
                         .retainedToken = operation,
                         .oldTreeDigest = oldTreeDigest,
                         .catalogStagingToken = operation + "-new.json",
                         .catalogBackupToken = operation + "-old.json",
                         .oldCatalogGeneration = oldCatalog->catalogGeneration,
                         .newCatalogGeneration = next.catalogGeneration,
                         .oldSourceGeneration = oldCatalog->sourceGeneration,
                         .newSourceGeneration = next.sourceGeneration,
                         .oldCatalogDigest =
                             catalog_.snapshotDigest(*oldCatalog),
                         .newCatalogDigest = catalog_.snapshotDigest(next)};
  if (exists &&
      (!catalog_.writeSnapshotFile(catalogStaging, next, result.diagnostics) ||
       !catalog_.writeSnapshotFile(catalogBackup, *oldCatalog,
                                   result.diagnostics) ||
       !writeRemovalJournal(removalJournal, journal, result.diagnostics))) {
    clearMutation();
    return result;
  }

  bool rollbackAttempted = false;
  bool rollbackSucceeded = false;
  const auto rollback = [&]() noexcept -> bool {
    if (rollbackAttempted) {
      return rollbackSucceeded;
    }
    rollbackAttempted = true;
    try {
      bool visibleRestored = true;
      if (exists && !treeDigestMatches(visible, package, oldTreeDigest, roots_,
                                       aliases_)) {
        visibleRestored = treeDigestMatches(retained, package, oldTreeDigest,
                                            roots_, aliases_) &&
                          visibleCapability->matchesIssuedIdentity() &&
                          visibleCapability->renameTo(visible);
      }
      std::vector<SkinDiagnostic> rollbackDiagnostics;
      const bool catalogRestored =
          catalog_.replaceSnapshotDurably(*oldCatalog, rollbackDiagnostics);
      const bool generationRestored = visibleRestored && catalogRestored;
      bool finalized = false;
      if (generationRestored) {
        const bool stagingClean = quarantineTree(
            catalogStaging, roots_.privateCatalog / ".recovery-quarantine",
            operation, "failed-removal-catalog-staging");
        const bool backupClean = quarantineTree(
            catalogBackup, roots_.privateCatalog / ".recovery-quarantine",
            operation, "failed-removal-catalog-backup");
        if (stagingClean && backupClean) {
          std::error_code cleanupError;
          fs::remove(removalJournal, cleanupError);
          std::string syncError;
          finalized = !cleanupError && atomic_file::syncDirectory(
                                           roots_.privateCatalog, syncError);
        }
      }
      rollbackSucceeded = generationRestored && finalized;
    } catch (...) {
      rollbackSucceeded = false;
    }
    clearMutation();
    if (!rollbackSucceeded) {
      poisoned_.store(true);
      appendDiagnosticNoThrow(
          result.diagnostics, "skin_package_remove_rollback_failed",
          "skin package removal could not restore and finalize a complete "
          "generation");
    }
    return rollbackSucceeded;
  };
  ScopeRollback rollbackGuard(rollback);
  if (exists) {
    if (!ensureDirectoryNoFollow(retained.parent_path())) {
      error = std::make_error_code(std::errc::io_error);
    }
    if (error || !visibleCapability->matchesIssuedIdentity() ||
        treeMetadataManifest(visible, aliases_) != oldTreeManifest ||
        !visibleCapability->renameTo(retained)) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_remove_failed",
          "the visible skin package cannot be retained for removal"));
      return result;
    }
    moved = true;
  }
  if (!catalog_.replaceSnapshotDurably(next, result.diagnostics)) {
    rollback();
    return result;
  }
  {
    std::scoped_lock lock(stateMutex_);
    std::erase_if(revisionLeases_, [&](const auto &item) {
      return item.second.revision().package.collisionKey ==
             package.collisionKey;
    });
    std::erase_if(activations_, [&](const auto &item) {
      return item.second.entry.package.collisionKey == package.collisionKey;
    });
    stateCatalogGeneration_ = next.catalogGeneration;
    stateSourceGeneration_ = next.sourceGeneration;
    catalogMutationInFlight_ = false;
  }
  mutationReset.dismiss();
  rollbackGuard.dismiss();
  bool cleaned = true;
  if (moved) {
    cleaned = quarantineTree(*visibleCapability,
                             roots_.visiblePackages.parent_path() /
                                 ".skin-recovery-quarantine",
                             operation, "removed-package") &&
              quarantineTree(catalogStaging,
                             roots_.privateCatalog / ".recovery-quarantine",
                             operation, "removed-catalog-staging") &&
              quarantineTree(catalogBackup,
                             roots_.privateCatalog / ".recovery-quarantine",
                             operation, "removed-catalog-backup");
    if (cleaned) {
      fs::remove(removalJournal, error);
      std::string syncError;
      cleaned = !error &&
                atomic_file::syncDirectory(roots_.privateCatalog, syncError);
    }
  }
  if (!cleaned) {
    poisoned_.store(true);
    result.diagnostics.push_back(
        storeDiagnostic("skin_package_remove_cleanup_failed",
                        "removed skin package data could not be quarantined"));
  }
  result.removed = moved || cataloged;
  return result;
}

void SkinPackageStore::removeProfileActivations(const SkinProfileId &profile) {
  const std::string prefix = profile.opaque + std::string(1, '\0');
  std::scoped_lock lock(stateMutex_);
  std::erase_if(activations_, [&](const auto &item) {
    return item.first.starts_with(prefix);
  });
}

void SkinPackageStore::reconcileProfileActivations(
    std::span<const SkinProfileId> existingProfiles) {
  std::set<std::string, std::less<>> retained;
  for (const SkinProfileId &profile : existingProfiles) {
    retained.insert(profile.opaque);
  }
  std::scoped_lock lock(stateMutex_);
  std::erase_if(activations_, [&](const auto &item) {
    const auto separator = item.first.find('\0');
    return separator == std::string::npos ||
           !retained.contains(item.first.substr(0, separator));
  });
}

GarbageCollectionResult SkinPackageStore::collectGarbage() {
  GarbageCollectionResult result;
  if (poisoned_.load()) {
    result.diagnostics.push_back(storeDiagnostic(
        "skin_package_store_poisoned",
        "skin package storage requires a successful restart recovery"));
    return result;
  }
  {
    std::scoped_lock lock(stateMutex_);
    if (catalogMutationInFlight_) {
      result.diagnostics.push_back(
          storeDiagnostic("skin_package_gc_catalog_busy",
                          "skin catalog mutation is already in progress"));
      return result;
    }
    catalogMutationInFlight_ = true;
  }
  LockedFlagReset mutationReset(stateMutex_, catalogMutationInFlight_);
  std::error_code error;
  if (!ensureDirectoryNoFollow(roots_.privateRevisions)) {
    error = std::make_error_code(std::errc::io_error);
  }
  const fs::path gcRoot = roots_.privateRevisions / ".gc-quarantine";
  if (ensureDirectoryNoFollow(gcRoot)) {
    for (fs::directory_iterator stale(gcRoot, error), staleEnd;
         !error && stale != staleEnd; ++stale) {
      std::error_code typeError;
      const auto status = fs::symlink_status(stale->path(), typeError);
      bool removed = false;
      if (!typeError && status.type() == fs::file_type::directory) {
#if defined(_WIN32)
        auto staleCapability = RetainedTreeCapability::issue(stale->path());
        removed = staleCapability && staleCapability->existed() &&
                  staleCapability->removeTreeExact();
#else
        removed = removeDirectoryTreeNoFollow(stale->path());
#endif
      }
      if (!removed) {
        result.diagnostics.push_back(storeDiagnostic(
            "skin_package_gc_retry_failed",
            "a prior quarantined revision could not be cleaned"));
      }
    }
    error.clear();
  }
  for (fs::directory_iterator iterator(roots_.privateRevisions, error), end;
       !error && iterator != end; ++iterator) {
    const auto nameUtf8 = iterator->path().filename().generic_u8string();
    const std::string digest(reinterpret_cast<const char *>(nameUtf8.data()),
                             nameUtf8.size());
    if (!lowercaseSha256(digest)) {
      continue;
    }
    bool retained = false;
    {
      std::scoped_lock lock(stateMutex_);
      retained = std::ranges::any_of(revisionLeases_, [&](const auto &item) {
        return item.second.revision().lowercaseSha256 == digest;
      });
      if (!retained) {
        for (const auto &[key, pin] : revisionPins_) {
          if (key.ends_with(":" + digest) && pin.hasLiveLease()) {
            retained = true;
            break;
          }
        }
      }
    }
    if (retained) {
      continue;
    }
    auto capability = RetainedTreeCapability::issue(iterator->path());
    const auto manifest = treeMetadataManifest(iterator->path(), aliases_);
    if (!capability || !capability->existed() || !manifest ||
        !capability->matchesIssuedIdentity()) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_gc_inspection_failed",
          "an unreferenced skin revision could not be inspected"));
      continue;
    }
    std::uint64_t bytes = 0;
    for (const TreeMetadataRecord &record : *manifest) {
      if (!record.directory) {
        bytes += record.size;
      }
    }
    const std::string operation = nextOperationId();
    const fs::path quarantine = gcRoot / operation / digest;
    if (!ensureDirectoryNoFollow(quarantine.parent_path())) {
      error = std::make_error_code(std::errc::io_error);
    }
    if (error || !capability->renameTo(quarantine)) {
      result.diagnostics.push_back(storeDiagnostic(
          "skin_package_gc_quarantine_failed",
          "an unreferenced skin revision could not be quarantined"));
      error.clear();
      continue;
    }
#if defined(_WIN32)
    const bool deleted = capability->removeTreeExact();
#else
    const bool deleted = removeDirectoryTreeNoFollow(quarantine);
#endif
    if (!deleted) {
      result.diagnostics.push_back(
          storeDiagnostic("skin_package_gc_delete_failed",
                          "a quarantined skin revision could not be deleted"));
      continue;
    }
    {
      std::scoped_lock lock(stateMutex_);
      std::erase_if(revisionPins_, [&](const auto &item) {
        return item.first.ends_with(":" + digest) &&
               !item.second.hasLiveLease();
      });
    }
    ++result.revisionsRemoved;
    result.bytesRemoved += bytes;
  }
  if (error) {
    result.diagnostics.push_back(
        storeDiagnostic("skin_package_gc_enumeration_failed",
                        "private skin revisions could not be enumerated"));
  }
  return result;
}

std::shared_ptr<const SkinPackageCatalogSnapshot>
SkinPackageStore::catalogSnapshot() const noexcept {
  return catalog_.snapshot();
}

} // namespace skin
