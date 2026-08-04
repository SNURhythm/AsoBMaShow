#include "LuaSkinFileSystem.h"

#include "../package/SkinPathPolicy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <Aclapi.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace skin {
namespace {

namespace fs = std::filesystem;

enum class HostEntryKind : std::uint8_t {
  Missing,
  Regular,
  Directory,
  NonRegular,
  LimitExceeded,
  IoError,
};

enum class RenderOperation : std::uint8_t { Read, Write, DirectoryScan };

// Overlay quotas and replacements are process-global invariants. Serialize
// them across filesystem instances that may share the same derived root.
std::mutex overlayMutationMutex;
constexpr std::size_t maximumTemporaryCreateAttempts = 16;
constexpr std::size_t maximumAbandonedTemporaries = 64;
constexpr std::uint64_t maximumOverlayDirectories = 1'024;
constexpr std::uint64_t maximumOverlayNodes = 2'048;
constexpr std::size_t maximumOverlayDepth =
    SkinPackagePolicy::maxPathComponents;
constexpr std::size_t maximumListingEntries = SkinPackagePolicy::maxFiles;
constexpr std::string_view internalTemporaryDirectoryName =
    ".asobmashow-internal";
constexpr std::string_view temporaryNamePrefix = "write-";
constexpr std::string_view temporaryNameSuffix = ".tmp";

std::optional<std::string> uniqueOverlayTemporaryName() {
  std::array<std::uint32_t, 4> randomWords{};
  try {
    std::random_device entropy;
    for (std::uint32_t &word : randomWords) {
      word = entropy();
    }
  } catch (...) {
    return std::nullopt;
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string name(temporaryNamePrefix);
  name.reserve(name.size() + randomWords.size() * 8 + 4);
  for (const std::uint32_t word : randomWords) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      name.push_back(hex[(word >> shift) & 0xfU]);
    }
  }
  name += ".tmp";
  return name;
}

bool isOwnedTemporaryName(std::string_view name) {
  if (!name.starts_with(temporaryNamePrefix) ||
      !name.ends_with(temporaryNameSuffix) ||
      name.size() !=
          temporaryNamePrefix.size() + 32 + temporaryNameSuffix.size()) {
    return false;
  }
  const std::string_view identifier =
      name.substr(temporaryNamePrefix.size(), 32);
  return std::ranges::all_of(identifier, [](unsigned char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
  });
}

struct HostStatResult {
  HostEntryKind kind = HostEntryKind::IoError;
  std::uint64_t size = 0;
};

struct HostReadResult {
  HostEntryKind kind = HostEntryKind::IoError;
  std::vector<std::byte> bytes;
  bool limitExceeded = false;
};

struct OverlayUsage {
  std::uint64_t bytes = 0;
  std::uint64_t files = 0;
  std::uint64_t directories = 0;
  std::uint64_t nodes = 0;
  HostEntryKind kind = HostEntryKind::Regular;
};

struct NormalizedReference {
  std::optional<std::string> path;
  std::optional<SkinFileFailure> failure;
};

SkinFileFailure failure(SkinFileError code, std::string_view virtualPath,
                        std::string_view message) {
  return {.code = code,
          .virtualPath = std::string(virtualPath),
          .message = std::string(message)};
}

std::vector<std::string> splitNormalized(std::string_view path) {
  std::vector<std::string> components;
  std::size_t start = 0;
  while (start < path.size()) {
    const std::size_t end = path.find('/', start);
    components.emplace_back(path.substr(start, end == std::string_view::npos
                                                   ? path.size() - start
                                                   : end - start));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return components;
}

std::string joinComponents(const std::vector<std::string> &components) {
  std::string joined;
  for (const std::string &component : components) {
    if (!joined.empty()) {
      joined.push_back('/');
    }
    joined += component;
  }
  return joined;
}

fs::path pathFromUtf8(std::string_view path) {
  std::u8string utf8;
  utf8.reserve(path.size());
  for (unsigned char byte : path) {
    utf8.push_back(static_cast<char8_t>(byte));
  }
  return fs::path(utf8);
}

bool equalsAsciiCaseInsensitive(std::string_view left, std::string_view right) {
  return left.size() == right.size() &&
         std::ranges::equal(left, right, [](unsigned char a, unsigned char b) {
           const auto fold = [](unsigned char value) {
             return value >= 'A' && value <= 'Z'
                        ? static_cast<unsigned char>(value - 'A' + 'a')
                        : value;
           };
           return fold(a) == fold(b);
         });
}

bool isPortableSkinFileComponent(std::string_view component) {
  if (component.empty() ||
      equalsAsciiCaseInsensitive(component, internalTemporaryDirectoryName) ||
      component.back() == '.' || component.back() == ' ') {
    return false;
  }
  for (const unsigned char value : component) {
    if (value < 0x20 ||
        std::string_view("<>:\"/\\|?*").find(value) != std::string_view::npos) {
      return false;
    }
  }
  std::string device(component.substr(0, component.find('.')));
  std::ranges::transform(device, device.begin(), [](unsigned char value) {
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A')
                                        : static_cast<char>(value);
  });
  if (device == "CON" || device == "PRN" || device == "AUX" ||
      device == "NUL" || device == "CLOCK$") {
    return false;
  }
  return !((device.starts_with("COM") || device.starts_with("LPT")) &&
           device.size() == 4 && device.back() >= '1' && device.back() <= '9');
}

std::optional<fs::path> canonicalTrustedRoot(const fs::path &root) {
  if (root.empty() || !root.is_absolute()) {
    return std::nullopt;
  }
  const fs::path normalized = root.lexically_normal();
#if defined(_WIN32)
  // Reparse containment is established by retained no-reparse handle walks;
  // canonicalizing here would follow the very alias being validated.
  return normalized;
#else
  const fs::path relative = normalized.relative_path();
  auto component = relative.begin();
  const auto end = relative.end();
  std::error_code error;
  fs::path canonical = normalized.root_path();
  if (component != end) {
    canonical = fs::canonical(canonical / *component, error);
    ++component;
  }
  if (error || canonical.empty() || !canonical.is_absolute()) {
    return std::nullopt;
  }
  for (; component != end; ++component) {
    const fs::path candidate = canonical / *component;
    const fs::file_status status = fs::symlink_status(candidate, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      for (; component != end; ++component) {
        canonical /= *component;
      }
      break;
    }
    if (error || fs::is_symlink(status) || !fs::is_directory(status)) {
      return std::nullopt;
    }
    canonical = candidate;
  }
  return canonical.lexically_normal();
#endif
}

std::string utf8Path(const fs::path &path) {
  const std::u8string value = path.generic_u8string();
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}

NormalizedReference
normalizeReference(const SkinPackageId &package,
                   const std::vector<std::string> &workingComponents,
                   std::string_view authored, bool allowPackageRoot = false) {
  if (authored.empty() || authored.find('\0') != std::string_view::npos ||
      authored.front() == '/' ||
      authored.find('\\') != std::string_view::npos ||
      (authored.size() >= 2 &&
       ((authored[0] >= 'A' && authored[0] <= 'Z') ||
        (authored[0] >= 'a' && authored[0] <= 'z')) &&
       authored[1] == ':')) {
    return {.failure = failure(SkinFileError::InvalidPath, authored,
                               "skin virtual path is invalid")};
  }

  std::vector<std::string> components = workingComponents;
  std::size_t start = 0;
  while (start <= authored.size()) {
    const std::size_t end = authored.find('/', start);
    const std::string_view component(
        authored.data() + start,
        (end == std::string_view::npos ? authored.size() : end) - start);
    if (component.empty()) {
      return {.failure = failure(SkinFileError::InvalidPath, authored,
                                 "skin virtual path has an empty component")};
    }
    if (component == ".") {
      // The working directory is an explicit safe virtual location.
    } else if (component == "..") {
      if (components.empty()) {
        return {.failure = failure(SkinFileError::EscapesPackage, authored,
                                   "skin virtual path escapes its package")};
      }
      components.pop_back();
    } else {
      const auto normalized = normalizeSkinSourceNameNfc(component);
      if (!normalized.value) {
        return {.failure = failure(SkinFileError::InvalidPath, authored,
                                   "skin virtual path is invalid")};
      }
      if (!isPortableSkinFileComponent(*normalized.value)) {
        return {.failure = failure(SkinFileError::InvalidPath, authored,
                                   "skin virtual path has an unsafe name")};
      }
      components.push_back(std::move(*normalized.value));
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }

  const std::string joined = joinComponents(components);
  if (joined.empty()) {
    if (allowPackageRoot) {
      return {.path = std::string{}};
    }
    return {.failure = failure(SkinFileError::InvalidPath, authored,
                               "skin virtual path does not name a file")};
  }
  const auto normalized = normalizeEntryPath(package, joined);
  if (!normalized.entry) {
    return {.failure = failure(SkinFileError::InvalidPath, authored,
                               "skin virtual path exceeds its limits")};
  }
  return {.path = normalized.entry->packageRelativePath};
}

SkinFileError errorForKind(HostEntryKind kind) {
  switch (kind) {
  case HostEntryKind::Missing:
    return SkinFileError::Missing;
  case HostEntryKind::NonRegular:
  case HostEntryKind::Directory:
    return SkinFileError::NonRegular;
  case HostEntryKind::LimitExceeded:
    return SkinFileError::QuotaExceeded;
  case HostEntryKind::IoError:
    return SkinFileError::IoError;
  case HostEntryKind::Regular:
    break;
  }
  return SkinFileError::IoError;
}

std::string_view messageForKind(HostEntryKind kind) {
  switch (kind) {
  case HostEntryKind::Missing:
    return "skin virtual file is missing";
  case HostEntryKind::NonRegular:
  case HostEntryKind::Directory:
    return "skin virtual path is not a single regular file";
  case HostEntryKind::LimitExceeded:
    return "skin filesystem work exceeds its fixed limit";
  case HostEntryKind::IoError:
    return "skin virtual file operation failed";
  case HostEntryKind::Regular:
    break;
  }
  return "skin virtual file operation failed";
}

#if !defined(_WIN32)
class UniqueFd {
public:
  UniqueFd() = default;
  explicit UniqueFd(int value) noexcept : value_(value) {}
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  UniqueFd(UniqueFd &&other) noexcept
      : value_(std::exchange(other.value_, -1)) {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.value_, -1));
    }
    return *this;
  }
  ~UniqueFd() { reset(); }

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

HostEntryKind errnoKind(int value) {
  if (value == ENOENT) {
    return HostEntryKind::Missing;
  }
  if (value == ELOOP || value == ENOTDIR) {
    return HostEntryKind::NonRegular;
  }
  return HostEntryKind::IoError;
}

std::pair<UniqueFd, HostEntryKind>
openDirectoryNoFollow(const fs::path &root, std::string_view virtualDirectory,
                      bool requirePrivate = false) {
  int flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  UniqueFd current(::open(root.c_str(), flags));
  if (!current) {
    return {UniqueFd{}, errnoKind(errno)};
  }
  struct stat rootStatus{};
  if (requirePrivate &&
      (::fstat(current.get(), &rootStatus) != 0 ||
       !S_ISDIR(rootStatus.st_mode) || rootStatus.st_uid != ::geteuid() ||
       (rootStatus.st_mode & 077) != 0)) {
    return {UniqueFd{}, HostEntryKind::NonRegular};
  }
  for (const std::string &component : splitNormalized(virtualDirectory)) {
    UniqueFd next(::openat(current.get(), component.c_str(), flags));
    if (!next) {
      return {UniqueFd{}, errnoKind(errno)};
    }
    struct stat status{};
    if (requirePrivate &&
        (::fstat(next.get(), &status) != 0 || !S_ISDIR(status.st_mode) ||
         status.st_uid != ::geteuid() || (status.st_mode & 077) != 0)) {
      return {UniqueFd{}, HostEntryKind::NonRegular};
    }
    current = std::move(next);
  }
  return {std::move(current), HostEntryKind::Directory};
}

HostStatResult statAtRootImpl(const fs::path &root,
                              std::string_view virtualPath,
                              bool requirePrivate) {
  const std::vector<std::string> components = splitNormalized(virtualPath);
  if (components.empty()) {
    auto [directory, kind] = openDirectoryNoFollow(root, {}, requirePrivate);
    return {.kind = directory ? HostEntryKind::Directory : kind};
  }
  std::string parent;
  if (components.size() > 1) {
    parent = joinComponents(
        std::vector<std::string>(components.begin(), components.end() - 1));
  }
  auto [directory, directoryKind] =
      openDirectoryNoFollow(root, parent, requirePrivate);
  if (!directory) {
    return {.kind = directoryKind};
  }
  struct stat status{};
  if (::fstatat(directory.get(), components.back().c_str(), &status,
                AT_SYMLINK_NOFOLLOW) != 0) {
    return {.kind = errnoKind(errno)};
  }
  if (S_ISDIR(status.st_mode)) {
    if (requirePrivate &&
        (status.st_uid != ::geteuid() || (status.st_mode & 077) != 0)) {
      return {.kind = HostEntryKind::NonRegular};
    }
    return {.kind = HostEntryKind::Directory};
  }
  if (!S_ISREG(status.st_mode) || status.st_nlink != 1) {
    return {.kind = HostEntryKind::NonRegular};
  }
  if (requirePrivate &&
      (status.st_uid != ::geteuid() || (status.st_mode & 077) != 0)) {
    return {.kind = HostEntryKind::NonRegular};
  }
  return {.kind = HostEntryKind::Regular,
          .size = static_cast<std::uint64_t>(status.st_size)};
}

HostStatResult statAtRoot(const fs::path &root, std::string_view virtualPath) {
  return statAtRootImpl(root, virtualPath, false);
}

HostStatResult statAtPrivateOverlay(const fs::path &root,
                                    std::string_view virtualPath) {
  return statAtRootImpl(root, virtualPath, true);
}

HostReadResult readAtRootImpl(const fs::path &root,
                              std::string_view virtualPath,
                              std::uint64_t maximumBytes, bool requirePrivate) {
  const std::vector<std::string> components = splitNormalized(virtualPath);
  if (components.empty()) {
    return {.kind = HostEntryKind::Directory};
  }
  std::string parent;
  if (components.size() > 1) {
    parent = joinComponents(
        std::vector<std::string>(components.begin(), components.end() - 1));
  }
  auto [directory, directoryKind] =
      openDirectoryNoFollow(root, parent, requirePrivate);
  if (!directory) {
    return {.kind = directoryKind};
  }
  int flags = O_RDONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
  flags |= O_NONBLOCK;
#endif
  UniqueFd file(::openat(directory.get(), components.back().c_str(), flags));
  if (!file) {
    return {.kind = errnoKind(errno)};
  }
  struct stat before{};
  if (::fstat(file.get(), &before) != 0) {
    return {.kind = HostEntryKind::IoError};
  }
  if (!S_ISREG(before.st_mode) || before.st_nlink != 1 ||
      (requirePrivate &&
       (before.st_uid != ::geteuid() || (before.st_mode & 077) != 0))) {
    return {.kind = HostEntryKind::NonRegular};
  }
  const std::uint64_t size = static_cast<std::uint64_t>(before.st_size);
  if (size > maximumBytes || size > std::numeric_limits<std::size_t>::max()) {
    return {.kind = HostEntryKind::Regular, .limitExceeded = true};
  }
  HostReadResult result{.kind = HostEntryKind::Regular};
  try {
    result.bytes.resize(static_cast<std::size_t>(size));
  } catch (...) {
    return {.kind = HostEntryKind::IoError};
  }
  std::size_t offset = 0;
  while (offset < result.bytes.size()) {
    const ssize_t count = ::read(file.get(), result.bytes.data() + offset,
                                 result.bytes.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return {.kind = HostEntryKind::IoError};
    }
    offset += static_cast<std::size_t>(count);
  }
  struct stat after{};
  if (::fstat(file.get(), &after) != 0 || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || before.st_size != after.st_size ||
      before.st_nlink != after.st_nlink || !S_ISREG(after.st_mode) ||
      after.st_nlink != 1 ||
      (requirePrivate &&
       (after.st_uid != ::geteuid() || (after.st_mode & 077) != 0))) {
    return {.kind = HostEntryKind::IoError};
  }
  return result;
}

HostReadResult readAtRoot(const fs::path &root, std::string_view virtualPath,
                          std::uint64_t maximumBytes) {
  return readAtRootImpl(root, virtualPath, maximumBytes, false);
}

HostReadResult readAtPrivateOverlay(const fs::path &root,
                                    std::string_view virtualPath,
                                    std::uint64_t maximumBytes) {
  return readAtRootImpl(root, virtualPath, maximumBytes, true);
}

HostEntryKind validatePrivateOverlayRoot(const fs::path &root) {
  return statAtPrivateOverlay(root, {}).kind;
}

bool isPrivatePosixEntry(const struct stat &status) {
  return status.st_uid == ::geteuid() && (status.st_mode & 077) == 0;
}

bool collectUsageFromDirectory(int directory, OverlayUsage &usage,
                               std::size_t depth, bool overlayRoot) {
  const int duplicate = ::dup(directory);
  if (duplicate < 0) {
    usage.kind = HostEntryKind::IoError;
    return false;
  }
  DIR *stream = ::fdopendir(duplicate);
  if (stream == nullptr) {
    ::close(duplicate);
    usage.kind = HostEntryKind::IoError;
    return false;
  }
  errno = 0;
  while (dirent *entry = ::readdir(stream)) {
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    if (overlayRoot && name == internalTemporaryDirectoryName) {
      continue;
    }
    struct stat status{};
    if (::fstatat(directory, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) !=
        0) {
      ::closedir(stream);
      usage.kind = HostEntryKind::IoError;
      return false;
    }
    if (usage.nodes == maximumOverlayNodes) {
      ::closedir(stream);
      usage.kind = HostEntryKind::LimitExceeded;
      return false;
    }
    ++usage.nodes;
    if (S_ISDIR(status.st_mode)) {
      if (!isPrivatePosixEntry(status) ||
          usage.directories == maximumOverlayDirectories ||
          depth == maximumOverlayDepth) {
        ::closedir(stream);
        usage.kind = !isPrivatePosixEntry(status)
                         ? HostEntryKind::NonRegular
                         : HostEntryKind::LimitExceeded;
        return false;
      }
      ++usage.directories;
      int flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
      flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
      flags |= O_NOFOLLOW;
#endif
      UniqueFd child(::openat(directory, entry->d_name, flags));
      struct stat opened{};
      if (!child || ::fstat(child.get(), &opened) != 0 ||
          opened.st_dev != status.st_dev || opened.st_ino != status.st_ino ||
          !isPrivatePosixEntry(opened) ||
          !collectUsageFromDirectory(child.get(), usage, depth + 1, false)) {
        ::closedir(stream);
        if (usage.kind == HostEntryKind::Regular) {
          usage.kind = HostEntryKind::NonRegular;
        }
        return false;
      }
      continue;
    }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        !isPrivatePosixEntry(status) || status.st_size < 0 ||
        usage.files == std::numeric_limits<std::uint64_t>::max() ||
        usage.bytes > std::numeric_limits<std::uint64_t>::max() -
                          static_cast<std::uint64_t>(status.st_size)) {
      ::closedir(stream);
      usage.kind = HostEntryKind::NonRegular;
      return false;
    }
    ++usage.files;
    usage.bytes += static_cast<std::uint64_t>(status.st_size);
  }
  const int savedErrno = errno;
  ::closedir(stream);
  if (savedErrno != 0) {
    usage.kind = HostEntryKind::IoError;
    return false;
  }
  return true;
}

OverlayUsage collectUsage(int root) {
  OverlayUsage usage;
  collectUsageFromDirectory(root, usage, 0, true);
  return usage;
}

std::pair<std::vector<std::string>, HostEntryKind>
listAtRoot(const fs::path &root, std::string_view virtualDirectory,
           const SkinPackageId &package, std::size_t maximumEntries) {
  auto [directory, kind] = openDirectoryNoFollow(root, virtualDirectory);
  if (!directory) {
    return {{}, kind};
  }
  const int duplicate = ::dup(directory.get());
  if (duplicate < 0) {
    return {{}, HostEntryKind::IoError};
  }
  DIR *stream = ::fdopendir(duplicate);
  if (stream == nullptr) {
    ::close(duplicate);
    return {{}, HostEntryKind::IoError};
  }
  std::vector<std::string> result;
  errno = 0;
  while (dirent *entry = ::readdir(stream)) {
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    if (result.size() > maximumEntries) {
      ::closedir(stream);
      return {{}, HostEntryKind::LimitExceeded};
    }
    struct stat status{};
    if (::fstatat(directory.get(), entry->d_name, &status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      ::closedir(stream);
      return {{}, HostEntryKind::IoError};
    }
    if ((!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode)) ||
        (S_ISREG(status.st_mode) && status.st_nlink != 1)) {
      ::closedir(stream);
      return {{}, HostEntryKind::NonRegular};
    }
    std::string candidate;
    if (!virtualDirectory.empty()) {
      candidate = std::string(virtualDirectory) + "/";
    }
    candidate += entry->d_name;
    const auto normalized = normalizeEntryPath(package, candidate);
    if (!normalized.entry ||
        normalized.entry->packageRelativePath != candidate) {
      ::closedir(stream);
      return {{}, HostEntryKind::NonRegular};
    }
    result.push_back(std::move(candidate));
  }
  const int savedErrno = errno;
  ::closedir(stream);
  if (savedErrno != 0) {
    return {{}, HostEntryKind::IoError};
  }
  std::ranges::sort(result);
  return {std::move(result), HostEntryKind::Directory};
}

class InternalTemporaryCleanup {
public:
  InternalTemporaryCleanup() = default;
  InternalTemporaryCleanup(const InternalTemporaryCleanup &) = delete;
  InternalTemporaryCleanup &
  operator=(const InternalTemporaryCleanup &) = delete;
  ~InternalTemporaryCleanup() { (void)remove(); }

  void arm(int directory, std::string name) {
    directory_ = directory;
    name_ = std::move(name);
    armed_ = true;
  }
  bool remove() {
    if (!armed_) {
      return true;
    }
    if (::unlinkat(directory_, name_.c_str(), 0) != 0 && errno != ENOENT) {
      return false;
    }
    armed_ = false;
    return true;
  }
  void disarm() noexcept { armed_ = false; }

private:
  int directory_ = -1;
  std::string name_;
  bool armed_ = false;
};

bool isPrivatePosixDirectory(int descriptor) {
  struct stat status{};
  return ::fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode) &&
         isPrivatePosixEntry(status);
}

std::pair<UniqueFd, HostEntryKind>
openOrCreatePrivateOverlayRoot(const fs::path &root) {
  const fs::path normalized = root.lexically_normal();
  if (!normalized.is_absolute() || normalized.root_path().empty()) {
    return {UniqueFd{}, HostEntryKind::NonRegular};
  }
  int flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  UniqueFd current(::open(normalized.root_path().c_str(), flags));
  if (!current) {
    return {UniqueFd{}, errnoKind(errno)};
  }
  const fs::path relative = normalized.relative_path();
  std::size_t index = 0;
  const std::size_t count =
      static_cast<std::size_t>(std::distance(relative.begin(), relative.end()));
  for (const fs::path &component : relative) {
    ++index;
    const std::string name = component.string();
    if (name.empty() || name == "." || name == "..") {
      return {UniqueFd{}, HostEntryKind::NonRegular};
    }
    UniqueFd next(::openat(current.get(), name.c_str(), flags));
    if (!next && errno == ENOENT) {
      if (::mkdirat(current.get(), name.c_str(), 0700) != 0) {
        return {UniqueFd{}, errnoKind(errno)};
      }
      next = UniqueFd(::openat(current.get(), name.c_str(), flags));
    }
    if (!next) {
      return {UniqueFd{}, errnoKind(errno)};
    }
    if (index == count && !isPrivatePosixDirectory(next.get())) {
      return {UniqueFd{}, HostEntryKind::NonRegular};
    }
    current = std::move(next);
  }
  return {std::move(current), HostEntryKind::Directory};
}

bool recoverPosixOwnedTemporaries(int temporaryDirectory) {
  const int duplicate = ::dup(temporaryDirectory);
  if (duplicate < 0) {
    return false;
  }
  DIR *stream = ::fdopendir(duplicate);
  if (stream == nullptr) {
    ::close(duplicate);
    return false;
  }
  std::size_t recovered = 0;
  errno = 0;
  while (dirent *entry = ::readdir(stream)) {
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    struct stat status{};
    if (++recovered > maximumAbandonedTemporaries ||
        !isOwnedTemporaryName(name) ||
        ::fstatat(temporaryDirectory, entry->d_name, &status,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        !isPrivatePosixEntry(status) ||
        ::unlinkat(temporaryDirectory, entry->d_name, 0) != 0) {
      ::closedir(stream);
      return false;
    }
  }
  const int savedErrno = errno;
  ::closedir(stream);
  return savedErrno == 0 && ::fsync(temporaryDirectory) == 0;
}

struct PosixOverlayMutationPin {
  std::vector<UniqueFd> parentChain;
  std::vector<std::string> missingParents;
  std::string leaf;
  UniqueFd temporaryDirectory;

  int root() const noexcept { return parentChain.front().get(); }
  int parent() const noexcept { return parentChain.back().get(); }
  bool parentExists() const noexcept { return missingParents.empty(); }
};

std::optional<PosixOverlayMutationPin>
acquirePosixOverlayMutationPin(const fs::path &root,
                               std::string_view virtualPath) {
  const std::vector<std::string> components = splitNormalized(virtualPath);
  if (components.empty()) {
    return std::nullopt;
  }
  auto [rootDescriptor, rootKind] = openOrCreatePrivateOverlayRoot(root);
  if (!rootDescriptor || rootKind != HostEntryKind::Directory) {
    return std::nullopt;
  }
  PosixOverlayMutationPin pin;
  pin.parentChain.push_back(std::move(rootDescriptor));
  pin.leaf = components.back();
  int flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  for (std::size_t index = 0; index + 1 < components.size(); ++index) {
    UniqueFd next(::openat(pin.parent(), components[index].c_str(), flags));
    if (!next) {
      if (errno != ENOENT) {
        return std::nullopt;
      }
      pin.missingParents.insert(pin.missingParents.end(),
                                components.begin() + index,
                                components.end() - 1);
      break;
    }
    if (!isPrivatePosixDirectory(next.get())) {
      return std::nullopt;
    }
    pin.parentChain.push_back(std::move(next));
  }
  if (::mkdirat(pin.root(), internalTemporaryDirectoryName.data(), 0700) != 0 &&
      errno != EEXIST) {
    return std::nullopt;
  }
  pin.temporaryDirectory = UniqueFd(
      ::openat(pin.root(), internalTemporaryDirectoryName.data(), flags));
  if (!pin.temporaryDirectory ||
      !isPrivatePosixDirectory(pin.temporaryDirectory.get()) ||
      !recoverPosixOwnedTemporaries(pin.temporaryDirectory.get())) {
    return std::nullopt;
  }
  return pin;
}

bool ensurePosixPinnedParents(PosixOverlayMutationPin &pin) {
  int flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  for (const std::string &component : pin.missingParents) {
    if (::mkdirat(pin.parent(), component.c_str(), 0700) != 0 &&
        errno != EEXIST) {
      return false;
    }
    UniqueFd next(::openat(pin.parent(), component.c_str(), flags));
    if (!next || !isPrivatePosixDirectory(next.get())) {
      return false;
    }
    pin.parentChain.push_back(std::move(next));
  }
  pin.missingParents.clear();
  return true;
}

HostStatResult statAtPosixMutationTarget(const PosixOverlayMutationPin &pin) {
  if (!pin.parentExists()) {
    return {.kind = HostEntryKind::Missing};
  }
  struct stat status{};
  if (::fstatat(pin.parent(), pin.leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) !=
      0) {
    return {.kind = errnoKind(errno)};
  }
  if (S_ISDIR(status.st_mode)) {
    return {.kind = isPrivatePosixEntry(status) ? HostEntryKind::Directory
                                                : HostEntryKind::NonRegular};
  }
  if (!S_ISREG(status.st_mode) || status.st_nlink != 1 ||
      !isPrivatePosixEntry(status) || status.st_size < 0) {
    return {.kind = HostEntryKind::NonRegular};
  }
  return {.kind = HostEntryKind::Regular,
          .size = static_cast<std::uint64_t>(status.st_size)};
}

HostReadResult readAtPosixMutationTarget(const PosixOverlayMutationPin &pin,
                                         std::uint64_t maximumBytes) {
  if (!pin.parentExists()) {
    return {.kind = HostEntryKind::Missing};
  }
  int flags = O_RDONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  UniqueFd file(::openat(pin.parent(), pin.leaf.c_str(), flags));
  if (!file) {
    return {.kind = errnoKind(errno)};
  }
  struct stat before{};
  if (::fstat(file.get(), &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_nlink != 1 || !isPrivatePosixEntry(before) ||
      before.st_size < 0) {
    return {.kind = HostEntryKind::NonRegular};
  }
  const std::uint64_t size = static_cast<std::uint64_t>(before.st_size);
  if (size > maximumBytes || size > std::numeric_limits<std::size_t>::max()) {
    return {.kind = HostEntryKind::Regular, .limitExceeded = true};
  }
  HostReadResult result{.kind = HostEntryKind::Regular};
  try {
    result.bytes.resize(static_cast<std::size_t>(size));
  } catch (...) {
    return {.kind = HostEntryKind::IoError};
  }
  std::size_t offset = 0;
  while (offset < result.bytes.size()) {
    const ssize_t count = ::read(file.get(), result.bytes.data() + offset,
                                 result.bytes.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return {.kind = HostEntryKind::IoError};
    }
    offset += static_cast<std::size_t>(count);
  }
  struct stat after{};
  if (::fstat(file.get(), &after) != 0 || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || before.st_size != after.st_size ||
      before.st_nlink != after.st_nlink || !isPrivatePosixEntry(after)) {
    return {.kind = HostEntryKind::IoError};
  }
  return result;
}

bool secureReplaceOverlayFile(PosixOverlayMutationPin &pin,
                              std::span<const std::byte> contents) {
  if (!ensurePosixPinnedParents(pin)) {
    return false;
  }
  int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  UniqueFd output;
  std::string temporary;
  InternalTemporaryCleanup cleanup;
  for (std::size_t attempt = 0; attempt < maximumTemporaryCreateAttempts;
       ++attempt) {
    const auto candidate = uniqueOverlayTemporaryName();
    if (!candidate) {
      return false;
    }
    const int descriptor =
        ::openat(pin.temporaryDirectory.get(), candidate->c_str(), flags, 0600);
    if (descriptor >= 0) {
      output = UniqueFd(descriptor);
      temporary = *candidate;
      cleanup.arm(pin.temporaryDirectory.get(), temporary);
      break;
    }
    if (errno != EEXIST) {
      return false;
    }
  }
  if (!output || ::fchmod(output.get(), 0600) != 0) {
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const auto *data = reinterpret_cast<const char *>(contents.data() + offset);
    const ssize_t written =
        ::write(output.get(), data, contents.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      (void)cleanup.remove();
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  struct stat before{};
  if (::fsync(output.get()) != 0 || ::fstat(output.get(), &before) != 0 ||
      !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
      ::renameat(pin.temporaryDirectory.get(), temporary.c_str(), pin.parent(),
                 pin.leaf.c_str()) != 0) {
    (void)cleanup.remove();
    return false;
  }
  cleanup.disarm();
  int readFlags = O_RDONLY;
#ifdef O_CLOEXEC
  readFlags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  readFlags |= O_NOFOLLOW;
#endif
  UniqueFd installed(::openat(pin.parent(), pin.leaf.c_str(), readFlags));
  struct stat after{};
  return installed && ::fstat(installed.get(), &after) == 0 &&
         S_ISREG(after.st_mode) && after.st_nlink == 1 &&
         isPrivatePosixEntry(after) && before.st_dev == after.st_dev &&
         before.st_ino == after.st_ino && before.st_size == after.st_size &&
         ::fsync(pin.parent()) == 0 &&
         ::fsync(pin.temporaryDirectory.get()) == 0;
}

HostEntryKind secureCreateOverlayDirectory(PosixOverlayMutationPin &pin) {
  if (!ensurePosixPinnedParents(pin)) {
    return HostEntryKind::NonRegular;
  }
  if (::mkdirat(pin.parent(), pin.leaf.c_str(), 0700) != 0 && errno != EEXIST) {
    return errnoKind(errno);
  }
  struct stat status{};
  if (::fstatat(pin.parent(), pin.leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) !=
      0) {
    return errnoKind(errno);
  }
  if (!S_ISDIR(status.st_mode) || !isPrivatePosixEntry(status)) {
    return HostEntryKind::NonRegular;
  }
  return ::fsync(pin.parent()) == 0 ? HostEntryKind::Directory
                                    : HostEntryKind::IoError;
}

#else
class UniqueWindowsHandle {
public:
  UniqueWindowsHandle() = default;
  explicit UniqueWindowsHandle(HANDLE value) noexcept : value_(value) {}
  UniqueWindowsHandle(const UniqueWindowsHandle &) = delete;
  UniqueWindowsHandle &operator=(const UniqueWindowsHandle &) = delete;
  UniqueWindowsHandle(UniqueWindowsHandle &&other) noexcept
      : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
  UniqueWindowsHandle &operator=(UniqueWindowsHandle &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.value_, INVALID_HANDLE_VALUE));
    }
    return *this;
  }
  ~UniqueWindowsHandle() { reset(); }

  HANDLE get() const noexcept { return value_; }
  explicit operator bool() const noexcept {
    return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
  }

private:
  void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
    if (*this) {
      CloseHandle(value_);
    }
    value_ = value;
  }
  HANDLE value_ = INVALID_HANDLE_VALUE;
};

struct WindowsMetadata {
  HostEntryKind kind = HostEntryKind::IoError;
  FILE_ID_INFO identity{};
  std::uint64_t size = 0;
  std::uint32_t links = 0;
  FILETIME modified{};
};

struct WindowsHandleChain {
  fs::path currentPath;
  std::vector<UniqueWindowsHandle> handles;
  WindowsMetadata metadata;
  std::size_t rootIndex = 0;

  HANDLE leaf() const noexcept {
    return handles.empty() ? INVALID_HANDLE_VALUE : handles.back().get();
  }
};

bool verifyPrivateWindowsSecurity(HANDLE handle);

HostEntryKind windowsErrorKind(DWORD error) {
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
    return HostEntryKind::Missing;
  }
  if (error == ERROR_CANT_ACCESS_FILE || error == ERROR_INVALID_REPARSE_DATA ||
      error == ERROR_REPARSE_TAG_INVALID) {
    return HostEntryKind::NonRegular;
  }
  return HostEntryKind::IoError;
}

bool windowsMetadataFromHandle(HANDLE handle, WindowsMetadata &metadata) {
  FILE_ATTRIBUTE_TAG_INFO tag{};
  BY_HANDLE_FILE_INFORMATION information{};
  FILE_ID_INFO identity{};
  const bool disk = GetFileType(handle) == FILE_TYPE_DISK;
  if (!disk ||
      !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag,
                                    sizeof(tag)) ||
      !GetFileInformationByHandleEx(handle, FileIdInfo, &identity,
                                    sizeof(identity)) ||
      !GetFileInformationByHandle(handle, &information)) {
    return false;
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    metadata.kind = HostEntryKind::NonRegular;
    return true;
  }
  const bool directory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if (!directory && information.nNumberOfLinks != 1) {
    metadata.kind = HostEntryKind::NonRegular;
    return true;
  }
  metadata.kind = directory ? HostEntryKind::Directory : HostEntryKind::Regular;
  metadata.identity = identity;
  metadata.size =
      directory
          ? 0
          : (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
                information.nFileSizeLow;
  metadata.links = information.nNumberOfLinks;
  metadata.modified = information.ftLastWriteTime;
  return true;
}

bool sameWindowsIdentity(const WindowsMetadata &left,
                         const WindowsMetadata &right) {
  return left.identity.VolumeSerialNumber ==
             right.identity.VolumeSerialNumber &&
         std::memcmp(left.identity.FileId.Identifier,
                     right.identity.FileId.Identifier,
                     sizeof(left.identity.FileId.Identifier)) == 0;
}

bool appendExistingWindowsComponent(WindowsHandleChain &chain,
                                    const fs::path &component,
                                    DWORD desiredAccess,
                                    std::optional<bool> expectedDirectory,
                                    HostEntryKind &failureKind) {
  if (component.empty() || component == fs::path(L".") ||
      component == fs::path(L"..")) {
    failureKind = HostEntryKind::NonRegular;
    return false;
  }
  chain.currentPath /= component;
  DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT;
  if (!expectedDirectory || *expectedDirectory) {
    flags |= FILE_FLAG_BACKUP_SEMANTICS;
  } else {
    flags |= FILE_FLAG_SEQUENTIAL_SCAN;
  }
  UniqueWindowsHandle handle(CreateFileW(
      chain.currentPath.c_str(), desiredAccess | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags,
      nullptr));
  if (!handle) {
    failureKind = windowsErrorKind(GetLastError());
    return false;
  }
  WindowsMetadata metadata;
  if (!windowsMetadataFromHandle(handle.get(), metadata)) {
    failureKind = HostEntryKind::IoError;
    return false;
  }
  if (metadata.kind == HostEntryKind::NonRegular ||
      (expectedDirectory &&
       (metadata.kind == HostEntryKind::Directory) != *expectedDirectory)) {
    failureKind = HostEntryKind::NonRegular;
    return false;
  }
  chain.metadata = metadata;
  chain.handles.push_back(std::move(handle));
  return true;
}

std::optional<WindowsHandleChain>
openWindowsDirectoryChain(const fs::path &directory, HostEntryKind &failureKind,
                          DWORD rootDesiredAccess = 0) {
  std::error_code error;
  const fs::path absolute = fs::absolute(directory, error).lexically_normal();
  if (error || !absolute.is_absolute() || absolute.root_path().empty()) {
    failureKind = HostEntryKind::NonRegular;
    return std::nullopt;
  }
  WindowsHandleChain chain{.currentPath = absolute.root_path()};
  UniqueWindowsHandle root(CreateFileW(
      chain.currentPath.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!root) {
    failureKind = windowsErrorKind(GetLastError());
    return std::nullopt;
  }
  if (!windowsMetadataFromHandle(root.get(), chain.metadata) ||
      chain.metadata.kind != HostEntryKind::Directory) {
    failureKind = HostEntryKind::NonRegular;
    return std::nullopt;
  }
  chain.handles.push_back(std::move(root));
  std::vector<fs::path> components;
  for (const fs::path &component :
       absolute.lexically_relative(absolute.root_path())) {
    components.push_back(component);
  }
  for (std::size_t index = 0; index < components.size(); ++index) {
    if (!appendExistingWindowsComponent(
            chain, components[index],
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES |
                (index + 1 == components.size() ? rootDesiredAccess : 0),
            true, failureKind)) {
      return std::nullopt;
    }
  }
  chain.rootIndex = chain.handles.size() - 1;
  failureKind = HostEntryKind::Directory;
  return chain;
}

std::optional<WindowsHandleChain>
openWindowsPathAtRoot(const fs::path &root, std::string_view virtualPath,
                      DWORD desiredAccess,
                      std::optional<bool> expectedDirectory,
                      HostEntryKind &failureKind, bool requirePrivate = false) {
  auto chain = openWindowsDirectoryChain(root, failureKind,
                                         requirePrivate ? READ_CONTROL : 0);
  if (!chain) {
    return std::nullopt;
  }
  if (requirePrivate && !verifyPrivateWindowsSecurity(chain->leaf())) {
    failureKind = HostEntryKind::NonRegular;
    return std::nullopt;
  }
  const std::vector<std::string> components = splitNormalized(virtualPath);
  for (std::size_t index = 0; index < components.size(); ++index) {
    const bool final = index + 1 == components.size();
    if (!appendExistingWindowsComponent(
            *chain, pathFromUtf8(components[index]),
            (final ? desiredAccess
                   : FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES) |
                (requirePrivate ? READ_CONTROL : 0),
            final ? expectedDirectory : std::optional<bool>(true),
            failureKind)) {
      return std::nullopt;
    }
    if (requirePrivate && !verifyPrivateWindowsSecurity(chain->leaf())) {
      failureKind = HostEntryKind::NonRegular;
      return std::nullopt;
    }
  }
  if (components.empty() && expectedDirectory && !*expectedDirectory) {
    failureKind = HostEntryKind::NonRegular;
    return std::nullopt;
  }
  failureKind = chain->metadata.kind;
  return chain;
}

HostStatResult statAtRoot(const fs::path &root, std::string_view virtualPath) {
  HostEntryKind kind = HostEntryKind::IoError;
  auto chain = openWindowsPathAtRoot(root, virtualPath, FILE_READ_ATTRIBUTES,
                                     std::nullopt, kind);
  if (!chain) {
    return {.kind = kind};
  }
  return {.kind = chain->metadata.kind, .size = chain->metadata.size};
}

HostStatResult statAtPrivateOverlay(const fs::path &root,
                                    std::string_view virtualPath) {
  HostEntryKind kind = HostEntryKind::IoError;
  auto chain = openWindowsPathAtRoot(root, virtualPath,
                                     FILE_READ_ATTRIBUTES | READ_CONTROL,
                                     std::nullopt, kind, true);
  if (!chain) {
    return {.kind = kind};
  }
  return {.kind = chain->metadata.kind, .size = chain->metadata.size};
}

HostReadResult readWindowsHandleChain(WindowsHandleChain &chain,
                                      std::uint64_t maximumBytes) {
  const WindowsMetadata before = chain.metadata;
  if (before.size > maximumBytes ||
      before.size > std::numeric_limits<std::size_t>::max()) {
    return {.kind = HostEntryKind::Regular, .limitExceeded = true};
  }
  HostReadResult result{.kind = HostEntryKind::Regular};
  try {
    result.bytes.resize(static_cast<std::size_t>(before.size));
  } catch (...) {
    return {.kind = HostEntryKind::IoError};
  }
  std::size_t offset = 0;
  while (offset < result.bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
        result.bytes.size() - offset, std::numeric_limits<DWORD>::max()));
    DWORD read = 0;
    if (!ReadFile(chain.leaf(), result.bytes.data() + offset, chunk, &read,
                  nullptr) ||
        read == 0) {
      return {.kind = HostEntryKind::IoError};
    }
    offset += read;
  }
  WindowsMetadata after;
  if (!windowsMetadataFromHandle(chain.leaf(), after) ||
      after.kind != HostEntryKind::Regular ||
      !sameWindowsIdentity(before, after) || before.size != after.size ||
      before.links != after.links ||
      CompareFileTime(&before.modified, &after.modified) != 0) {
    return {.kind = HostEntryKind::IoError};
  }
  return result;
}

HostReadResult readAtRoot(const fs::path &root, std::string_view virtualPath,
                          std::uint64_t maximumBytes) {
  HostEntryKind kind = HostEntryKind::IoError;
  auto chain =
      openWindowsPathAtRoot(root, virtualPath, GENERIC_READ, false, kind);
  if (!chain) {
    return {.kind = kind};
  }
  return readWindowsHandleChain(*chain, maximumBytes);
}

HostReadResult readAtPrivateOverlay(const fs::path &root,
                                    std::string_view virtualPath,
                                    std::uint64_t maximumBytes) {
  HostEntryKind kind = HostEntryKind::IoError;
  auto chain = openWindowsPathAtRoot(
      root, virtualPath, GENERIC_READ | READ_CONTROL, false, kind, true);
  if (!chain) {
    return {.kind = kind};
  }
  return readWindowsHandleChain(*chain, maximumBytes);
}

HostEntryKind validatePrivateOverlayRoot(const fs::path &root) {
  return statAtPrivateOverlay(root, {}).kind;
}

struct WindowsDirectoryEntry {
  std::wstring name;
  DWORD attributes = 0;
  FILE_ID_128 fileId{};
  ULONGLONG volumeSerial = 0;
  bool hasFileId = false;
};

bool sameWindowsEnumeratedIdentity(const WindowsDirectoryEntry &entry,
                                   const WindowsMetadata &metadata) {
  if (!entry.hasFileId) {
    return true;
  }
  return entry.volumeSerial == metadata.identity.VolumeSerialNumber &&
         std::memcmp(entry.fileId.Identifier,
                     metadata.identity.FileId.Identifier,
                     sizeof(entry.fileId.Identifier)) == 0;
}

HostEntryKind
enumerateWindowsDirectory(HANDLE directory,
                          std::vector<WindowsDirectoryEntry> &entries,
                          std::size_t maximumEntries) {
  WindowsMetadata directoryMetadata;
  if (!windowsMetadataFromHandle(directory, directoryMetadata) ||
      directoryMetadata.kind != HostEntryKind::Directory) {
    return HostEntryKind::IoError;
  }
  alignas(FILE_ID_EXTD_DIR_INFO) std::array<std::byte, 64 * 1024> buffer{};
  bool restart = true;
  for (;;) {
    const FILE_INFO_BY_HANDLE_CLASS informationClass =
        restart ? FileIdExtdDirectoryRestartInfo : FileIdExtdDirectoryInfo;
    if (!GetFileInformationByHandleEx(directory, informationClass,
                                      buffer.data(),
                                      static_cast<DWORD>(buffer.size()))) {
      return GetLastError() == ERROR_NO_MORE_FILES ? HostEntryKind::Directory
                                                   : HostEntryKind::IoError;
    }
    restart = false;
    std::size_t offset = 0;
    for (;;) {
      if (offset + sizeof(FILE_ID_EXTD_DIR_INFO) > buffer.size()) {
        return HostEntryKind::IoError;
      }
      const auto *entry = reinterpret_cast<const FILE_ID_EXTD_DIR_INFO *>(
          buffer.data() + offset);
      constexpr std::size_t fileNameOffset =
          offsetof(FILE_ID_EXTD_DIR_INFO, FileName);
      if (entry->FileNameLength % sizeof(wchar_t) != 0 ||
          entry->FileNameLength > buffer.size() - offset - fileNameOffset) {
        return HostEntryKind::IoError;
      }
      const std::wstring name(entry->FileName,
                              entry->FileNameLength / sizeof(wchar_t));
      if (name != L"." && name != L"..") {
        entries.push_back(
            {.name = name,
             .attributes = entry->FileAttributes,
             .fileId = entry->FileId,
             .volumeSerial = directoryMetadata.identity.VolumeSerialNumber,
             .hasFileId = true});
        if (entries.size() > maximumEntries) {
          return HostEntryKind::LimitExceeded;
        }
      }
      if (entry->NextEntryOffset == 0) {
        break;
      }
      if (entry->NextEntryOffset > buffer.size() - offset) {
        return HostEntryKind::IoError;
      }
      offset += entry->NextEntryOffset;
    }
  }
}

std::optional<std::string> utf8FromWide(std::wstring_view value) {
  if (value.empty()) {
    return std::string{};
  }
  const int bytes = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) {
    return std::nullopt;
  }
  std::string result(static_cast<std::size_t>(bytes), '\0');
  return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                             static_cast<int>(value.size()), result.data(),
                             bytes, nullptr, nullptr) == bytes
             ? std::optional<std::string>(std::move(result))
             : std::nullopt;
}

std::pair<UniqueWindowsHandle, WindowsMetadata>
openWindowsChildNoFollow(const fs::path &parentPath,
                         const WindowsDirectoryEntry &entry,
                         DWORD desiredAccess) {
  DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT;
  if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    flags |= FILE_FLAG_BACKUP_SEMANTICS;
  }
  UniqueWindowsHandle child(CreateFileW(
      (parentPath / entry.name).c_str(), desiredAccess | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags,
      nullptr));
  WindowsMetadata metadata;
  if (!child || !windowsMetadataFromHandle(child.get(), metadata) ||
      metadata.kind == HostEntryKind::NonRegular ||
      ((entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) ||
      ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) !=
          (metadata.kind == HostEntryKind::Directory) ||
      !sameWindowsEnumeratedIdentity(entry, metadata)) {
    return {};
  }
  return {std::move(child), metadata};
}

bool verifyPrivateWindowsSecurity(HANDLE handle);

bool collectUsageFromWindowsDirectory(HANDLE directory,
                                      const fs::path &directoryPath,
                                      OverlayUsage &usage, std::size_t depth,
                                      bool overlayRoot) {
  std::vector<WindowsDirectoryEntry> entries;
  const std::size_t remaining = static_cast<std::size_t>(
      maximumOverlayNodes - std::min(usage.nodes, maximumOverlayNodes));
  const HostEntryKind enumeration =
      enumerateWindowsDirectory(directory, entries, remaining);
  if (enumeration != HostEntryKind::Directory) {
    usage.kind = enumeration;
    return false;
  }
  for (const WindowsDirectoryEntry &entry : entries) {
    if (overlayRoot && entry.name == L".asobmashow-internal") {
      continue;
    }
    auto [child, metadata] = openWindowsChildNoFollow(
        directoryPath, entry,
        ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
             ? FILE_LIST_DIRECTORY
             : FILE_READ_ATTRIBUTES) |
            READ_CONTROL);
    if (!child) {
      usage.kind = HostEntryKind::NonRegular;
      return false;
    }
    if (!verifyPrivateWindowsSecurity(child.get())) {
      usage.kind = HostEntryKind::NonRegular;
      return false;
    }
    if (usage.nodes == maximumOverlayNodes) {
      usage.kind = HostEntryKind::LimitExceeded;
      return false;
    }
    ++usage.nodes;
    if (metadata.kind == HostEntryKind::Directory) {
      if (usage.directories == maximumOverlayDirectories ||
          depth == maximumOverlayDepth) {
        usage.kind = HostEntryKind::LimitExceeded;
        return false;
      }
      ++usage.directories;
      if (!collectUsageFromWindowsDirectory(child.get(),
                                            directoryPath / entry.name, usage,
                                            depth + 1, false)) {
        return false;
      }
      continue;
    }
    if (usage.files == std::numeric_limits<std::uint64_t>::max() ||
        usage.bytes >
            std::numeric_limits<std::uint64_t>::max() - metadata.size) {
      usage.kind = HostEntryKind::NonRegular;
      return false;
    }
    ++usage.files;
    usage.bytes += metadata.size;
  }
  return true;
}

OverlayUsage collectUsage(const fs::path &root) {
  HostEntryKind kind = HostEntryKind::IoError;
  auto chain = openWindowsPathAtRoot(
      root, {}, FILE_LIST_DIRECTORY | READ_CONTROL, true, kind);
  if (!chain) {
    return kind == HostEntryKind::Missing ? OverlayUsage{}
                                          : OverlayUsage{.kind = kind};
  }
  if (!verifyPrivateWindowsSecurity(chain->leaf())) {
    return {.kind = HostEntryKind::NonRegular};
  }
  OverlayUsage usage;
  collectUsageFromWindowsDirectory(chain->leaf(), chain->currentPath, usage, 0,
                                   true);
  return usage;
}

std::pair<std::vector<std::string>, HostEntryKind>
listAtRoot(const fs::path &root, std::string_view virtualDirectory,
           const SkinPackageId &package, std::size_t maximumEntries) {
  HostEntryKind kind = HostEntryKind::IoError;
  auto chain = openWindowsPathAtRoot(root, virtualDirectory,
                                     FILE_LIST_DIRECTORY, true, kind);
  if (!chain) {
    return {{}, kind};
  }
  std::vector<WindowsDirectoryEntry> children;
  const HostEntryKind enumeration =
      enumerateWindowsDirectory(chain->leaf(), children, maximumEntries);
  if (enumeration != HostEntryKind::Directory) {
    return {{}, enumeration};
  }
  std::vector<std::string> result;
  result.reserve(children.size());
  for (const WindowsDirectoryEntry &childEntry : children) {
    auto [child, metadata] = openWindowsChildNoFollow(
        chain->currentPath, childEntry, FILE_READ_ATTRIBUTES);
    if (!child) {
      return {{}, HostEntryKind::NonRegular};
    }
    const auto name = utf8FromWide(childEntry.name);
    if (!name) {
      return {{}, HostEntryKind::NonRegular};
    }
    std::string candidate;
    if (!virtualDirectory.empty()) {
      candidate = std::string(virtualDirectory) + "/";
    }
    candidate += *name;
    const auto normalized = normalizeEntryPath(package, candidate);
    if (!normalized.entry ||
        normalized.entry->packageRelativePath != candidate) {
      return {{}, HostEntryKind::NonRegular};
    }
    result.push_back(std::move(candidate));
  }
  std::ranges::sort(result);
  return {std::move(result), HostEntryKind::Directory};
}

class PrivateWindowsSecurity {
public:
  PrivateWindowsSecurity() = default;
  PrivateWindowsSecurity(const PrivateWindowsSecurity &) = delete;
  PrivateWindowsSecurity &operator=(const PrivateWindowsSecurity &) = delete;
  ~PrivateWindowsSecurity() {
    if (accessControlList_ != nullptr) {
      LocalFree(accessControlList_);
    }
    if (token_ != nullptr) {
      CloseHandle(token_);
    }
  }

  bool initialize() {
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_)) {
      return false;
    }
    DWORD bytes = 0;
    GetTokenInformation(token_, TokenUser, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
      return false;
    }
    tokenUser_.resize(bytes);
    if (!GetTokenInformation(token_, TokenUser, tokenUser_.data(), bytes,
                             &bytes)) {
      return false;
    }
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(userSid());
    if (SetEntriesInAclW(1, &access, nullptr, &accessControlList_) !=
            ERROR_SUCCESS ||
        !InitializeSecurityDescriptor(&descriptor_,
                                      SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&descriptor_, userSid(), FALSE) ||
        !SetSecurityDescriptorDacl(&descriptor_, TRUE, accessControlList_,
                                   FALSE) ||
        !SetSecurityDescriptorControl(&descriptor_, SE_DACL_PROTECTED,
                                      SE_DACL_PROTECTED)) {
      return false;
    }
    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = &descriptor_;
    attributes_.bInheritHandle = FALSE;
    ready_ = true;
    return true;
  }

  bool ready() const noexcept { return ready_; }
  SECURITY_ATTRIBUTES *attributes() noexcept { return &attributes_; }

  bool verify(HANDLE handle) const {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result =
        GetSecurityInfo(handle, SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &dacl, nullptr, &descriptor);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    void *rawAce = nullptr;
    bool valid =
        result == ERROR_SUCCESS && descriptor != nullptr && owner != nullptr &&
        EqualSid(owner, userSid()) != FALSE && dacl != nullptr &&
        dacl->AceCount == 1 &&
        GetSecurityDescriptorControl(descriptor, &control, &revision) &&
        (control & SE_DACL_PROTECTED) != 0 && GetAce(dacl, 0, &rawAce) &&
        rawAce != nullptr;
    if (valid) {
      const auto *header = static_cast<const ACE_HEADER *>(rawAce);
      const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(rawAce);
      valid = header->AceType == ACCESS_ALLOWED_ACE_TYPE &&
              header->AceFlags == 0 && ace->Mask == FILE_ALL_ACCESS &&
              EqualSid(const_cast<DWORD *>(&ace->SidStart), userSid()) != FALSE;
    }
    if (descriptor != nullptr) {
      LocalFree(descriptor);
    }
    return valid;
  }

private:
  PSID userSid() const noexcept {
    return tokenUser_.empty()
               ? nullptr
               : reinterpret_cast<const TOKEN_USER *>(tokenUser_.data())
                     ->User.Sid;
  }

  HANDLE token_ = nullptr;
  std::vector<std::byte> tokenUser_;
  PACL accessControlList_ = nullptr;
  SECURITY_DESCRIPTOR descriptor_{};
  SECURITY_ATTRIBUTES attributes_{};
  bool ready_ = false;
};

PrivateWindowsSecurity &privateWindowsSecurity() {
  static PrivateWindowsSecurity security;
  static const bool initialized = security.initialize();
  (void)initialized;
  return security;
}

bool verifyPrivateWindowsSecurity(HANDLE handle) {
  PrivateWindowsSecurity &security = privateWindowsSecurity();
  return security.ready() && security.verify(handle);
}

bool appendWindowsOverlayDirectory(WindowsHandleChain &chain,
                                   const fs::path &component, bool create,
                                   bool requirePrivate,
                                   PrivateWindowsSecurity &security) {
  const fs::path candidate = chain.currentPath / component;
  if (create && !CreateDirectoryW(candidate.c_str(), security.attributes()) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    return false;
  }
  HostEntryKind kind = HostEntryKind::IoError;
  if (!appendExistingWindowsComponent(chain, component,
                                      FILE_LIST_DIRECTORY |
                                          FILE_READ_ATTRIBUTES | READ_CONTROL,
                                      true, kind)) {
    return false;
  }
  return !requirePrivate || security.verify(chain.leaf());
}

std::optional<WindowsHandleChain>
ensureWindowsOverlayDirectory(const fs::path &root,
                              std::string_view virtualDirectory) {
  PrivateWindowsSecurity &security = privateWindowsSecurity();
  if (!security.ready() || root.empty() || !root.is_absolute()) {
    return std::nullopt;
  }
  const fs::path base = root.parent_path();
  const fs::path absolute = base.lexically_normal();
  WindowsHandleChain chain{.currentPath = absolute.root_path()};
  UniqueWindowsHandle rootHandle(CreateFileW(
      chain.currentPath.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!rootHandle ||
      !windowsMetadataFromHandle(rootHandle.get(), chain.metadata) ||
      chain.metadata.kind != HostEntryKind::Directory) {
    return std::nullopt;
  }
  chain.handles.push_back(std::move(rootHandle));
  for (const fs::path &component :
       absolute.lexically_relative(absolute.root_path())) {
    if (!appendWindowsOverlayDirectory(chain, component, true, false,
                                       security)) {
      return std::nullopt;
    }
  }
  if (!appendWindowsOverlayDirectory(chain, root.filename(), true, true,
                                     security)) {
    return std::nullopt;
  }
  for (const std::string &component : splitNormalized(virtualDirectory)) {
    if (!appendWindowsOverlayDirectory(chain, pathFromUtf8(component), true,
                                       true, security)) {
      return std::nullopt;
    }
  }
  return chain;
}

bool markWindowsDeletion(HANDLE handle) {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                    sizeof(disposition)) != FALSE;
}

class InternalTemporaryCleanup {
public:
  InternalTemporaryCleanup() = default;
  InternalTemporaryCleanup(const InternalTemporaryCleanup &) = delete;
  InternalTemporaryCleanup &
  operator=(const InternalTemporaryCleanup &) = delete;
  ~InternalTemporaryCleanup() { (void)remove(); }

  void arm(HANDLE handle) noexcept {
    handle_ = handle;
    armed_ = true;
  }
  bool remove() {
    if (!armed_) {
      return true;
    }
    if (!markWindowsDeletion(handle_)) {
      return false;
    }
    armed_ = false;
    return true;
  }
  void disarm() noexcept { armed_ = false; }

private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  bool armed_ = false;
};

bool recoverWindowsOwnedTemporaries(WindowsHandleChain &temporary) {
  std::vector<WindowsDirectoryEntry> entries;
  if (enumerateWindowsDirectory(temporary.leaf(), entries,
                                maximumAbandonedTemporaries) !=
      HostEntryKind::Directory) {
    return false;
  }
  for (const WindowsDirectoryEntry &entry : entries) {
    const auto name = utf8FromWide(entry.name);
    if (!name || !isOwnedTemporaryName(*name)) {
      return false;
    }
    auto [child, metadata] =
        openWindowsChildNoFollow(temporary.currentPath, entry,
                                 DELETE | FILE_READ_ATTRIBUTES | READ_CONTROL);
    if (!child || metadata.kind != HostEntryKind::Regular ||
        !verifyPrivateWindowsSecurity(child.get()) ||
        !markWindowsDeletion(child.get())) {
      return false;
    }
  }
  return true;
}

std::optional<std::size_t>
countMissingWindowsOverlayParents(const fs::path &root,
                                  std::string_view virtualPath) {
  const std::vector<std::string> components = splitNormalized(virtualPath);
  std::string prefix;
  for (std::size_t index = 0; index + 1 < components.size(); ++index) {
    if (!prefix.empty()) {
      prefix.push_back('/');
    }
    prefix += components[index];
    const HostStatResult status = statAtPrivateOverlay(root, prefix);
    if (status.kind == HostEntryKind::Missing) {
      return components.size() - index - 1;
    }
    if (status.kind != HostEntryKind::Directory) {
      return std::nullopt;
    }
  }
  return 0;
}

struct WindowsOverlayMutationPin {
  WindowsHandleChain root;
  WindowsHandleChain temporary;
  std::size_t missingParents = 0;
};

std::optional<WindowsOverlayMutationPin>
acquireWindowsOverlayMutationPin(const fs::path &root,
                                 std::string_view virtualPath) {
  auto rootChain = ensureWindowsOverlayDirectory(root, {});
  auto temporary =
      ensureWindowsOverlayDirectory(root, internalTemporaryDirectoryName);
  const auto missing = countMissingWindowsOverlayParents(root, virtualPath);
  if (!rootChain || !temporary || !missing ||
      !recoverWindowsOwnedTemporaries(*temporary)) {
    return std::nullopt;
  }
  return WindowsOverlayMutationPin{.root = std::move(*rootChain),
                                   .temporary = std::move(*temporary),
                                   .missingParents = *missing};
}

bool secureReplaceAtWindowsParent(HANDLE parent, const fs::path &parentPath,
                                  const fs::path &temporaryPath,
                                  const std::wstring &leaf,
                                  std::span<const std::byte> contents,
                                  PrivateWindowsSecurity &security) {
  UniqueWindowsHandle output;
  InternalTemporaryCleanup cleanup;
  for (std::size_t attempt = 0; attempt < maximumTemporaryCreateAttempts;
       ++attempt) {
    const auto candidate = uniqueOverlayTemporaryName();
    if (!candidate) {
      return false;
    }
    UniqueWindowsHandle created(CreateFileW(
        (temporaryPath / pathFromUtf8(*candidate)).c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, security.attributes(), CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (created) {
      cleanup.arm(created.get());
      output = std::move(created);
      break;
    }
    if (GetLastError() != ERROR_FILE_EXISTS &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      return false;
    }
  }
  WindowsMetadata before;
  if (!output || !windowsMetadataFromHandle(output.get(), before) ||
      before.kind != HostEntryKind::Regular || !security.verify(output.get())) {
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
        contents.size() - offset, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(output.get(), contents.data() + offset, chunk, &written,
                   nullptr) ||
        written == 0) {
      (void)cleanup.remove();
      return false;
    }
    offset += written;
  }
  if (!FlushFileBuffers(output.get()) ||
      !windowsMetadataFromHandle(output.get(), before) ||
      before.kind != HostEntryKind::Regular || before.size != contents.size()) {
    (void)cleanup.remove();
    return false;
  }
  const std::size_t renameBytes =
      sizeof(FILE_RENAME_INFO) + leaf.size() * sizeof(wchar_t);
  std::vector<std::max_align_t> storage(
      (renameBytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
  auto &rename = *reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
  std::memset(&rename, 0, renameBytes);
  rename.ReplaceIfExists = TRUE;
  rename.RootDirectory = parent;
  rename.FileNameLength = static_cast<DWORD>(leaf.size() * sizeof(wchar_t));
  std::memcpy(rename.FileName, leaf.data(), rename.FileNameLength);
  if (!SetFileInformationByHandle(output.get(), FileRenameInfo, &rename,
                                  static_cast<DWORD>(renameBytes))) {
    (void)cleanup.remove();
    return false;
  }
  cleanup.disarm();
  WindowsDirectoryEntry installedEntry{.name = leaf};
  auto [installed, after] = openWindowsChildNoFollow(
      parentPath, installedEntry, FILE_READ_ATTRIBUTES | READ_CONTROL);
  return installed && after.kind == HostEntryKind::Regular &&
         sameWindowsIdentity(before, after) && before.size == after.size &&
         security.verify(installed.get());
}

bool secureReplaceOverlayFile(const WindowsOverlayMutationPin &pin,
                              const fs::path &root,
                              std::string_view virtualPath,
                              std::span<const std::byte> contents) {
  const std::vector<std::string> components = splitNormalized(virtualPath);
  if (components.empty()) {
    return false;
  }
  std::string parentVirtual;
  if (components.size() > 1) {
    parentVirtual = joinComponents(
        std::vector<std::string>(components.begin(), components.end() - 1));
  }
  auto parent = ensureWindowsOverlayDirectory(root, parentVirtual);
  PrivateWindowsSecurity &security = privateWindowsSecurity();
  return parent &&
         secureReplaceAtWindowsParent(
             parent->leaf(), parent->currentPath, pin.temporary.currentPath,
             pathFromUtf8(components.back()).native(), contents, security);
}

HostEntryKind secureCreateOverlayDirectory(const fs::path &root,
                                           std::string_view virtualDirectory) {
  const std::vector<std::string> components = splitNormalized(virtualDirectory);
  if (components.empty()) {
    return HostEntryKind::NonRegular;
  }
  std::string parentVirtual;
  if (components.size() > 1) {
    parentVirtual = joinComponents(
        std::vector<std::string>(components.begin(), components.end() - 1));
  }
  auto parent = ensureWindowsOverlayDirectory(root, parentVirtual);
  PrivateWindowsSecurity &security = privateWindowsSecurity();
  if (!parent ||
      !appendWindowsOverlayDirectory(*parent, pathFromUtf8(components.back()),
                                     true, true, security)) {
    return HostEntryKind::IoError;
  }
  return HostEntryKind::Directory;
}
#endif

enum class LinearLuaAtomKind : std::uint8_t { Literal, Any, CharacterClass };

struct LinearLuaAtom {
  LinearLuaAtomKind kind = LinearLuaAtomKind::Literal;
  unsigned char value = 0;
  bool inverted = false;
};

struct LinearLuaPattern {
  std::vector<LinearLuaAtom> atoms;
  bool anchoredStart = false;
  bool anchoredEnd = false;
};

bool isAsciiAlpha(unsigned char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool matchesCharacterClass(unsigned char className, unsigned char value) {
  switch (className) {
  case 'a':
    return isAsciiAlpha(value);
  case 'd':
    return value >= '0' && value <= '9';
  case 'l':
    return value >= 'a' && value <= 'z';
  case 's':
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
           value == '\f' || value == '\v';
  case 'u':
    return value >= 'A' && value <= 'Z';
  case 'w':
    return isAsciiAlpha(value) || (value >= '0' && value <= '9') ||
           value == '_';
  case 'x':
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F') ||
           (value >= 'a' && value <= 'f');
  case 'z':
    return value == 0;
  default:
    return false;
  }
}

std::optional<LinearLuaPattern>
compileLinearLuaPattern(std::string_view pattern) {
  // This fixed-width subset intentionally excludes captures, bracket classes,
  // and repetition operators so matching time is bounded by path × pattern.
  LinearLuaPattern compiled;
  std::size_t cursor = 0;
  if (!pattern.empty() && pattern.front() == '^') {
    compiled.anchoredStart = true;
    ++cursor;
  }
  while (cursor < pattern.size()) {
    const unsigned char value = static_cast<unsigned char>(pattern[cursor]);
    if (value == '$' && cursor + 1 == pattern.size()) {
      compiled.anchoredEnd = true;
      ++cursor;
      break;
    }
    if (value == '%') {
      if (++cursor == pattern.size()) {
        return std::nullopt;
      }
      const unsigned char escaped =
          static_cast<unsigned char>(pattern[cursor++]);
      unsigned char className = escaped;
      bool inverted = false;
      if (className >= 'A' && className <= 'Z') {
        className = static_cast<unsigned char>(className - 'A' + 'a');
        inverted = true;
      }
      if (std::string_view("adlsuwxz").find(static_cast<char>(className)) !=
          std::string_view::npos) {
        compiled.atoms.push_back({.kind = LinearLuaAtomKind::CharacterClass,
                                  .value = className,
                                  .inverted = inverted});
        continue;
      }
      if (isAsciiAlpha(escaped) || (escaped >= '0' && escaped <= '9')) {
        return std::nullopt;
      }
      compiled.atoms.push_back(
          {.kind = LinearLuaAtomKind::Literal, .value = escaped});
      continue;
    }
    if (value == '.') {
      compiled.atoms.push_back({.kind = LinearLuaAtomKind::Any});
      ++cursor;
      continue;
    }
    if (std::string_view("[]()*+-?").find(static_cast<char>(value)) !=
        std::string_view::npos) {
      return std::nullopt;
    }
    compiled.atoms.push_back(
        {.kind = LinearLuaAtomKind::Literal, .value = value});
    ++cursor;
  }
  return compiled;
}

bool matchesLinearLuaPattern(const LinearLuaPattern &pattern,
                             std::string_view input) {
  if (pattern.atoms.size() > input.size()) {
    return false;
  }
  const std::size_t lastStart = input.size() - pattern.atoms.size();
  const std::size_t firstStart = pattern.anchoredStart ? 0 : 0;
  const std::size_t finalStart = pattern.anchoredStart ? 0 : lastStart;
  for (std::size_t start = firstStart; start <= finalStart; ++start) {
    if (pattern.anchoredEnd && start + pattern.atoms.size() != input.size()) {
      continue;
    }
    bool matched = true;
    for (std::size_t index = 0; index < pattern.atoms.size(); ++index) {
      const LinearLuaAtom &atom = pattern.atoms[index];
      const unsigned char value =
          static_cast<unsigned char>(input[start + index]);
      bool atomMatched = false;
      switch (atom.kind) {
      case LinearLuaAtomKind::Literal:
        atomMatched = atom.value == value;
        break;
      case LinearLuaAtomKind::Any:
        atomMatched = value != 0;
        break;
      case LinearLuaAtomKind::CharacterClass:
        atomMatched = matchesCharacterClass(atom.value, value);
        if (atom.inverted) {
          atomMatched = !atomMatched;
        }
        break;
      }
      if (!atomMatched) {
        matched = false;
        break;
      }
    }
    if (matched) {
      return true;
    }
  }
  return false;
}

} // namespace

struct LuaSkinFileSystem::Impl {
  SkinRevisionReadView revision;
  SkinEntryId entry;
  SkinStorageRoots roots;
  std::vector<std::string> workingComponents;
  std::string workingDirectory;
  std::optional<fs::path> overlayRoot;
  bool allowDataWrites = false;
  SkinDataOverlayPolicy dataPolicy;
  mutable std::mutex operationMutex;
  std::atomic_bool renderPhase = false;
  mutable std::atomic_uint64_t renderReadsPerformed = 0;
  mutable std::atomic_uint64_t renderReadsDenied = 0;
  mutable std::atomic_uint64_t renderWritesPerformed = 0;
  mutable std::atomic_uint64_t renderWritesDenied = 0;
  mutable std::atomic_uint64_t renderDirectoryScansPerformed = 0;
  mutable std::atomic_uint64_t renderDirectoryScansDenied = 0;

  std::optional<SkinFileFailure> guard(RenderOperation operation,
                                       std::string_view virtualPath) const {
    if (!renderPhase.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    switch (operation) {
    case RenderOperation::Read:
      renderReadsDenied.fetch_add(1, std::memory_order_relaxed);
      break;
    case RenderOperation::Write:
      renderWritesDenied.fetch_add(1, std::memory_order_relaxed);
      break;
    case RenderOperation::DirectoryScan:
      renderDirectoryScansDenied.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    return failure(SkinFileError::RenderPhase, virtualPath,
                   "skin filesystem access is denied during render phase");
  }

  NormalizedReference normalize(std::string_view authored,
                                bool allowPackageRoot = false) const {
    return normalizeReference(entry.package, workingComponents, authored,
                              allowPackageRoot);
  }

  HostStatResult statLogicalData(std::string_view normalized) const {
    if (overlayRoot) {
      const HostStatResult overlay =
          statAtPrivateOverlay(*overlayRoot, normalized);
      if (overlay.kind != HostEntryKind::Missing) {
        return overlay;
      }
    }
    return statAtRoot(revision.root(), normalized);
  }

  HostReadResult readLogicalData(std::string_view normalized,
                                 std::uint64_t maximumBytes) const {
    if (overlayRoot) {
      HostReadResult overlay =
          readAtPrivateOverlay(*overlayRoot, normalized, maximumBytes);
      if (overlay.kind != HostEntryKind::Missing) {
        return overlay;
      }
    }
    return readAtRoot(revision.root(), normalized, maximumBytes);
  }

  SkinFileReadResult readNormalized(std::string_view normalized,
                                    SkinFileUse use,
                                    std::uint64_t maximumBytes) const {
    HostReadResult host;
    if (use == SkinFileUse::DataRead) {
      host = readLogicalData(normalized, maximumBytes);
    } else {
      host = readAtRoot(revision.root(), normalized, maximumBytes);
    }
    if (host.limitExceeded) {
      return {.failure = failure(SkinFileError::LimitExceeded, normalized,
                                 "skin virtual file exceeds the read limit")};
    }
    if (host.kind != HostEntryKind::Regular) {
      return {.failure = failure(errorForKind(host.kind), normalized,
                                 messageForKind(host.kind))};
    }
    if ((use == SkinFileUse::LuaEntry || use == SkinFileUse::LuaModule) &&
        !host.bytes.empty() &&
        host.bytes.front() == static_cast<std::byte>(0x1b)) {
      return {.failure = failure(SkinFileError::BinaryChunk, normalized,
                                 "binary Lua chunks are not allowed")};
    }
    return {.bytes = std::move(host.bytes)};
  }

  std::vector<std::string>
  moduleCandidates(std::string_view moduleName,
                   std::optional<SkinFileFailure> &moduleFailure) const {
    if (moduleName.empty() || moduleName.find('\0') != std::string_view::npos ||
        moduleName.front() == '/' ||
        moduleName.find('\\') != std::string_view::npos ||
        (moduleName.size() >= 2 && moduleName[1] == ':')) {
      moduleFailure = failure(SkinFileError::InvalidPath, moduleName,
                              "Lua module name is invalid");
      return {};
    }
    std::string substitution(moduleName);
    if (substitution.find('/') == std::string::npos) {
      std::ranges::replace(substitution, '.', '/');
    }
    std::vector<std::string> candidates;
    for (const std::string suffix :
         {std::string(".lua"), std::string("/init.lua")}) {
      const auto normalized = normalize(substitution + suffix);
      if (!normalized.path) {
        moduleFailure = normalized.failure;
        return {};
      }
      candidates.push_back(*normalized.path);
    }
    return candidates;
  }
};

LuaSkinFileSystemCreateResult
LuaSkinFileSystem::create(LuaSkinFileSystemOptions options) {
  const auto normalizedPackage =
      normalizePackageId(options.entry.package.directoryName);
  const auto normalizedEntry =
      normalizedPackage.package
          ? normalizeEntryPath(*normalizedPackage.package,
                               options.entry.packageRelativePath)
          : SkinEntryIdResult{};
  if (!normalizedEntry.entry || *normalizedEntry.entry != options.entry ||
      options.revision.revision().package != options.entry.package) {
    return {.failure = failure(SkinFileError::InvalidPath,
                               options.entry.packageRelativePath,
                               "skin entry does not belong to the revision")};
  }
  for (const std::string &component :
       splitNormalized(options.entry.packageRelativePath)) {
    if (!isPortableSkinFileComponent(component)) {
      return {.failure = failure(SkinFileError::InvalidPath,
                                 options.entry.packageRelativePath,
                                 "skin entry has an unsafe portable name")};
    }
  }
  const fs::path revisionRoot = options.revision.root().lexically_normal();
  if (revisionRoot.empty() || !revisionRoot.is_absolute() ||
      statAtRoot(revisionRoot, {}).kind != HostEntryKind::Directory) {
    return {.failure = failure(SkinFileError::IoError,
                               options.entry.packageRelativePath,
                               "skin revision root is unavailable")};
  }
  if (options.dataPolicy.maximumBytes >
          SkinDataOverlayPolicy::maximumPolicyBytes ||
      options.dataPolicy.maximumFiles >
          SkinDataOverlayPolicy::maximumPolicyFiles) {
    return {.failure = failure(SkinFileError::LimitExceeded,
                               options.entry.packageRelativePath,
                               "skin data policy exceeds the fixed limit")};
  }
  if (options.allowDataWrites && !options.profileId) {
    return {.failure = failure(SkinFileError::WrongUse,
                               options.entry.packageRelativePath,
                               "skin data writes require a profile")};
  }

  std::optional<fs::path> overlayRoot;
  if (options.profileId) {
    const auto canonicalOverlayBase =
        canonicalTrustedRoot(options.storageRoots.profileOverlays);
    if (!canonicalOverlayBase) {
      return {.failure = failure(SkinFileError::InvalidPath,
                                 options.entry.packageRelativePath,
                                 "skin data overlay identity is invalid")};
    }
    options.storageRoots.profileOverlays = *canonicalOverlayBase;
    const auto derived = deriveSkinPrivateOverlayRoot(
        options.storageRoots, *options.profileId, options.entry);
    if (!derived.root) {
      return {.failure = failure(SkinFileError::InvalidPath,
                                 options.entry.packageRelativePath,
                                 "skin data overlay identity is invalid")};
    }
    overlayRoot = derived.root->lexically_normal();
    if (!overlayRoot->is_absolute() ||
        overlayRoot->parent_path() !=
            options.storageRoots.profileOverlays.lexically_normal()) {
      return {.failure = failure(SkinFileError::InvalidPath,
                                 options.entry.packageRelativePath,
                                 "skin data overlay identity is invalid")};
    }
    const HostEntryKind status = validatePrivateOverlayRoot(*overlayRoot);
    if (status != HostEntryKind::Missing &&
        status != HostEntryKind::Directory) {
      return {.failure = failure(SkinFileError::NonRegular,
                                 options.entry.packageRelativePath,
                                 "skin data overlay root is not a directory")};
    }
  }

  auto impl = std::unique_ptr<Impl>(new Impl{
      .revision = options.revision,
      .entry = std::move(options.entry),
      .roots = std::move(options.storageRoots),
      .overlayRoot = std::move(overlayRoot),
      .allowDataWrites = options.allowDataWrites,
      .dataPolicy = options.dataPolicy,
  });
  const fs::path parent =
      pathFromUtf8(impl->entry.packageRelativePath).parent_path();
  impl->workingDirectory = utf8Path(parent);
  impl->workingComponents = splitNormalized(impl->workingDirectory);
  return {.fileSystem = std::unique_ptr<LuaSkinFileSystem>(
              new LuaSkinFileSystem(std::move(impl)))};
}

LuaSkinFileSystem::LuaSkinFileSystem(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

LuaSkinFileSystem::LuaSkinFileSystem(LuaSkinFileSystem &&) noexcept = default;
LuaSkinFileSystem &
LuaSkinFileSystem::operator=(LuaSkinFileSystem &&) noexcept = default;
LuaSkinFileSystem::~LuaSkinFileSystem() = default;

SkinFileResolveResult LuaSkinFileSystem::resolve(std::string_view virtualPath,
                                                 SkinFileUse use) const {
  const std::scoped_lock lock(impl_->operationMutex);
  const RenderOperation operation = use == SkinFileUse::DataWrite
                                        ? RenderOperation::Write
                                        : RenderOperation::Read;
  if (auto denied = impl_->guard(operation, virtualPath)) {
    return {.failure = std::move(denied)};
  }
  const auto normalized = impl_->normalize(virtualPath);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  if (use == SkinFileUse::LuaEntry &&
      *normalized.path != impl_->entry.packageRelativePath) {
    return {.failure =
                failure(SkinFileError::WrongUse, *normalized.path,
                        "Lua entry access is limited to the selected entry")};
  }
  if (use == SkinFileUse::DataWrite) {
    if (!impl_->allowDataWrites || !impl_->overlayRoot) {
      return {.failure = failure(SkinFileError::WrongUse, *normalized.path,
                                 "skin data writes are not enabled")};
    }
    return {.normalizedVirtualPath = *normalized.path};
  }
  const HostStatResult status =
      use == SkinFileUse::DataRead
          ? impl_->statLogicalData(*normalized.path)
          : statAtRoot(impl_->revision.root(), *normalized.path);
  if (status.kind != HostEntryKind::Regular) {
    return {.failure = failure(errorForKind(status.kind), *normalized.path,
                               messageForKind(status.kind))};
  }
  return {.normalizedVirtualPath = *normalized.path};
}

SkinFileResolveResult
LuaSkinFileSystem::resolveModule(std::string_view moduleName) const {
  const std::scoped_lock lock(impl_->operationMutex);
  if (auto denied = impl_->guard(RenderOperation::Read, moduleName)) {
    return {.failure = std::move(denied)};
  }
  std::optional<SkinFileFailure> moduleFailure;
  const std::vector<std::string> candidates =
      impl_->moduleCandidates(moduleName, moduleFailure);
  if (moduleFailure) {
    return {.failure = std::move(moduleFailure)};
  }
  for (const std::string &candidate : candidates) {
    const HostStatResult status = statAtRoot(impl_->revision.root(), candidate);
    if (status.kind == HostEntryKind::Regular) {
      return {.normalizedVirtualPath = candidate};
    }
    if (status.kind != HostEntryKind::Missing) {
      return {.failure = failure(errorForKind(status.kind), candidate,
                                 messageForKind(status.kind))};
    }
  }
  return {.failure = failure(SkinFileError::Missing, moduleName,
                             "Lua module is missing")};
}

SkinFileReadResult
LuaSkinFileSystem::readEntry(std::uint64_t maximumBytes) const {
  const std::scoped_lock lock(impl_->operationMutex);
  if (auto denied = impl_->guard(RenderOperation::Read,
                                 impl_->entry.packageRelativePath)) {
    return {.failure = std::move(denied)};
  }
  return impl_->readNormalized(impl_->entry.packageRelativePath,
                               SkinFileUse::LuaEntry, maximumBytes);
}

SkinFileReadResult LuaSkinFileSystem::read(std::string_view virtualPath,
                                           SkinFileUse use,
                                           std::uint64_t maximumBytes) const {
  const std::scoped_lock lock(impl_->operationMutex);
  if (auto denied = impl_->guard(RenderOperation::Read, virtualPath)) {
    return {.failure = std::move(denied)};
  }
  if (use == SkinFileUse::DataWrite) {
    return {.failure = failure(SkinFileError::WrongUse, virtualPath,
                               "write use cannot be read")};
  }
  const auto normalized = impl_->normalize(virtualPath);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  if (use == SkinFileUse::LuaEntry &&
      *normalized.path != impl_->entry.packageRelativePath) {
    return {.failure =
                failure(SkinFileError::WrongUse, *normalized.path,
                        "Lua entry access is limited to the selected entry")};
  }
  return impl_->readNormalized(*normalized.path, use, maximumBytes);
}

SkinFileReadResult
LuaSkinFileSystem::readModule(std::string_view moduleName,
                              std::uint64_t maximumBytes) const {
  const std::scoped_lock lock(impl_->operationMutex);
  if (auto denied = impl_->guard(RenderOperation::Read, moduleName)) {
    return {.failure = std::move(denied)};
  }
  std::optional<SkinFileFailure> moduleFailure;
  const std::vector<std::string> candidates =
      impl_->moduleCandidates(moduleName, moduleFailure);
  if (moduleFailure) {
    return {.failure = std::move(moduleFailure)};
  }
  for (const std::string &candidate : candidates) {
    SkinFileReadResult result =
        impl_->readNormalized(candidate, SkinFileUse::LuaModule, maximumBytes);
    if (!(result.failure && result.failure->code == SkinFileError::Missing)) {
      return result;
    }
  }
  return {.failure = failure(SkinFileError::Missing, moduleName,
                             "Lua module is missing")};
}

SkinFileListResult LuaSkinFileSystem::list(std::string_view virtualDirectory,
                                           std::string_view luaPattern,
                                           std::size_t maximumEntries) const {
  const std::scoped_lock lock(impl_->operationMutex);
  if (auto denied =
          impl_->guard(RenderOperation::DirectoryScan, virtualDirectory)) {
    return {.failure = std::move(denied)};
  }
  if (luaPattern.size() > SkinPackagePolicy::maxPathBytes) {
    return {.failure = failure(SkinFileError::LimitExceeded, virtualDirectory,
                               "Lua file-list pattern exceeds its limit")};
  }
  std::optional<LinearLuaPattern> compiledPattern;
  if (!luaPattern.empty()) {
    compiledPattern = compileLinearLuaPattern(luaPattern);
    if (!compiledPattern) {
      return {.failure = failure(SkinFileError::InvalidPath, virtualDirectory,
                                 "Lua file-list pattern is invalid")};
    }
  }
  const auto normalized = impl_->normalize(virtualDirectory, true);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  const std::size_t boundedMaximum =
      std::min(maximumEntries, maximumListingEntries);
  auto [entries, kind] = listAtRoot(impl_->revision.root(), *normalized.path,
                                    impl_->entry.package, boundedMaximum);
  if (kind == HostEntryKind::LimitExceeded) {
    return {.failure = failure(SkinFileError::LimitExceeded, *normalized.path,
                               "skin directory listing exceeds its limit")};
  }
  if (kind != HostEntryKind::Directory) {
    return {.failure = failure(errorForKind(kind), *normalized.path,
                               messageForKind(kind))};
  }
  if (compiledPattern) {
    std::erase_if(entries, [&compiledPattern](const std::string &entry) {
      return !matchesLinearLuaPattern(*compiledPattern, entry);
    });
  }
  if (entries.size() > boundedMaximum) {
    return {.failure = failure(SkinFileError::LimitExceeded, *normalized.path,
                               "skin directory listing exceeds its limit")};
  }
  return {.entries = std::move(entries)};
}

SkinFileWriteResult
LuaSkinFileSystem::writeData(std::string_view virtualPath,
                             std::span<const std::byte> bytes, bool append) {
  const std::scoped_lock lock(impl_->operationMutex);
  if (auto denied = impl_->guard(RenderOperation::Write, virtualPath)) {
    return {.failure = std::move(denied)};
  }
  const auto normalized = impl_->normalize(virtualPath);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  if (!impl_->allowDataWrites || !impl_->overlayRoot) {
    return {.failure = failure(SkinFileError::WrongUse, *normalized.path,
                               "skin data writes are not enabled")};
  }
  if (bytes.size() > impl_->dataPolicy.maximumBytes) {
    return {.failure = failure(SkinFileError::QuotaExceeded, *normalized.path,
                               "skin data write exceeds its byte quota")};
  }
  const std::scoped_lock overlayLock(overlayMutationMutex);
#if defined(_WIN32)
  auto overlayPin =
      acquireWindowsOverlayMutationPin(*impl_->overlayRoot, *normalized.path);
#else
  auto overlayPin =
      acquirePosixOverlayMutationPin(*impl_->overlayRoot, *normalized.path);
#endif
  if (!overlayPin) {
    return {.failure = failure(SkinFileError::NonRegular, *normalized.path,
                               "skin data overlay root is not private")};
  }
#if defined(_WIN32)
  OverlayUsage usage = collectUsage(*impl_->overlayRoot);
  const HostStatResult existing =
      statAtRoot(*impl_->overlayRoot, *normalized.path);
#else
  OverlayUsage usage = collectUsage(overlayPin->root());
  const HostStatResult existing = statAtPosixMutationTarget(*overlayPin);
#endif
  if (usage.kind != HostEntryKind::Regular) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure =
                failure(errorForKind(usage.kind), *normalized.path,
                        "skin data overlay contains a non-regular entry")};
  }
  if (existing.kind != HostEntryKind::Missing &&
      existing.kind != HostEntryKind::Regular) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(errorForKind(existing.kind), *normalized.path,
                               messageForKind(existing.kind))};
  }

  std::vector<std::byte> replacement;
  if (append) {
#if defined(_WIN32)
    HostReadResult prior = impl_->readLogicalData(
        *normalized.path, impl_->dataPolicy.maximumBytes);
#else
    HostReadResult prior =
        readAtPosixMutationTarget(*overlayPin, impl_->dataPolicy.maximumBytes);
    if (prior.kind == HostEntryKind::Missing) {
      prior = readAtRoot(impl_->revision.root(), *normalized.path,
                         impl_->dataPolicy.maximumBytes);
    }
#endif
    if (prior.limitExceeded) {
      return {.resultingBytes = usage.bytes,
              .resultingFiles = usage.files,
              .failure = failure(SkinFileError::QuotaExceeded, *normalized.path,
                                 "skin data append exceeds its byte quota")};
    }
    if (prior.kind != HostEntryKind::Missing &&
        prior.kind != HostEntryKind::Regular) {
      return {.resultingBytes = usage.bytes,
              .resultingFiles = usage.files,
              .failure = failure(errorForKind(prior.kind), *normalized.path,
                                 messageForKind(prior.kind))};
    }
    if (prior.kind == HostEntryKind::Regular) {
      replacement = std::move(prior.bytes);
    }
  }
  if (bytes.size() > impl_->dataPolicy.maximumBytes - replacement.size()) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::QuotaExceeded, *normalized.path,
                               "skin data write exceeds its byte quota")};
  }
  const std::uint64_t priorSize =
      existing.kind == HostEntryKind::Regular ? existing.size : 0;
  const std::uint64_t resultingFiles =
      usage.files + (existing.kind == HostEntryKind::Missing ? 1U : 0U);
  const std::uint64_t replacementSize = replacement.size() + bytes.size();
  if (usage.bytes < priorSize ||
      replacementSize > std::numeric_limits<std::uint64_t>::max() -
                            (usage.bytes - priorSize)) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::QuotaExceeded, *normalized.path,
                               "skin data write exceeds its byte quota")};
  }
  const std::uint64_t resultingBytes =
      usage.bytes - priorSize + replacementSize;
  if (resultingBytes > impl_->dataPolicy.maximumBytes ||
      resultingFiles > impl_->dataPolicy.maximumFiles) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::QuotaExceeded, *normalized.path,
                               "skin data overlay quota is exhausted")};
  }
#if defined(_WIN32)
  const std::uint64_t createdDirectories = overlayPin->missingParents;
#else
  const std::uint64_t createdDirectories = overlayPin->missingParents.size();
#endif
  const std::uint64_t createdNodes =
      createdDirectories + (existing.kind == HostEntryKind::Missing ? 1U : 0U);
  if (usage.directories > maximumOverlayDirectories - createdDirectories ||
      usage.nodes > maximumOverlayNodes - createdNodes) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::QuotaExceeded, *normalized.path,
                               "skin data overlay node quota is exhausted")};
  }
  try {
    replacement.insert(replacement.end(), bytes.begin(), bytes.end());
  } catch (...) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure =
                failure(SkinFileError::LimitExceeded, *normalized.path,
                        "skin data write could not allocate its buffer")};
  }

#if defined(_WIN32)
  const HostStatResult rechecked =
      statAtRoot(*impl_->overlayRoot, *normalized.path);
#else
  const HostStatResult rechecked = statAtPosixMutationTarget(*overlayPin);
#endif
  if (rechecked.kind != existing.kind ||
      (rechecked.kind == HostEntryKind::Regular &&
       rechecked.size != existing.size)) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin data target changed during replacement")};
  }
#if defined(_WIN32)
  if (!secureReplaceOverlayFile(*overlayPin, *impl_->overlayRoot,
                                *normalized.path, replacement)) {
#else
  if (!secureReplaceOverlayFile(*overlayPin, replacement)) {
#endif
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin data atomic replacement failed")};
  }
  return {.resultingBytes = resultingBytes, .resultingFiles = resultingFiles};
}

SkinFileWriteResult
LuaSkinFileSystem::mkdirData(std::string_view virtualDirectory) {
  const std::scoped_lock lock(impl_->operationMutex);
  if (auto denied = impl_->guard(RenderOperation::Write, virtualDirectory)) {
    return {.failure = std::move(denied)};
  }
  const auto normalized = impl_->normalize(virtualDirectory);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  if (!impl_->allowDataWrites || !impl_->overlayRoot) {
    return {.failure = failure(SkinFileError::WrongUse, *normalized.path,
                               "skin data writes are not enabled")};
  }
  const std::scoped_lock overlayLock(overlayMutationMutex);
#if defined(_WIN32)
  auto overlayPin =
      acquireWindowsOverlayMutationPin(*impl_->overlayRoot, *normalized.path);
#else
  auto overlayPin =
      acquirePosixOverlayMutationPin(*impl_->overlayRoot, *normalized.path);
#endif
  if (!overlayPin) {
    return {.failure = failure(SkinFileError::NonRegular, *normalized.path,
                               "skin data overlay root is not private")};
  }
#if defined(_WIN32)
  OverlayUsage usage = collectUsage(*impl_->overlayRoot);
#else
  OverlayUsage usage = collectUsage(overlayPin->root());
#endif
  if (usage.kind != HostEntryKind::Regular) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure =
                failure(errorForKind(usage.kind), *normalized.path,
                        "skin data overlay contains a non-regular entry")};
  }
  const fs::path relative = pathFromUtf8(*normalized.path);
  const std::string parentVirtual = utf8Path(relative.parent_path());
  if (!parentVirtual.empty()) {
    const HostStatResult packageParent =
        statAtRoot(impl_->revision.root(), parentVirtual);
#if defined(_WIN32)
    const HostStatResult overlayParent =
        statAtRoot(*impl_->overlayRoot, parentVirtual);
#else
    const HostStatResult overlayParent{.kind = overlayPin->parentExists()
                                                   ? HostEntryKind::Directory
                                                   : HostEntryKind::Missing};
#endif
    if (overlayParent.kind != HostEntryKind::Directory &&
        packageParent.kind != HostEntryKind::Directory) {
      return {.resultingBytes = usage.bytes,
              .resultingFiles = usage.files,
              .failure = failure(SkinFileError::Missing, *normalized.path,
                                 "skin data directory parent is missing")};
    }
  }
#if defined(_WIN32)
  const HostStatResult existing =
      statAtRoot(*impl_->overlayRoot, *normalized.path);
#else
  const HostStatResult existing = statAtPosixMutationTarget(*overlayPin);
#endif
  if (existing.kind == HostEntryKind::Directory) {
    return {.resultingBytes = usage.bytes, .resultingFiles = usage.files};
  }
  if (existing.kind != HostEntryKind::Missing) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(errorForKind(existing.kind), *normalized.path,
                               messageForKind(existing.kind))};
  }
#if defined(_WIN32)
  const std::uint64_t createdDirectories = overlayPin->missingParents + 1U;
#else
  const std::uint64_t createdDirectories =
      overlayPin->missingParents.size() + 1U;
#endif
  if (usage.directories > maximumOverlayDirectories - createdDirectories ||
      usage.nodes > maximumOverlayNodes - createdDirectories) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::QuotaExceeded, *normalized.path,
                               "skin data overlay node quota is exhausted")};
  }
#if defined(_WIN32)
  if (secureCreateOverlayDirectory(*impl_->overlayRoot, *normalized.path) !=
      HostEntryKind::Directory) {
#else
  if (secureCreateOverlayDirectory(*overlayPin) != HostEntryKind::Directory) {
#endif
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin data directory creation failed")};
  }
  return {.resultingBytes = usage.bytes, .resultingFiles = usage.files};
}

SkinFileRenderTransitionResult LuaSkinFileSystem::enterRenderPhase() {
  const std::scoped_lock lock(impl_->operationMutex);
  impl_->renderPhase.store(true, std::memory_order_release);
  return {.ok = true};
}

SkinFileActivityCounters LuaSkinFileSystem::activityCounters() const noexcept {
  return {
      .renderReadsPerformed =
          impl_->renderReadsPerformed.load(std::memory_order_relaxed),
      .renderReadsDenied =
          impl_->renderReadsDenied.load(std::memory_order_relaxed),
      .renderWritesPerformed =
          impl_->renderWritesPerformed.load(std::memory_order_relaxed),
      .renderWritesDenied =
          impl_->renderWritesDenied.load(std::memory_order_relaxed),
      .renderDirectoryScansPerformed =
          impl_->renderDirectoryScansPerformed.load(std::memory_order_relaxed),
      .renderDirectoryScansDenied =
          impl_->renderDirectoryScansDenied.load(std::memory_order_relaxed),
  };
}

} // namespace skin
