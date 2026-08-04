#include "LuaSkinFileSystem.h"

#include "../../AtomicFile.h"
#include "../package/SkinPathPolicy.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

#if defined(_WIN32)
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
  IoError,
};

enum class RenderOperation : std::uint8_t { Read, Write, DirectoryScan };

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

std::optional<fs::path> canonicalTrustedRoot(const fs::path &root) {
  if (root.empty() || !root.is_absolute()) {
    return std::nullopt;
  }
  const fs::path normalized = root.lexically_normal();
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
openDirectoryNoFollow(const fs::path &root, std::string_view virtualDirectory) {
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
  for (const std::string &component : splitNormalized(virtualDirectory)) {
    UniqueFd next(::openat(current.get(), component.c_str(), flags));
    if (!next) {
      return {UniqueFd{}, errnoKind(errno)};
    }
    current = std::move(next);
  }
  return {std::move(current), HostEntryKind::Directory};
}

HostStatResult statAtRoot(const fs::path &root, std::string_view virtualPath) {
  const std::vector<std::string> components = splitNormalized(virtualPath);
  if (components.empty()) {
    auto [directory, kind] = openDirectoryNoFollow(root, {});
    return {.kind = directory ? HostEntryKind::Directory : kind};
  }
  std::string parent;
  if (components.size() > 1) {
    parent = joinComponents(
        std::vector<std::string>(components.begin(), components.end() - 1));
  }
  auto [directory, directoryKind] = openDirectoryNoFollow(root, parent);
  if (!directory) {
    return {.kind = directoryKind};
  }
  struct stat status{};
  if (::fstatat(directory.get(), components.back().c_str(), &status,
                AT_SYMLINK_NOFOLLOW) != 0) {
    return {.kind = errnoKind(errno)};
  }
  if (S_ISDIR(status.st_mode)) {
    return {.kind = HostEntryKind::Directory};
  }
  if (!S_ISREG(status.st_mode) || status.st_nlink != 1) {
    return {.kind = HostEntryKind::NonRegular};
  }
  return {.kind = HostEntryKind::Regular,
          .size = static_cast<std::uint64_t>(status.st_size)};
}

HostReadResult readAtRoot(const fs::path &root, std::string_view virtualPath,
                          std::uint64_t maximumBytes) {
  const std::vector<std::string> components = splitNormalized(virtualPath);
  if (components.empty()) {
    return {.kind = HostEntryKind::Directory};
  }
  std::string parent;
  if (components.size() > 1) {
    parent = joinComponents(
        std::vector<std::string>(components.begin(), components.end() - 1));
  }
  auto [directory, directoryKind] = openDirectoryNoFollow(root, parent);
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
  if (!S_ISREG(before.st_mode) || before.st_nlink != 1) {
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
      after.st_nlink != 1) {
    return {.kind = HostEntryKind::IoError};
  }
  return result;
}

bool ensureAbsoluteDirectoryNoFollow(const fs::path &directory) {
  const fs::path normalized = directory.lexically_normal();
  if (!normalized.is_absolute() || normalized.root_path().empty()) {
    return false;
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
    return false;
  }
  for (const fs::path &component : normalized.relative_path()) {
    const std::string name = component.string();
    if (name.empty() || name == "." || name == "..") {
      return false;
    }
    UniqueFd next(::openat(current.get(), name.c_str(), flags));
    if (!next && errno == ENOENT) {
      if (::mkdirat(current.get(), name.c_str(), 0700) != 0 &&
          errno != EEXIST) {
        return false;
      }
      next = UniqueFd(::openat(current.get(), name.c_str(), flags));
    }
    if (!next) {
      return false;
    }
    current = std::move(next);
  }
  return true;
}

bool collectUsageFromDirectory(int directory, OverlayUsage &usage) {
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
    struct stat status{};
    if (::fstatat(directory, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) !=
        0) {
      ::closedir(stream);
      usage.kind = HostEntryKind::IoError;
      return false;
    }
    if (S_ISDIR(status.st_mode)) {
      int flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
      flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
      flags |= O_NOFOLLOW;
#endif
      UniqueFd child(::openat(directory, entry->d_name, flags));
      if (!child || !collectUsageFromDirectory(child.get(), usage)) {
        ::closedir(stream);
        if (usage.kind == HostEntryKind::Regular) {
          usage.kind = HostEntryKind::NonRegular;
        }
        return false;
      }
      continue;
    }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        status.st_size < 0 ||
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

OverlayUsage collectUsage(const fs::path &root) {
  auto [directory, kind] = openDirectoryNoFollow(root, {});
  if (!directory) {
    if (kind == HostEntryKind::Missing) {
      return {};
    }
    return {.kind = kind};
  }
  OverlayUsage usage;
  collectUsageFromDirectory(directory.get(), usage);
  return usage;
}

std::pair<std::vector<std::string>, HostEntryKind>
listAtRoot(const fs::path &root, std::string_view virtualDirectory,
           const SkinPackageId &package) {
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

#else
HostStatResult statAtRoot(const fs::path &root, std::string_view virtualPath) {
  fs::path current = root;
  std::error_code error;
  const auto rootStatus = fs::symlink_status(current, error);
  if (error == std::errc::no_such_file_or_directory) {
    return {.kind = HostEntryKind::Missing};
  }
  if (error || !fs::is_directory(rootStatus) || fs::is_symlink(rootStatus)) {
    return {.kind = HostEntryKind::NonRegular};
  }
  for (const std::string &component : splitNormalized(virtualPath)) {
    current /= pathFromUtf8(component);
    const auto status = fs::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) {
      return {.kind = HostEntryKind::Missing};
    }
    if (error || fs::is_symlink(status)) {
      return {.kind = HostEntryKind::NonRegular};
    }
  }
  const auto status = fs::symlink_status(current, error);
  if (error) {
    return {.kind = HostEntryKind::IoError};
  }
  if (fs::is_directory(status)) {
    return {.kind = HostEntryKind::Directory};
  }
  if (!fs::is_regular_file(status) ||
      fs::hard_link_count(current, error) != 1 || error) {
    return {.kind = HostEntryKind::NonRegular};
  }
  const std::uint64_t size = fs::file_size(current, error);
  return error ? HostStatResult{.kind = HostEntryKind::IoError}
               : HostStatResult{.kind = HostEntryKind::Regular, .size = size};
}

HostReadResult readAtRoot(const fs::path &root, std::string_view virtualPath,
                          std::uint64_t maximumBytes) {
  const HostStatResult status = statAtRoot(root, virtualPath);
  if (status.kind != HostEntryKind::Regular) {
    return {.kind = status.kind};
  }
  if (status.size > maximumBytes ||
      status.size > std::numeric_limits<std::size_t>::max()) {
    return {.kind = HostEntryKind::Regular, .limitExceeded = true};
  }
  std::ifstream input(root / pathFromUtf8(virtualPath), std::ios::binary);
  if (!input) {
    return {.kind = HostEntryKind::IoError};
  }
  HostReadResult result{.kind = HostEntryKind::Regular};
  result.bytes.resize(static_cast<std::size_t>(status.size));
  input.read(reinterpret_cast<char *>(result.bytes.data()),
             static_cast<std::streamsize>(result.bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(result.bytes.size())) {
    return {.kind = HostEntryKind::IoError};
  }
  return result;
}

bool ensureAbsoluteDirectoryNoFollow(const fs::path &directory) {
  std::error_code error;
  fs::path current = directory.root_path();
  for (const fs::path &component :
       directory.lexically_normal().relative_path()) {
    current /= component;
    auto status = fs::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      if (!fs::create_directory(current, error) || error) {
        return false;
      }
      status = fs::symlink_status(current, error);
    }
    if (error || fs::is_symlink(status) || !fs::is_directory(status)) {
      return false;
    }
  }
  return true;
}

OverlayUsage collectUsage(const fs::path &root) {
  const HostStatResult rootStatus = statAtRoot(root, {});
  if (rootStatus.kind == HostEntryKind::Missing) {
    return {};
  }
  if (rootStatus.kind != HostEntryKind::Directory) {
    return {.kind = rootStatus.kind};
  }
  OverlayUsage usage;
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; ++iterator) {
    const auto status = iterator->symlink_status(error);
    if (error || fs::is_symlink(status)) {
      return {.kind = HostEntryKind::NonRegular};
    }
    if (fs::is_directory(status)) {
      continue;
    }
    if (!fs::is_regular_file(status) ||
        fs::hard_link_count(iterator->path(), error) != 1 || error) {
      return {.kind = HostEntryKind::NonRegular};
    }
    ++usage.files;
    usage.bytes += iterator->file_size(error);
    if (error) {
      return {.kind = HostEntryKind::IoError};
    }
  }
  return error ? OverlayUsage{.kind = HostEntryKind::IoError} : usage;
}

std::pair<std::vector<std::string>, HostEntryKind>
listAtRoot(const fs::path &root, std::string_view virtualDirectory,
           const SkinPackageId &package) {
  const fs::path directory = root / pathFromUtf8(virtualDirectory);
  const HostStatResult directoryStatus = statAtRoot(root, virtualDirectory);
  if (directoryStatus.kind != HostEntryKind::Directory) {
    return {{}, directoryStatus.kind};
  }
  std::vector<std::string> result;
  std::error_code error;
  for (fs::directory_iterator iterator(directory, error), end;
       !error && iterator != end; ++iterator) {
    const auto status = iterator->symlink_status(error);
    if (error || fs::is_symlink(status) ||
        (!fs::is_directory(status) && !fs::is_regular_file(status)) ||
        (fs::is_regular_file(status) &&
         fs::hard_link_count(iterator->path(), error) != 1)) {
      return {{}, HostEntryKind::NonRegular};
    }
    std::string candidate;
    if (!virtualDirectory.empty()) {
      candidate = std::string(virtualDirectory) + "/";
    }
    candidate += utf8Path(iterator->path().filename());
    const auto normalized = normalizeEntryPath(package, candidate);
    if (!normalized.entry ||
        normalized.entry->packageRelativePath != candidate) {
      return {{}, HostEntryKind::NonRegular};
    }
    result.push_back(std::move(candidate));
  }
  if (error) {
    return {{}, HostEntryKind::IoError};
  }
  std::ranges::sort(result);
  return {std::move(result), HostEntryKind::Directory};
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
      const HostStatResult overlay = statAtRoot(*overlayRoot, normalized);
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
          readAtRoot(*overlayRoot, normalized, maximumBytes);
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
    const HostStatResult status = statAtRoot(*overlayRoot, {});
    if (status.kind != HostEntryKind::Missing &&
        status.kind != HostEntryKind::Directory) {
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
  auto [entries, kind] = listAtRoot(impl_->revision.root(), *normalized.path,
                                    impl_->entry.package);
  if (kind != HostEntryKind::Directory) {
    return {.failure = failure(errorForKind(kind), *normalized.path,
                               messageForKind(kind))};
  }
  if (compiledPattern) {
    std::erase_if(entries, [&compiledPattern](const std::string &entry) {
      return !matchesLinearLuaPattern(*compiledPattern, entry);
    });
  }
  if (entries.size() > maximumEntries) {
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
  OverlayUsage usage = collectUsage(*impl_->overlayRoot);
  if (usage.kind != HostEntryKind::Regular) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure =
                failure(errorForKind(usage.kind), *normalized.path,
                        "skin data overlay contains a non-regular entry")};
  }
  const HostStatResult existing =
      statAtRoot(*impl_->overlayRoot, *normalized.path);
  if (existing.kind != HostEntryKind::Missing &&
      existing.kind != HostEntryKind::Regular) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(errorForKind(existing.kind), *normalized.path,
                               messageForKind(existing.kind))};
  }

  std::vector<std::byte> replacement;
  if (append) {
    HostReadResult prior = impl_->readLogicalData(
        *normalized.path, impl_->dataPolicy.maximumBytes);
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
  if (bytes.size() >
      std::numeric_limits<std::size_t>::max() - replacement.size()) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::QuotaExceeded, *normalized.path,
                               "skin data write exceeds its byte quota")};
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

  const std::uint64_t priorSize =
      existing.kind == HostEntryKind::Regular ? existing.size : 0;
  const std::uint64_t resultingFiles =
      usage.files + (existing.kind == HostEntryKind::Missing ? 1U : 0U);
  if (usage.bytes < priorSize ||
      replacement.size() > std::numeric_limits<std::uint64_t>::max() -
                               (usage.bytes - priorSize)) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::QuotaExceeded, *normalized.path,
                               "skin data write exceeds its byte quota")};
  }
  const std::uint64_t resultingBytes =
      usage.bytes - priorSize + replacement.size();
  if (resultingBytes > impl_->dataPolicy.maximumBytes ||
      resultingFiles > impl_->dataPolicy.maximumFiles) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::QuotaExceeded, *normalized.path,
                               "skin data overlay quota is exhausted")};
  }

  const fs::path destination =
      *impl_->overlayRoot / pathFromUtf8(*normalized.path);
  if (!ensureAbsoluteDirectoryNoFollow(destination.parent_path())) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::NonRegular, *normalized.path,
                               "skin data parent is not a safe directory")};
  }
  const HostStatResult rechecked =
      statAtRoot(*impl_->overlayRoot, *normalized.path);
  if (rechecked.kind != existing.kind ||
      (rechecked.kind == HostEntryKind::Regular &&
       rechecked.size != existing.size)) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin data target changed during replacement")};
  }
  const std::string temporaryVirtualPath = *normalized.path + ".tmp";
  if (statAtRoot(*impl_->overlayRoot, temporaryVirtualPath).kind !=
      HostEntryKind::Missing) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin data temporary name is unavailable")};
  }

  std::string ignoredHostError;
  const atomic_file::Operations privateOperations =
      atomic_file::privateFileOperations();
  if (!atomic_file::writeWithoutBackup(destination, replacement,
                                       ignoredHostError, &privateOperations)) {
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
  OverlayUsage usage = collectUsage(*impl_->overlayRoot);
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
    const HostStatResult overlayParent =
        statAtRoot(*impl_->overlayRoot, parentVirtual);
    if (overlayParent.kind != HostEntryKind::Directory &&
        packageParent.kind != HostEntryKind::Directory) {
      return {.resultingBytes = usage.bytes,
              .resultingFiles = usage.files,
              .failure = failure(SkinFileError::Missing, *normalized.path,
                                 "skin data directory parent is missing")};
    }
  }
  const fs::path destination = *impl_->overlayRoot / relative;
  if (!ensureAbsoluteDirectoryNoFollow(destination.parent_path())) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(SkinFileError::NonRegular, *normalized.path,
                               "skin data parent is not a safe directory")};
  }
  const HostStatResult existing =
      statAtRoot(*impl_->overlayRoot, *normalized.path);
  if (existing.kind == HostEntryKind::Directory) {
    return {.resultingBytes = usage.bytes, .resultingFiles = usage.files};
  }
  if (existing.kind != HostEntryKind::Missing) {
    return {.resultingBytes = usage.bytes,
            .resultingFiles = usage.files,
            .failure = failure(errorForKind(existing.kind), *normalized.path,
                               messageForKind(existing.kind))};
  }
  std::error_code createError;
  if (!fs::create_directory(destination, createError) || createError ||
      statAtRoot(*impl_->overlayRoot, *normalized.path).kind !=
          HostEntryKind::Directory) {
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
