#include "SkinTreeSnapshotter.h"

#include "../../FileChecksum.h"
#include "SkinPathPolicy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace skin {
namespace {

namespace fs = std::filesystem;

struct FileMetadata {
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::uint64_t size = 0;
  std::uint64_t links = 0;
  std::int64_t modifiedSeconds = 0;
  std::int64_t modifiedNanoseconds = 0;
  bool directory = false;

  auto operator<=>(const FileMetadata &) const = default;
};

struct InventoryEntry {
  std::string normalizedPath;
  std::string collisionKey;
  fs::path sourcePath;
  FileMetadata metadata;

  bool operator==(const InventoryEntry &other) const {
    return normalizedPath == other.normalizedPath &&
           collisionKey == other.collisionKey && metadata == other.metadata;
  }
};

struct Inventory {
  fs::path rootPath;
  FileMetadata rootMetadata;
  std::vector<InventoryEntry> entries;
  std::uint64_t fileCount = 0;
  std::uint64_t totalBytes = 0;
};

SkinDiagnostic diagnostic(std::string code, std::string message,
                          std::string virtualPath = {}) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = std::move(virtualPath),
          .severity = DiagnosticSeverity::Error};
}

std::string utf8Path(const fs::path &path) {
  const auto value = path.generic_u8string();
  return std::string(reinterpret_cast<const char *>(value.data()),
                     value.size());
}

fs::path pathFromUtf8(std::string_view value) {
  std::u8string utf8;
  utf8.reserve(value.size());
  for (const unsigned char byte : value) {
    utf8.push_back(static_cast<char8_t>(byte));
  }
  return fs::path(utf8);
}

bool report(const SkinProgressCallback &callback, SkinProgress progress,
            std::vector<SkinDiagnostic> &diagnostics) {
  if (!callback) {
    return true;
  }
  try {
    callback(progress);
    return true;
  } catch (...) {
    diagnostics.push_back(
        diagnostic("skin_snapshot_progress_callback_failed",
                   "skin snapshot progress callback raised an exception"));
    return false;
  }
}

#if defined(_WIN32)
bool readMetadata(const fs::path &path, FileMetadata &metadata,
                  std::error_code &error) {
  const fs::file_status status = fs::symlink_status(path, error);
  if (error) {
    return false;
  }
  metadata.directory = fs::is_directory(status);
  if (!metadata.directory && !fs::is_regular_file(status)) {
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  metadata.size = metadata.directory ? 0 : fs::file_size(path, error);
  if (error) {
    return false;
  }
  metadata.links = fs::hard_link_count(path, error);
  if (error) {
    return false;
  }
  const auto modified = fs::last_write_time(path, error);
  if (error) {
    return false;
  }
  metadata.modifiedNanoseconds = modified.time_since_epoch().count();
  return true;
}
#else
bool readMetadata(const fs::path &path, FileMetadata &metadata,
                  std::error_code &error) {
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0) {
    error = std::error_code(errno, std::generic_category());
    return false;
  }
  if (!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode)) {
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  metadata.device = static_cast<std::uint64_t>(status.st_dev);
  metadata.inode = static_cast<std::uint64_t>(status.st_ino);
  metadata.size =
      S_ISREG(status.st_mode) ? static_cast<std::uint64_t>(status.st_size) : 0;
  metadata.links = static_cast<std::uint64_t>(status.st_nlink);
  metadata.directory = S_ISDIR(status.st_mode);
#if defined(__APPLE__)
  metadata.modifiedSeconds = status.st_mtimespec.tv_sec;
  metadata.modifiedNanoseconds = status.st_mtimespec.tv_nsec;
#else
  metadata.modifiedSeconds = status.st_mtim.tv_sec;
  metadata.modifiedNanoseconds = status.st_mtim.tv_nsec;
#endif
  return true;
}
#endif

bool inspectTree(const fs::path &directory, const fs::path &root,
                 const SkinPackageId &package, const SkinAliasDetector &aliases,
                 Inventory &inventory, std::map<std::string, bool> &identities,
                 std::stop_token stop,
                 std::vector<SkinDiagnostic> &diagnostics) {
  std::error_code iteratorError;
  fs::directory_iterator iterator(directory, iteratorError);
  if (iteratorError) {
    diagnostics.push_back(
        diagnostic("skin_snapshot_directory_read_failed",
                   "unable to enumerate the skin source directory"));
    return false;
  }
  for (const fs::directory_entry &entry : iterator) {
    if (stop.stop_requested()) {
      return false;
    }
    const fs::path path = entry.path();
    const fs::path relative = path.lexically_relative(root);
    const std::string authoredRelative = utf8Path(relative);
    const auto normalized = normalizeEntryPath(package, authoredRelative);
    if (!normalized.entry) {
      diagnostics.push_back(diagnostic("skin_snapshot_path_invalid",
                                       normalized.error, authoredRelative));
      return false;
    }

    const SkinRejectedLinkKind rejected = aliases.inspectNoFollow(path);
    if (rejected != SkinRejectedLinkKind::None) {
      diagnostics.push_back(diagnostic(
          "skin_snapshot_link_rejected",
          "skin source contains a link, alias, reparse point, or special node",
          normalized.entry->packageRelativePath));
      return false;
    }

    std::error_code metadataError;
    FileMetadata metadata;
    if (!readMetadata(path, metadata, metadataError)) {
      diagnostics.push_back(
          diagnostic("skin_snapshot_nonregular_rejected",
                     "skin source contains an unreadable or non-regular node",
                     normalized.entry->packageRelativePath));
      return false;
    }
    if (!metadata.directory && metadata.links != 1) {
      diagnostics.push_back(
          diagnostic("skin_snapshot_hard_link_rejected",
                     "skin source contains a multiply-linked regular file",
                     normalized.entry->packageRelativePath));
      return false;
    }
    const auto [identity, inserted] =
        identities.emplace(normalized.entry->collisionKey, metadata.directory);
    if (!inserted) {
      diagnostics.push_back(diagnostic(
          "skin_snapshot_path_collision",
          identity->second == metadata.directory
              ? "skin source contains duplicate normalized paths"
              : "skin source contains a file/directory path collision",
          normalized.entry->packageRelativePath));
      return false;
    }
    inventory.entries.push_back(
        {.normalizedPath = normalized.entry->packageRelativePath,
         .collisionKey = normalized.entry->collisionKey,
         .sourcePath = path,
         .metadata = metadata});
    if (metadata.directory) {
      if (!inspectTree(path, root, package, aliases, inventory, identities,
                       stop, diagnostics)) {
        return false;
      }
      continue;
    }
    if (metadata.size > SkinPackagePolicy::maxRegularFileBytes ||
        inventory.totalBytes >
            SkinPackagePolicy::maxExpandedBytes - metadata.size) {
      diagnostics.push_back(diagnostic(
          "skin_snapshot_size_limit",
          "skin source exceeds the regular-file or expanded-size limit",
          normalized.entry->packageRelativePath));
      return false;
    }
    ++inventory.fileCount;
    inventory.totalBytes += metadata.size;
    if (inventory.fileCount > SkinPackagePolicy::maxFiles) {
      diagnostics.push_back(diagnostic("skin_snapshot_file_count_limit",
                                       "skin source exceeds the file limit"));
      return false;
    }
  }
  return true;
}

std::optional<Inventory>
inventoryTree(const fs::path &root, const SkinPackageId &package,
              const SkinAliasDetector &aliases, std::stop_token stop,
              std::vector<SkinDiagnostic> &diagnostics) {
  if (aliases.inspectNoFollow(root) != SkinRejectedLinkKind::None) {
    diagnostics.push_back(diagnostic(
        "skin_snapshot_root_rejected",
        "skin source root is a link, alias, reparse point, or special node"));
    return std::nullopt;
  }
  FileMetadata rootMetadata;
  std::error_code rootError;
  if (!readMetadata(root, rootMetadata, rootError) || !rootMetadata.directory) {
    diagnostics.push_back(diagnostic("skin_snapshot_root_invalid",
                                     "skin source root is not a directory"));
    return std::nullopt;
  }
  Inventory inventory;
  inventory.rootPath = root;
  inventory.rootMetadata = rootMetadata;
  std::map<std::string, bool> identities;
  if (!inspectTree(root, root, package, aliases, inventory, identities, stop,
                   diagnostics)) {
    return std::nullopt;
  }
  std::sort(inventory.entries.begin(), inventory.entries.end(),
            [](const InventoryEntry &left, const InventoryEntry &right) {
              return left.normalizedPath < right.normalizedPath;
            });
  return inventory;
}

template <typename Integer>
void hashBigEndian(file_checksum::Sha256 &hash, Integer value) {
  std::array<std::byte, sizeof(Integer)> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[bytes.size() - index - 1] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
  hash.update(bytes);
}

void hashText(file_checksum::Sha256 &hash, std::string_view text) {
  hash.update(std::as_bytes(std::span(text.data(), text.size())));
}

bool metadataMatchesOpenFile(
#if defined(_WIN32)
    const fs::path &path,
#else
    int descriptor,
#endif
    const FileMetadata &expected) {
#if defined(_WIN32)
  FileMetadata actual;
  std::error_code error;
  return readMetadata(path, actual, error) && actual == expected;
#else
  struct stat status{};
  if (::fstat(descriptor, &status) != 0) {
    return false;
  }
  FileMetadata actual;
  actual.device = static_cast<std::uint64_t>(status.st_dev);
  actual.inode = static_cast<std::uint64_t>(status.st_ino);
  actual.size =
      S_ISREG(status.st_mode) ? static_cast<std::uint64_t>(status.st_size) : 0;
  actual.links = static_cast<std::uint64_t>(status.st_nlink);
  actual.directory = S_ISDIR(status.st_mode);
#if defined(__APPLE__)
  actual.modifiedSeconds = status.st_mtimespec.tv_sec;
  actual.modifiedNanoseconds = status.st_mtimespec.tv_nsec;
#else
  actual.modifiedSeconds = status.st_mtim.tv_sec;
  actual.modifiedNanoseconds = status.st_mtim.tv_nsec;
#endif
  return actual == expected;
#endif
}

#if !defined(_WIN32)
bool fsyncDirectory(const fs::path &directory) {
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
  if (descriptor < 0) {
    return false;
  }
  const bool ok = ::fsync(descriptor) == 0;
  ::close(descriptor);
  return ok;
}

bool fsyncDirectoryTree(const fs::path &root) {
  std::vector<fs::path> directories{root};
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; ++iterator) {
    if (iterator->is_directory(error)) {
      directories.push_back(iterator->path());
    }
  }
  if (error) {
    return false;
  }
  for (auto iterator = directories.rbegin(); iterator != directories.rend();
       ++iterator) {
    if (!fsyncDirectory(*iterator)) {
      return false;
    }
  }
  return true;
}

int openInventoryFileNoFollow(const Inventory &inventory,
                              std::string_view relativePath) {
  int parent =
      ::open(inventory.rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (parent < 0 || !metadataMatchesOpenFile(parent, inventory.rootMetadata)) {
    if (parent >= 0) {
      ::close(parent);
    }
    return -1;
  }

  std::size_t componentStart = 0;
  while (true) {
    const std::size_t separator = relativePath.find('/', componentStart);
    const std::string component(
        relativePath.substr(componentStart, separator == std::string_view::npos
                                                ? std::string_view::npos
                                                : separator - componentStart));
    if (separator == std::string_view::npos) {
      const int file =
          ::openat(parent, component.c_str(), O_RDONLY | O_NOFOLLOW);
      ::close(parent);
      return file;
    }
    const int next = ::openat(parent, component.c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    ::close(parent);
    if (next < 0) {
      return -1;
    }
    parent = next;
    componentStart = separator + 1;
  }
}
#else
bool fsyncDirectory(const fs::path &) { return true; }
bool fsyncDirectoryTree(const fs::path &) { return true; }
#endif

std::optional<std::string>
digestAndMaybeCopy(const Inventory &inventory,
                   const std::optional<fs::path> &destination,
                   SkinProgressPhase phase, std::stop_token stop,
                   const SkinProgressCallback &callback,
                   std::vector<SkinDiagnostic> &diagnostics) {
  file_checksum::Sha256 hash;
  hashText(hash, "ASOBMSKIN-TREE-V1");
  const std::array<std::byte, 1> terminator{std::byte{0}};
  hash.update(terminator);
  hashBigEndian(hash, inventory.fileCount);

  std::uint64_t completedBytes = 0;
  std::uint64_t completedFiles = 0;
  std::array<char, 64 * 1024> buffer{};
  for (const InventoryEntry &entry : inventory.entries) {
    if (entry.metadata.directory) {
      if (destination) {
        std::error_code createError;
        fs::create_directories(
            *destination / pathFromUtf8(entry.normalizedPath), createError);
        if (createError) {
          diagnostics.push_back(
              diagnostic("skin_snapshot_staging_create_failed",
                         "unable to create a normalized staging directory",
                         entry.normalizedPath));
          return std::nullopt;
        }
      }
      continue;
    }
    if (stop.stop_requested()) {
      return std::nullopt;
    }
    hashBigEndian(hash,
                  static_cast<std::uint32_t>(entry.normalizedPath.size()));
    hashText(hash, entry.normalizedPath);
    hashBigEndian(hash, entry.metadata.size);

#if defined(_WIN32)
    std::ifstream input(entry.sourcePath, std::ios::binary);
    if (!input || !metadataMatchesOpenFile(entry.sourcePath, entry.metadata)) {
      diagnostics.push_back(diagnostic("skin_snapshot_source_changed",
                                       "skin source changed before copying",
                                       entry.normalizedPath));
      return std::nullopt;
    }
    std::ofstream output;
    if (destination) {
      const fs::path target = *destination / pathFromUtf8(entry.normalizedPath);
      fs::create_directories(target.parent_path());
      output.open(target, std::ios::binary | std::ios::trunc);
      if (!output) {
        diagnostics.push_back(diagnostic("skin_snapshot_copy_failed",
                                         "unable to create staging file",
                                         entry.normalizedPath));
        return std::nullopt;
      }
    }
    std::uint64_t readTotal = 0;
    while (input) {
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const std::streamsize count = input.gcount();
      if (count > 0) {
        hash.update(std::as_bytes(
            std::span(buffer.data(), static_cast<std::size_t>(count))));
        if (destination) {
          output.write(buffer.data(), count);
        }
        readTotal += static_cast<std::uint64_t>(count);
      }
    }
    if (!input.eof() || readTotal != entry.metadata.size ||
        !metadataMatchesOpenFile(entry.sourcePath, entry.metadata)) {
      diagnostics.push_back(diagnostic("skin_snapshot_source_changed",
                                       "skin source changed while copying",
                                       entry.normalizedPath));
      return std::nullopt;
    }
#else
    const int input =
        openInventoryFileNoFollow(inventory, entry.normalizedPath);
    if (input < 0 || !metadataMatchesOpenFile(input, entry.metadata)) {
      if (input >= 0) {
        ::close(input);
      }
      diagnostics.push_back(diagnostic("skin_snapshot_source_changed",
                                       "skin source changed before copying",
                                       entry.normalizedPath));
      return std::nullopt;
    }
    int output = -1;
    if (destination) {
      const fs::path target = *destination / pathFromUtf8(entry.normalizedPath);
      std::error_code createError;
      fs::create_directories(target.parent_path(), createError);
      if (createError) {
        ::close(input);
        diagnostics.push_back(diagnostic("skin_snapshot_copy_failed",
                                         "unable to create staging parent",
                                         entry.normalizedPath));
        return std::nullopt;
      }
      output = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                      0600);
      if (output < 0) {
        ::close(input);
        diagnostics.push_back(diagnostic("skin_snapshot_copy_failed",
                                         "unable to create staging file",
                                         entry.normalizedPath));
        return std::nullopt;
      }
    }
    std::uint64_t readTotal = 0;
    while (true) {
      if (stop.stop_requested()) {
        if (output >= 0) {
          ::close(output);
        }
        ::close(input);
        return std::nullopt;
      }
      const ssize_t count = ::read(input, buffer.data(), buffer.size());
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (output >= 0) {
          ::close(output);
        }
        ::close(input);
        diagnostics.push_back(diagnostic("skin_snapshot_copy_failed",
                                         "unable to read skin source file",
                                         entry.normalizedPath));
        return std::nullopt;
      }
      if (count == 0) {
        break;
      }
      const auto countSize = static_cast<std::size_t>(count);
      hash.update(std::as_bytes(std::span(buffer.data(), countSize)));
      if (output >= 0) {
        std::size_t written = 0;
        while (written < countSize) {
          const ssize_t amount =
              ::write(output, buffer.data() + written, countSize - written);
          if (amount < 0 && errno == EINTR) {
            continue;
          }
          if (amount <= 0) {
            ::close(output);
            ::close(input);
            diagnostics.push_back(diagnostic("skin_snapshot_copy_failed",
                                             "unable to write staging file",
                                             entry.normalizedPath));
            return std::nullopt;
          }
          written += static_cast<std::size_t>(amount);
        }
      }
      readTotal += static_cast<std::uint64_t>(count);
    }
    const bool stable = readTotal == entry.metadata.size &&
                        metadataMatchesOpenFile(input, entry.metadata);
    if (output >= 0) {
      if (::fsync(output) != 0) {
        ::close(output);
        ::close(input);
        diagnostics.push_back(diagnostic("skin_snapshot_fsync_failed",
                                         "unable to synchronize staging file",
                                         entry.normalizedPath));
        return std::nullopt;
      }
      ::close(output);
    }
    ::close(input);
    if (!stable) {
      diagnostics.push_back(diagnostic("skin_snapshot_source_changed",
                                       "skin source changed while copying",
                                       entry.normalizedPath));
      return std::nullopt;
    }
#endif
    completedBytes += entry.metadata.size;
    ++completedFiles;
    if (!report(callback,
                {.phase = phase,
                 .completedBytes = completedBytes,
                 .totalBytes = inventory.totalBytes,
                 .completedFiles = completedFiles},
                diagnostics)) {
      return std::nullopt;
    }
  }
  if (destination && !fsyncDirectoryTree(*destination)) {
    diagnostics.push_back(diagnostic("skin_snapshot_fsync_failed",
                                     "unable to synchronize staging root"));
    return std::nullopt;
  }
  return hash.finalHex();
}

void makeImmutable(const fs::path &root, std::error_code &error) {
  for (fs::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; ++iterator) {
    const bool directory = iterator->is_directory(error);
    if (error) {
      return;
    }
    fs::permissions(iterator->path(),
                    directory ? fs::perms::owner_read | fs::perms::owner_exec
                              : fs::perms::owner_read,
                    fs::perm_options::replace, error);
    if (error) {
      return;
    }
  }
  fs::permissions(root, fs::perms::owner_read | fs::perms::owner_exec,
                  fs::perm_options::replace, error);
}

std::string uniqueStagingName() {
  static std::atomic_uint64_t serial{0};
  return "snapshot-" + std::to_string(++serial);
}

} // namespace

struct SkinRevisionPin {
  SkinRevision revision;
  fs::path root;
};

struct PreparedSkinRevision::State {
  State(SkinRevision revisionValue, fs::path stagingValue,
        fs::path publishedValue)
      : revision(std::move(revisionValue)),
        stagingRoot(std::move(stagingValue)),
        publishedRoot(std::move(publishedValue)) {}

  SkinRevision revision;
  fs::path stagingRoot;
  fs::path publishedRoot;

  ~State() {
    if (stagingRoot.empty()) {
      return;
    }
    std::error_code ignored;
    fs::permissions(stagingRoot, fs::perms::owner_all, fs::perm_options::add,
                    ignored);
    fs::remove_all(stagingRoot, ignored);
  }
};

SkinRevisionReadView::SkinRevisionReadView(const SkinRevision *revision,
                                           const fs::path *root)
    : revision_(revision), root_(root) {}

const SkinRevision &SkinRevisionReadView::revision() const noexcept {
  assert(revision_ != nullptr);
  return *revision_;
}

const fs::path &SkinRevisionReadView::root() const noexcept {
  assert(root_ != nullptr);
  return *root_;
}

SkinRevisionLease::SkinRevisionLease(std::shared_ptr<const SkinRevisionPin> pin)
    : pin_(std::move(pin)) {}

const SkinRevision &SkinRevisionLease::revision() const noexcept {
  assert(pin_ != nullptr);
  return pin_->revision;
}

const fs::path &SkinRevisionLease::root() const noexcept {
  assert(pin_ != nullptr);
  return pin_->root;
}

SkinRevisionReadView SkinRevisionLease::readView() const noexcept {
  return SkinRevisionReadView(&pin_->revision, &pin_->root);
}

SkinRevisionLease SkinRevisionLease::clone() const {
  assert(pin_ != nullptr);
  return SkinRevisionLease(pin_);
}

PreparedSkinRevision::PreparedSkinRevision(SkinRevision revision,
                                           fs::path stagingRoot,
                                           fs::path publishedRoot)
    : state_(std::make_unique<State>(std::move(revision),
                                     std::move(stagingRoot),
                                     std::move(publishedRoot))) {}

PreparedSkinRevision::PreparedSkinRevision(PreparedSkinRevision &&) noexcept =
    default;
PreparedSkinRevision &
PreparedSkinRevision::operator=(PreparedSkinRevision &&) noexcept = default;
PreparedSkinRevision::~PreparedSkinRevision() = default;

const SkinRevision &PreparedSkinRevision::revision() const noexcept {
  assert(state_ != nullptr);
  return state_->revision;
}

const fs::path &PreparedSkinRevision::stagingRoot() const noexcept {
  assert(state_ != nullptr);
  return state_->stagingRoot;
}

SkinRevisionReadView PreparedSkinRevision::readView() const noexcept {
  return SkinRevisionReadView(&state_->revision, &state_->stagingRoot);
}

std::optional<SkinRevisionLease>
PreparedSkinRevision::publish(std::string &error) && {
  error.clear();
  if (!state_ || state_->stagingRoot.empty()) {
    error = "prepared skin revision is no longer publishable";
    return std::nullopt;
  }
  std::error_code filesystemError;
  fs::create_directories(state_->publishedRoot.parent_path(), filesystemError);
  if (filesystemError) {
    error =
        "unable to create private revision root: " + filesystemError.message();
    return std::nullopt;
  }
  if (fs::exists(state_->publishedRoot, filesystemError)) {
    fs::remove_all(state_->stagingRoot, filesystemError);
  } else {
    filesystemError.clear();
    fs::rename(state_->stagingRoot, state_->publishedRoot, filesystemError);
  }
  if (filesystemError) {
    error =
        "unable to publish private skin revision: " + filesystemError.message();
    return std::nullopt;
  }
  state_->stagingRoot.clear();
  makeImmutable(state_->publishedRoot, filesystemError);
  if (filesystemError) {
    error =
        "unable to make skin revision immutable: " + filesystemError.message();
    return std::nullopt;
  }
  if (!fsyncDirectory(state_->publishedRoot.parent_path())) {
    error = "unable to synchronize private revision publication";
    return std::nullopt;
  }
  auto pin = std::make_shared<SkinRevisionPin>(SkinRevisionPin{
      .revision = state_->revision, .root = state_->publishedRoot});
  return SkinRevisionLease(std::move(pin));
}

SkinTreeSnapshotter::SkinTreeSnapshotter(SkinStorageRoots roots,
                                         const SkinAliasDetector &aliases)
    : roots_(std::move(roots)), aliases_(aliases) {}

SnapshotTreeResult SkinTreeSnapshotter::snapshot(
    const fs::path &sourceRoot, const SkinPackageId &package,
    std::stop_token stop, SkinProgressCallback callback) {
  SnapshotTreeResult result;
  const auto normalizedPackage = normalizePackageId(package.directoryName);
  if (!normalizedPackage.package ||
      normalizedPackage.package->collisionKey != package.collisionKey) {
    result.diagnostics.push_back(diagnostic(
        "skin_snapshot_package_invalid", "skin package identity is invalid"));
    return result;
  }
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }

  auto first =
      inventoryTree(sourceRoot, package, aliases_, stop, result.diagnostics);
  if (!first) {
    result.cancelled = stop.stop_requested();
    return result;
  }
  if (!report(callback,
              {.phase = SkinProgressPhase::Inspecting,
               .completedBytes = first->totalBytes,
               .totalBytes = first->totalBytes,
               .completedFiles = first->fileCount},
              result.diagnostics)) {
    return result;
  }
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }

  std::error_code stagingError;
  const fs::path stagingParent = roots_.privateRevisions / ".staging";
  fs::create_directories(stagingParent, stagingError);
  const fs::path stagingRoot = stagingParent / uniqueStagingName();
  if (!stagingError) {
    fs::create_directory(stagingRoot, stagingError);
  }
  if (stagingError) {
    result.diagnostics.push_back(
        diagnostic("skin_snapshot_staging_create_failed",
                   "unable to create private skin revision staging"));
    return result;
  }
  struct StagingCleanup {
    fs::path path;
    ~StagingCleanup() {
      if (path.empty()) {
        return;
      }
      std::error_code ignored;
      fs::permissions(path, fs::perms::owner_all, fs::perm_options::add,
                      ignored);
      fs::remove_all(path, ignored);
    }
  } cleanup{stagingRoot};

  const auto copiedDigest =
      digestAndMaybeCopy(*first, stagingRoot, SkinProgressPhase::Copying, stop,
                         callback, result.diagnostics);
  if (!copiedDigest) {
    result.cancelled = stop.stop_requested();
    return result;
  }
  if (!report(callback,
              {.phase = SkinProgressPhase::Validating,
               .completedBytes = 0,
               .totalBytes = first->totalBytes,
               .completedFiles = 0},
              result.diagnostics)) {
    return result;
  }
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }
  auto second =
      inventoryTree(sourceRoot, package, aliases_, stop, result.diagnostics);
  if (!second) {
    result.cancelled = stop.stop_requested();
    return result;
  }
  if (first->rootMetadata != second->rootMetadata ||
      first->entries != second->entries ||
      first->fileCount != second->fileCount ||
      first->totalBytes != second->totalBytes) {
    result.diagnostics.push_back(
        diagnostic("skin_snapshot_source_changed",
                   "skin source changed between snapshot inventory passes"));
    return result;
  }
  const auto verifiedDigest =
      digestAndMaybeCopy(*second, std::nullopt, SkinProgressPhase::Validating,
                         stop, callback, result.diagnostics);
  if (!verifiedDigest) {
    result.cancelled = stop.stop_requested();
    return result;
  }
  if (*copiedDigest != *verifiedDigest) {
    result.diagnostics.push_back(diagnostic(
        "skin_snapshot_source_changed",
        "skin source content changed while the snapshot was copied"));
    return result;
  }

  if (!report(callback,
              {.phase = SkinProgressPhase::Publishing,
               .completedBytes = second->totalBytes,
               .totalBytes = second->totalBytes,
               .completedFiles = second->fileCount},
              result.diagnostics)) {
    return result;
  }
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }
  SkinRevision revision{.package = package,
                        .lowercaseSha256 = *verifiedDigest,
                        .fileCount = second->fileCount,
                        .totalBytes = second->totalBytes};
  const fs::path publishedRoot =
      roots_.privateRevisions / revision.lowercaseSha256;
  result.prepared =
      PreparedSkinRevision(std::move(revision), stagingRoot, publishedRoot);
  cleanup.path.clear();
  return result;
}

} // namespace skin
