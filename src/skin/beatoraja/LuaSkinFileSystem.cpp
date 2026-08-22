#include "LuaSkinFileSystem.h"

#include "../package/SkinAliasDetector.h"
#include "../package/SkinPathPolicy.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace skin {

namespace lua_skin_file_system_detail {

bool readExact(std::istream &input, std::span<std::byte> destination) {
  if (destination.empty()) {
    return true;
  }
  input.read(reinterpret_cast<char *>(destination.data()),
             static_cast<std::streamsize>(destination.size()));
  return input.gcount() == static_cast<std::streamsize>(destination.size()) &&
         !input.bad();
}

} // namespace lua_skin_file_system_detail

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

struct HostStatResult {
  HostEntryKind kind = HostEntryKind::IoError;
  std::uint64_t size = 0;
};

struct HostReadResult {
  HostEntryKind kind = HostEntryKind::IoError;
  std::vector<std::byte> bytes;
  bool limitExceeded = false;
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

std::string utf8Path(const fs::path &path) {
  const std::u8string value = path.generic_u8string();
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}

bool isWithinDirectory(const fs::path &path, const fs::path &directory) {
  const fs::path normalizedPath = path.lexically_normal();
  const fs::path normalizedDirectory = directory.lexically_normal();
  auto pathComponent = normalizedPath.begin();
  const auto pathEnd = normalizedPath.end();
  for (auto directoryComponent = normalizedDirectory.begin();
       directoryComponent != normalizedDirectory.end(); ++directoryComponent,
       ++pathComponent) {
    if (pathComponent == pathEnd || *pathComponent != *directoryComponent) {
      return false;
    }
  }
  return true;
}

// This mirrors SkinLuaPathResolver.  Beatoraja's normal Lua runtime retains
// this one boundary: io and skin_config may address only the selected skin
// file's directory.  It deliberately follows normal host filesystem aliases.
NormalizedReference normalizeAtSkinDirectory(const fs::path &skinDirectory,
                                             std::string_view authored,
                                             bool allowDirectory = false) {
  if (authored.find('\0') != std::string_view::npos) {
    return {.failure = failure(SkinFileError::InvalidPath, authored,
                               "skin file path is invalid")};
  }
  if (authored.empty() && !allowDirectory) {
    return {.failure = failure(SkinFileError::InvalidPath, authored,
                               "skin file path does not name a file")};
  }

  const fs::path root = skinDirectory.lexically_normal();
  const fs::path authoredPath = pathFromUtf8(authored);
  fs::path resolved;
  if (authoredPath.is_absolute()) {
    resolved = authoredPath.lexically_normal();
  } else {
    std::error_code error;
    const fs::path workingDirectoryPath =
        fs::absolute(authoredPath, error).lexically_normal();
    if (error) {
      return {.failure = failure(SkinFileError::IoError, authored,
                                 "skin file path could not be resolved")};
    }
    resolved = isWithinDirectory(workingDirectoryPath, root)
                   ? workingDirectoryPath
                   : (root / authoredPath).lexically_normal();
  }
  if (!isWithinDirectory(resolved, root)) {
    return {.failure = failure(SkinFileError::EscapesPackage, authored,
                               "skin file access is outside the skin directory")};
  }
  return {.path = utf8Path(resolved)};
}

// SkinLoader and JsonSkinObjectLoader resolve skin resources with ordinary
// File/Path operations relative to the entry's parent.  They do not impose
// the Lua io root on resource declarations.
fs::path resolveResourcePath(const fs::path &skinDirectory,
                             std::string_view authored) {
  const fs::path authoredPath = pathFromUtf8(authored);
  return (authoredPath.is_absolute() ? authoredPath
                                     : skinDirectory / authoredPath)
      .lexically_normal();
}

HostStatResult statDirectPath(const fs::path &path) {
  std::error_code error;
  const fs::file_status status = fs::status(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      status.type() == fs::file_type::not_found) {
    return {.kind = HostEntryKind::Missing};
  }
  if (error) {
    return {.kind = HostEntryKind::IoError};
  }
  if (fs::is_directory(status)) {
    return {.kind = HostEntryKind::Directory};
  }
  if (!fs::is_regular_file(status)) {
    return {.kind = HostEntryKind::NonRegular};
  }
  const std::uintmax_t size = fs::file_size(path, error);
  if (error || size > std::numeric_limits<std::uint64_t>::max()) {
    return {.kind = HostEntryKind::IoError};
  }
  return {.kind = HostEntryKind::Regular,
          .size = static_cast<std::uint64_t>(size)};
}

struct LuaFileOpenSpec {
  bool writable = false;
  bool create = false;
  bool truncate = false;
  bool append = false;
  const char *stdioMode = "rb";
};

LuaFileOpenSpec luaFileOpenSpec(LuaSkinFileOpenMode mode) {
  switch (mode) {
  case LuaSkinFileOpenMode::Read:
    return {.stdioMode = "rb"};
  case LuaSkinFileOpenMode::ReadUpdate:
    return {.writable = true, .stdioMode = "r+b"};
  case LuaSkinFileOpenMode::Write:
  case LuaSkinFileOpenMode::WriteUpdate:
    return {.writable = true,
            .create = true,
            .truncate = true,
            .stdioMode = "w+b"};
  case LuaSkinFileOpenMode::Append:
  case LuaSkinFileOpenMode::AppendUpdate:
    return {.writable = true,
            .create = true,
            .append = true,
            .stdioMode = "r+b"};
  }
  return {};
}

#if !defined(_WIN32)

class UniqueFd {
public:
  explicit UniqueFd(int value = -1) noexcept : value_(value) {}
  ~UniqueFd() {
    if (value_ >= 0) {
      ::close(value_);
    }
  }
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  UniqueFd(UniqueFd &&other) noexcept : value_(std::exchange(other.value_, -1)) {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept { return value_ >= 0; }
  [[nodiscard]] int release() noexcept { return std::exchange(value_, -1); }
  void reset(int value = -1) noexcept {
    if (value_ >= 0) {
      ::close(value_);
    }
    value_ = value;
  }

private:
  int value_ = -1;
};

bool sameFileIdentity(const struct stat &expected,
                      const struct stat &opened) noexcept {
  return expected.st_dev == opened.st_dev && expected.st_ino == opened.st_ino;
}

bool regularSingleLink(const struct stat &status) noexcept {
  return S_ISREG(status.st_mode) && status.st_nlink == 1;
}

std::FILE *adoptDescriptorAsStream(int descriptor, const char *mode) {
  std::FILE *stream = ::fdopen(descriptor, mode);
  if (stream == nullptr) {
    ::close(descriptor);
  }
  return stream;
}

std::optional<UniqueFd> openVerifiedDirectory(const fs::path &directory) {
  struct stat expected {};
  if (::lstat(directory.c_str(), &expected) != 0 || !S_ISDIR(expected.st_mode)) {
    return std::nullopt;
  }
  UniqueFd opened(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                              skinOpenNoFollowFlag()));
  struct stat actual {};
  if (!opened || ::fstat(opened.get(), &actual) != 0 ||
      !S_ISDIR(actual.st_mode) || !sameFileIdentity(expected, actual)) {
    return std::nullopt;
  }
  return opened;
}

std::optional<UniqueFd>
openVerifiedChildDirectory(int parent, const fs::path &component,
                           bool createMissing) {
  const std::string name = component.string();
  if (name.empty() || name == "." || name == "..") {
    return std::nullopt;
  }
  struct stat expected {};
  if (::fstatat(parent, name.c_str(), &expected, AT_SYMLINK_NOFOLLOW) != 0) {
    if (!createMissing || errno != ENOENT ||
        ::mkdirat(parent, name.c_str(), 0700) != 0 ||
        ::fstatat(parent, name.c_str(), &expected, AT_SYMLINK_NOFOLLOW) != 0) {
      return std::nullopt;
    }
  }
  if (!S_ISDIR(expected.st_mode)) {
    return std::nullopt;
  }
  UniqueFd opened(::openat(parent, name.c_str(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                               skinOpenNoFollowFlag()));
  struct stat actual {};
  if (!opened || ::fstat(opened.get(), &actual) != 0 || !S_ISDIR(actual.st_mode) ||
      !sameFileIdentity(expected, actual)) {
    return std::nullopt;
  }
  return opened;
}

std::optional<UniqueFd>
openVerifiedParentDirectory(const fs::path &root, const fs::path &target,
                            bool createMissing, fs::path &leaf) {
  const fs::path relative = target.lexically_relative(root);
  if (relative.empty() || relative == "." || relative.is_absolute()) {
    return std::nullopt;
  }
  std::vector<fs::path> components;
  for (const fs::path &component : relative) {
    if (component.empty() || component == "." || component == "..") {
      return std::nullopt;
    }
    components.push_back(component);
  }
  if (components.empty()) {
    return std::nullopt;
  }
  leaf = components.back();
  components.pop_back();

  auto current = openVerifiedDirectory(root);
  if (!current) {
    return std::nullopt;
  }
  for (const fs::path &component : components) {
    auto next = openVerifiedChildDirectory(current->get(), component,
                                           createMissing);
    if (!next) {
      return std::nullopt;
    }
    current = std::move(next);
  }
  return current;
}

std::FILE *openVerifiedRegularFile(const fs::path &root, const fs::path &target,
                                   const LuaFileOpenSpec &spec) {
  fs::path leaf;
  auto parent = openVerifiedParentDirectory(root, target, spec.create, leaf);
  if (!parent) {
    return nullptr;
  }
  const std::string name = leaf.string();
  struct stat expected {};
  if (::fstatat(parent->get(), name.c_str(), &expected, AT_SYMLINK_NOFOLLOW) !=
      0) {
    if (!spec.create || errno != ENOENT) {
      return nullptr;
    }
    const int createdDescriptor =
        ::openat(parent->get(), name.c_str(), O_RDWR | O_CREAT | O_EXCL |
                                             O_CLOEXEC,
                 0600);
    if (createdDescriptor < 0 || ::fstat(createdDescriptor, &expected) != 0 ||
        !regularSingleLink(expected)) {
      if (createdDescriptor >= 0) {
        ::close(createdDescriptor);
      }
      return nullptr;
    }
    if (spec.truncate && ::ftruncate(createdDescriptor, 0) != 0) {
      ::close(createdDescriptor);
      return nullptr;
    }
    if (spec.append && ::lseek(createdDescriptor, 0, SEEK_END) < 0) {
      ::close(createdDescriptor);
      return nullptr;
    }
    return adoptDescriptorAsStream(createdDescriptor, spec.stdioMode);
  }
  if (!regularSingleLink(expected)) {
    return nullptr;
  }
  const int openFlags = (spec.writable ? O_RDWR : O_RDONLY) | O_CLOEXEC |
                        skinOpenNoFollowFlag();
  const int descriptor = ::openat(parent->get(), name.c_str(), openFlags);
  struct stat actual {};
  if (descriptor < 0 || ::fstat(descriptor, &actual) != 0 ||
      !regularSingleLink(actual) || !sameFileIdentity(expected, actual)) {
    if (descriptor >= 0) {
      ::close(descriptor);
    }
    return nullptr;
  }
  if (spec.truncate && ::ftruncate(descriptor, 0) != 0) {
    ::close(descriptor);
    return nullptr;
  }
  if (spec.append && ::lseek(descriptor, 0, SEEK_END) < 0) {
    ::close(descriptor);
    return nullptr;
  }
  return adoptDescriptorAsStream(descriptor, spec.stdioMode);
}

#endif

std::FILE *openDirectRegularFile(const fs::path &path,
                                 const LuaFileOpenSpec &spec) {
#if defined(_WIN32)
  std::wstring mode;
  for (const char *character = spec.stdioMode; *character != '\0'; ++character) {
    mode.push_back(static_cast<wchar_t>(*character));
  }
  std::FILE *stream = _wfopen(path.c_str(), mode.c_str());
  if (stream == nullptr && spec.append && spec.create) {
    stream = _wfopen(path.c_str(), L"w+b");
  }
#else
  std::FILE *stream = std::fopen(path.c_str(), spec.stdioMode);
  if (stream == nullptr && spec.append && spec.create) {
    stream = std::fopen(path.c_str(), "w+b");
  }
#endif
  if (stream != nullptr && spec.append && std::fseek(stream, 0, SEEK_END) != 0) {
    std::fclose(stream);
    return nullptr;
  }
  return stream;
}

HostReadResult readDirectPath(const fs::path &path,
                              std::uint64_t maximumBytes) {
  const HostStatResult status = statDirectPath(path);
  if (status.kind != HostEntryKind::Regular) {
    return {.kind = status.kind};
  }
  // maximumBytes is supplied by the caller for operations that have a real
  // representation limit.  The normal Beatoraja Lua path passes UINT64_MAX.
  if (status.size > maximumBytes ||
      status.size > static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max())) {
    return {.kind = HostEntryKind::LimitExceeded, .limitExceeded = true};
  }

  HostReadResult result;
  result.kind = HostEntryKind::Regular;
  try {
    result.bytes.resize(static_cast<std::size_t>(status.size));
  } catch (...) {
    result.kind = HostEntryKind::LimitExceeded;
    result.limitExceeded = true;
    return result;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {.kind = HostEntryKind::IoError};
  }
  if (!lua_skin_file_system_detail::readExact(input, result.bytes)) {
    return {.kind = HostEntryKind::IoError};
  }
  return result;
}

SkinFileError errorForKind(HostEntryKind kind) {
  switch (kind) {
  case HostEntryKind::Missing:
    return SkinFileError::Missing;
  case HostEntryKind::NonRegular:
  case HostEntryKind::Directory:
    return SkinFileError::NonRegular;
  case HostEntryKind::LimitExceeded:
    return SkinFileError::LimitExceeded;
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
    return "skin virtual file could not fit in memory";
  case HostEntryKind::IoError:
    return "skin virtual file operation failed";
  case HostEntryKind::Regular:
    break;
  }
  return "skin virtual file operation failed";
}

// SkinFileLuaApiExporter translates Lua %-escapes to Java regex escapes and
// otherwise hands the expression to Pattern.  Do the same rather than keep
// the previous bounded custom subset.
std::optional<std::regex> compileLuaFileListPattern(std::string_view pattern) {
  std::string expression;
  expression.reserve(pattern.size() + 1);
  bool escaped = false;
  for (const char value : pattern) {
    if (escaped) {
      expression.push_back('\\');
      expression.push_back(value);
      escaped = false;
    } else if (value == '%') {
      escaped = true;
    } else {
      expression.push_back(value);
    }
  }
  if (escaped) {
    expression.push_back('%');
  }
  try {
    return std::regex(expression);
  } catch (const std::regex_error &) {
    return std::nullopt;
  }
}

} // namespace

struct LuaSkinFileSystem::Impl {
  SkinRevisionReadView revision;
  SkinEntryId entry;
  fs::path packageRoot;
  fs::path skinDirectory;
  fs::path entryPath;
  fs::path beatorajaSkinRoot;
  std::unique_ptr<SkinAliasDetector> aliases;
  bool allowDataWrites = false;
  SkinSafetyPolicy safetyPolicy;
  mutable std::mutex operationMutex;

  std::optional<SkinFileFailure>
  rejectAliasedPath(const fs::path &root, const fs::path &resolved,
                    std::string_view authored) const {
    if (!safetyPolicy.enforces(SkinSafetyGuard::VirtualFileContainment)) {
      return std::nullopt;
    }
    if (!isWithinDirectory(resolved, root)) {
      return failure(SkinFileError::EscapesPackage, authored,
                     "skin file access is outside the skin directory");
    }

    fs::path candidate = root.lexically_normal();
    const fs::path relative = resolved.lexically_relative(candidate);
    const auto inspect = [&](const fs::path &path)
        -> std::optional<SkinFileFailure> {
      std::error_code error;
      const fs::file_status status = fs::symlink_status(path, error);
      if (error == std::errc::no_such_file_or_directory ||
          status.type() == fs::file_type::not_found) {
        // A missing suffix is valid for the Beatoraja Lua write helpers.
        return std::nullopt;
      }
      if (error) {
        return failure(SkinFileError::IoError, authored,
                       "skin file path could not be inspected");
      }
      if (!aliases || aliases->inspectNoFollow(path) !=
                          SkinRejectedLinkKind::None) {
        return failure(SkinFileError::EscapesPackage, authored,
                       "skin file path contains a linked entry");
      }
      return std::nullopt;
    };

    if (const auto rejected = inspect(candidate)) {
      return rejected;
    }
    for (const fs::path &component : relative) {
      // A trailing slash creates an empty final component.  It does not name
      // a filesystem entry and is already accepted by the Beatoraja path
      // helpers for directory operations.
      if (component.empty()) {
        continue;
      }
      if (component == "." || component == "..") {
        return failure(SkinFileError::EscapesPackage, authored,
                       "skin file path is outside the skin directory");
      }
      candidate /= component;
      if (const auto rejected = inspect(candidate)) {
        return rejected;
      }
    }
    return std::nullopt;
  }

  NormalizedReference checkedReference(const fs::path &root,
                                       const fs::path &resolved,
                                       std::string_view authored) const {
    if (const auto rejected = rejectAliasedPath(root, resolved, authored)) {
      return {.failure = rejected};
    }
    return {.path = utf8Path(resolved)};
  }

  NormalizedReference normalize(std::string_view authored,
                                bool allowPackageRoot = false) const {
    if (!safetyPolicy.enforces(SkinSafetyGuard::VirtualFileContainment)) {
      if (authored.empty() || authored.find('\0') != std::string_view::npos) {
        return {.failure = failure(SkinFileError::InvalidPath, authored,
                                   "skin file path is invalid")};
      }
      const fs::path authoredPath = pathFromUtf8(authored);
      const fs::path resolved =
          (authoredPath.is_absolute() ? authoredPath : skinDirectory / authoredPath)
              .lexically_normal();
      return {.path = utf8Path(resolved)};
    }
    // SkinLoader#getPath returns paths rooted at Beatoraja's `skin/`
    // directory.  A configured skin can subsequently pass that exact result
    // to dofile/io, including a sibling reached through `..` from its entry
    // directory.  Interpret only this explicit virtual form against the
    // package root; ordinary relative Lua paths retain SkinLuaPathResolver's
    // selected-entry-directory boundary below.
    constexpr std::string_view beatorajaRootPrefix = "skin/";
    if (authored.starts_with(beatorajaRootPrefix)) {
      const std::string packagePrefix =
          std::string(beatorajaRootPrefix) + entry.package.directoryName;
      const bool currentPackage =
          authored == packagePrefix ||
          (authored.size() > packagePrefix.size() &&
           authored.starts_with(packagePrefix) &&
           authored[packagePrefix.size()] == '/');
      std::string_view suffix =
          currentPackage ? authored.substr(packagePrefix.size())
                         : authored.substr(beatorajaRootPrefix.size());
      if (currentPackage && suffix.starts_with('/')) {
        suffix.remove_prefix(1);
      }
      const fs::path resolved =
          ((currentPackage ? packageRoot : beatorajaSkinRoot) /
           pathFromUtf8(suffix))
              .lexically_normal();
      if (!isWithinDirectory(resolved, beatorajaSkinRoot)) {
        return {.failure = failure(
                    SkinFileError::EscapesPackage, authored,
                    "skin file access is outside Beatoraja's skin directory")};
      }
      return checkedReference(currentPackage ? packageRoot : beatorajaSkinRoot,
                              resolved, authored);
    }
    const auto normalized =
        normalizeAtSkinDirectory(skinDirectory, authored, allowPackageRoot);
    if (!normalized.path) {
      return normalized;
    }
    return checkedReference(packageRoot, pathFromUtf8(*normalized.path),
                            authored);
  }

  NormalizedReference normalizeDataWrite(std::string_view authored,
                                         bool allowPackageRoot = false) const {
    auto normalized = normalize(authored, allowPackageRoot);
    if (!normalized.path) {
      return normalized;
    }
    if (!safetyPolicy.enforces(SkinSafetyGuard::VirtualFileContainment)) {
      return normalized;
    }
    if (!isWithinDirectory(pathFromUtf8(*normalized.path), packageRoot)) {
      return {.failure = failure(
          SkinFileError::EscapesPackage, authored,
          "Lua data writes must remain inside the selected skin package")};
    }
    return normalized;
  }

  std::FILE *openNormalized(std::string_view normalized,
                            LuaSkinFileOpenMode mode) const {
    const fs::path target = pathFromUtf8(normalized);
    const LuaFileOpenSpec spec = luaFileOpenSpec(mode);
    if (!safetyPolicy.enforces(SkinSafetyGuard::VirtualFileContainment)) {
      return openDirectRegularFile(target, spec);
    }
#if defined(_WIN32)
    return openDirectRegularFile(target, spec);
#else
    const fs::path &root = isWithinDirectory(target, packageRoot)
                               ? packageRoot
                               : beatorajaSkinRoot;
    return openVerifiedRegularFile(root, target, spec);
#endif
  }

  SkinFileReadResult readOpenedStream(std::FILE *stream,
                                      std::string_view normalized,
                                      std::uint64_t maximumBytes) const {
    if (std::fseek(stream, 0, SEEK_END) != 0) {
      return {.failure = failure(SkinFileError::IoError, normalized,
                                 "skin virtual file operation failed")};
    }
    const long end = std::ftell(stream);
    if (end < 0 || std::fseek(stream, 0, SEEK_SET) != 0) {
      return {.failure = failure(SkinFileError::IoError, normalized,
                                 "skin virtual file operation failed")};
    }
    const std::uint64_t byteCount = static_cast<std::uint64_t>(end);
    if (byteCount > maximumBytes ||
        byteCount > static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max())) {
      return {.failure = failure(SkinFileError::LimitExceeded, normalized,
                                 "skin virtual file could not fit in memory")};
    }
    std::vector<std::byte> bytes;
    try {
      bytes.resize(static_cast<std::size_t>(byteCount));
    } catch (...) {
      return {.failure = failure(SkinFileError::LimitExceeded, normalized,
                                 "skin virtual file could not fit in memory")};
    }
    if (!bytes.empty() &&
        std::fread(bytes.data(), 1, bytes.size(), stream) != bytes.size()) {
      return {.failure = failure(SkinFileError::IoError, normalized,
                                 "skin virtual file operation failed")};
    }
    return {.bytes = std::move(bytes)};
  }

  SkinFileReadResult readNormalized(std::string_view normalized,
                                    SkinFileUse use,
                                    std::uint64_t maximumBytes) const {
    if (safetyPolicy.enforces(SkinSafetyGuard::VirtualFileContainment) &&
        use != SkinFileUse::Resource) {
      std::FILE *stream = openNormalized(normalized, LuaSkinFileOpenMode::Read);
      if (stream == nullptr) {
        const HostStatResult status = statDirectPath(pathFromUtf8(normalized));
        return {.failure = failure(errorForKind(status.kind), normalized,
                                   messageForKind(status.kind))};
      }
      const SkinFileReadResult result =
          readOpenedStream(stream, normalized, maximumBytes);
      std::fclose(stream);
      return result;
    }
    HostReadResult host =
        readDirectPath(pathFromUtf8(normalized), maximumBytes);
    if (host.limitExceeded) {
      return {.failure = failure(SkinFileError::LimitExceeded, normalized,
                                 "skin virtual file could not fit in memory")};
    }
    if (host.kind != HostEntryKind::Regular) {
      return {.failure = failure(errorForKind(host.kind), normalized,
                                 messageForKind(host.kind))};
    }
    return {.bytes = std::move(host.bytes)};
  }

  std::vector<std::string>
  moduleCandidates(std::string_view moduleName,
                   std::optional<SkinFileFailure> &moduleFailure) const {
    if (moduleName.find('\0') != std::string_view::npos) {
      moduleFailure = failure(SkinFileError::InvalidPath, moduleName,
                              "Lua module name is invalid");
      return {};
    }
    std::string substitution(moduleName);
    if (substitution.find('/') == std::string::npos) {
      std::ranges::replace(substitution, '.', '/');
    }
    std::vector<std::string> candidates;
    for (const std::string_view suffix : {std::string_view(".lua"),
                                          std::string_view("/init.lua")}) {
      const auto normalized = normalize(substitution + std::string(suffix));
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
      statDirectPath(revisionRoot).kind != HostEntryKind::Directory) {
    return {.failure = failure(SkinFileError::IoError,
                               options.entry.packageRelativePath,
                               "skin revision root is unavailable")};
  }

  // A catalog revision identifies the current package, but normal Beatoraja
  // execution reads the ordinary on-disk skin directory.  Prefer the
  // Files-visible package whenever it is present.
  fs::path packageRoot = revisionRoot;
  // A private revision is laid out as `<revision digest>/<package>`; its
  // parent supplies the same virtual `skin/` root when a Files-visible
  // package is not available.
  fs::path beatorajaSkinRoot = revisionRoot.parent_path();
  if (!options.storageRoots.visiblePackages.empty()) {
    const fs::path visiblePackage =
        (options.storageRoots.visiblePackages / options.entry.package.directoryName)
            .lexically_normal();
    if (statDirectPath(visiblePackage).kind == HostEntryKind::Directory) {
      packageRoot = visiblePackage;
      beatorajaSkinRoot = options.storageRoots.visiblePackages.lexically_normal();
    }
  }
  const fs::path entryPath =
      (packageRoot / pathFromUtf8(options.entry.packageRelativePath))
          .lexically_normal();
  const fs::path skinDirectory = entryPath.parent_path();
  if (skinDirectory.empty() || !skinDirectory.is_absolute()) {
    return {.failure = failure(SkinFileError::IoError,
                               options.entry.packageRelativePath,
                               "skin directory is unavailable")};
  }

  auto impl = std::unique_ptr<Impl>(new Impl{
      .revision = options.revision,
      .entry = std::move(options.entry),
      .packageRoot = packageRoot,
      .skinDirectory = skinDirectory,
      .entryPath = entryPath,
      .beatorajaSkinRoot = beatorajaSkinRoot,
      .aliases = options.safetyPolicy.enforces(
                     SkinSafetyGuard::VirtualFileContainment)
                     ? createPlatformSkinAliasDetector()
                     : nullptr,
      .allowDataWrites = options.allowDataWrites,
      .safetyPolicy = options.safetyPolicy,
  });
  if (impl->safetyPolicy.enforces(SkinSafetyGuard::VirtualFileContainment) &&
      !impl->aliases) {
    return {.failure = failure(SkinFileError::IoError,
                               impl->entry.packageRelativePath,
                               "skin link inspector is unavailable")};
  }
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
  if (use == SkinFileUse::DataWrite && !impl_->allowDataWrites &&
      impl_->safetyPolicy.enforces(SkinSafetyGuard::CatalogWriteAuthorization)) {
    return {.failure = failure(SkinFileError::WrongUse, virtualPath,
                               "Lua data writes are unavailable in this phase")};
  }
  const auto normalized = use == SkinFileUse::DataWrite
                              ? impl_->normalizeDataWrite(virtualPath)
                              : impl_->normalize(virtualPath);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  if (impl_->safetyPolicy.enforces(SkinSafetyGuard::VirtualFileContainment) &&
      use == SkinFileUse::LuaEntry &&
      pathFromUtf8(*normalized.path) != impl_->entryPath) {
    return {.failure = failure(SkinFileError::WrongUse, *normalized.path,
                               "Lua entry access is limited to the selected entry")};
  }
  if (use == SkinFileUse::DataWrite) {
    return {.normalizedVirtualPath = *normalized.path};
  }
  const HostStatResult status = statDirectPath(pathFromUtf8(*normalized.path));
  if (status.kind != HostEntryKind::Regular) {
    return {.failure = failure(errorForKind(status.kind), *normalized.path,
                               messageForKind(status.kind))};
  }
  return {.normalizedVirtualPath = *normalized.path};
}

SkinFileResolveResult LuaSkinFileSystem::normalizeVirtualPath(
    std::string_view virtualPath, bool directoryPath) const {
  const std::scoped_lock lock(impl_->operationMutex);
  while (directoryPath && virtualPath.size() > 1 &&
         virtualPath.ends_with('/')) {
    virtualPath.remove_suffix(1);
  }
  const auto normalized = impl_->normalize(virtualPath, directoryPath);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  return {.normalizedVirtualPath = *normalized.path};
}

SkinFileResolveResult LuaSkinFileSystem::resolveResourceCandidates(
    std::string_view entryRelative,
    std::string_view packageNormalized) const {
  const std::scoped_lock lock(impl_->operationMutex);
  (void)packageNormalized;
  const fs::path resolved = entryRelative.starts_with("skin/")
                                ? (impl_->beatorajaSkinRoot /
                                   pathFromUtf8(entryRelative.substr(5)))
                                      .lexically_normal()
                                : resolveResourcePath(impl_->skinDirectory,
                                                      entryRelative);
  // SkinLoader#getPath returns the ordinary File even when it has not yet
  // been created.  Do not turn resolution into an availability check.
  return {.normalizedVirtualPath = utf8Path(resolved)};
}

SkinFileListResult LuaSkinFileSystem::listResourceDirectory(
    std::string_view entryRelativeDirectory) const {
  const std::scoped_lock lock(impl_->operationMutex);
  const fs::path directory =
      resolveResourcePath(impl_->skinDirectory, entryRelativeDirectory);
  std::error_code error;
  std::vector<std::string> entries;
  for (fs::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    entries.push_back(utf8Path(iterator->path()));
  }
  if (error) {
    return {.failure = failure(SkinFileError::IoError,
                               entryRelativeDirectory,
                               "skin resource directory could not be listed")};
  }
  return {.entries = std::move(entries)};
}

SkinFileReadResult LuaSkinFileSystem::readResolvedResource(
    std::string_view packageNormalized, std::uint64_t maximumBytes) const {
  const std::scoped_lock lock(impl_->operationMutex);
  return impl_->readNormalized(packageNormalized, SkinFileUse::Resource,
                               maximumBytes);
}

const SkinEntryId &LuaSkinFileSystem::entry() const noexcept {
  return impl_->entry;
}

const SkinRevision &LuaSkinFileSystem::revision() const noexcept {
  return impl_->revision.revision();
}

const fs::path &LuaSkinFileSystem::revisionRoot() const noexcept {
  return impl_->revision.root();
}

const fs::path &LuaSkinFileSystem::skinDirectory() const noexcept {
  return impl_->skinDirectory;
}

SkinFileResolveResult
LuaSkinFileSystem::resolveModule(std::string_view moduleName) const {
  const std::scoped_lock lock(impl_->operationMutex);
  std::optional<SkinFileFailure> moduleFailure;
  const std::vector<std::string> candidates =
      impl_->moduleCandidates(moduleName, moduleFailure);
  if (moduleFailure) {
    return {.failure = std::move(moduleFailure)};
  }
  for (const std::string &candidate : candidates) {
    const HostStatResult status = statDirectPath(pathFromUtf8(candidate));
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
  if (impl_->safetyPolicy.enforces(SkinSafetyGuard::VirtualFileContainment)) {
    if (const auto rejected = impl_->rejectAliasedPath(
            impl_->packageRoot, impl_->entryPath,
            impl_->entry.packageRelativePath)) {
      return {.failure = rejected};
    }
  }
  return impl_->readNormalized(utf8Path(impl_->entryPath),
                               SkinFileUse::LuaEntry, maximumBytes);
}

SkinFileReadResult LuaSkinFileSystem::read(std::string_view virtualPath,
                                           SkinFileUse use,
                                           std::uint64_t maximumBytes) const {
  const std::scoped_lock lock(impl_->operationMutex);
  if (use == SkinFileUse::DataWrite) {
    return {.failure = failure(SkinFileError::WrongUse, virtualPath,
                               "write use cannot be read")};
  }
  const auto normalized = impl_->normalize(virtualPath);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  if (impl_->safetyPolicy.enforces(SkinSafetyGuard::VirtualFileContainment) &&
      use == SkinFileUse::LuaEntry &&
      pathFromUtf8(*normalized.path) != impl_->entryPath) {
    return {.failure = failure(SkinFileError::WrongUse, *normalized.path,
                               "Lua entry access is limited to the selected entry")};
  }
  return impl_->readNormalized(*normalized.path, use, maximumBytes);
}

SkinFileReadResult
LuaSkinFileSystem::readLuaPath(std::string_view virtualPath,
                               std::uint64_t maximumBytes) const {
  const std::scoped_lock lock(impl_->operationMutex);
  const auto resolved = impl_->normalize(virtualPath);
  if (!resolved.path) {
    return {.failure = resolved.failure};
  }
  SkinFileReadResult result = impl_->readNormalized(
      *resolved.path, SkinFileUse::LuaModule, maximumBytes);
  if (!(result.failure && result.failure->code == SkinFileError::Missing) ||
      !virtualPath.starts_with("skin/")) {
    return result;
  }

  // Beatoraja's package path commonly contains `skin/...`; on iOS that is
  // the Files-visible Skins root and must remain available for shared Hub
  // modules.
  const fs::path sharedPath =
      (impl_->beatorajaSkinRoot / pathFromUtf8(virtualPath.substr(5)))
          .lexically_normal();
  return impl_->readNormalized(utf8Path(sharedPath), SkinFileUse::LuaModule,
                               maximumBytes);
}

SkinFileReadResult
LuaSkinFileSystem::readModule(std::string_view moduleName,
                              std::uint64_t maximumBytes) const {
  const std::scoped_lock lock(impl_->operationMutex);
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

LuaSkinFileOpenResult
LuaSkinFileSystem::openLuaFile(std::string_view virtualPath,
                               LuaSkinFileOpenMode mode) {
  const std::scoped_lock lock(impl_->operationMutex);
  const bool writable = mode != LuaSkinFileOpenMode::Read;
  if (writable && !impl_->allowDataWrites &&
      impl_->safetyPolicy.enforces(SkinSafetyGuard::CatalogWriteAuthorization)) {
    return {.failure = failure(SkinFileError::WrongUse, virtualPath,
                               "Lua data writes are unavailable in this phase")};
  }
  const auto normalized = writable ? impl_->normalizeDataWrite(virtualPath)
                                   : impl_->normalize(virtualPath);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  const fs::path target = pathFromUtf8(*normalized.path);
#if defined(_WIN32)
  if (writable) {
#else
  if (writable &&
      !impl_->safetyPolicy.enforces(SkinSafetyGuard::VirtualFileContainment)) {
#endif
    std::error_code error;
    if (!target.parent_path().empty()) {
      fs::create_directories(target.parent_path(), error);
    }
    if (error) {
      return {.failure = failure(
          SkinFileError::IoError, *normalized.path,
          "skin file parent directory could not be created")};
    }
  }
  std::FILE *stream = impl_->openNormalized(*normalized.path, mode);
  if (stream == nullptr) {
    const HostStatResult status = statDirectPath(pathFromUtf8(*normalized.path));
    return {.failure = failure(errorForKind(status.kind), *normalized.path,
                               messageForKind(status.kind))};
  }
  return {.file = stream};
}

SkinFileExistsResult
LuaSkinFileSystem::exists(std::string_view virtualPath) const {
  const std::scoped_lock lock(impl_->operationMutex);
  const auto normalized = impl_->normalize(virtualPath, true);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  std::error_code error;
  const bool pathExists = fs::exists(pathFromUtf8(*normalized.path), error);
  return {.exists = !error && pathExists};
}

SkinFileListResult LuaSkinFileSystem::list(std::string_view virtualDirectory,
                                           std::string_view luaPattern,
                                           std::size_t maximumEntries) const {
  const std::scoped_lock lock(impl_->operationMutex);
  (void)maximumEntries;
  const auto normalized = impl_->normalize(virtualDirectory, true);
  // LegacySkinLuaApi#fileFacade returns nil when the path cannot be resolved
  // or Files.newDirectoryStream fails.  It does not cap or sort the host
  // directory stream.
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  std::optional<std::regex> pattern;
  if (!luaPattern.empty()) {
    pattern = compileLuaFileListPattern(luaPattern);
    if (!pattern) {
      return {.entries = {}};
    }
  }

  std::error_code error;
  std::vector<std::string> entries;
  for (fs::directory_iterator iterator(pathFromUtf8(*normalized.path), error),
       end;
       !error && iterator != end; iterator.increment(error)) {
    const std::string path = utf8Path(iterator->path());
    if (!pattern) {
      entries.push_back(path);
      continue;
    }
    std::smatch match;
    if (std::regex_search(path, match, *pattern)) {
      entries.push_back(match.str());
    }
  }
  if (error) {
    return {.failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin directory could not be listed")};
  }
  return {.entries = std::move(entries)};
}

SkinFileWriteResult
LuaSkinFileSystem::writeData(std::string_view virtualPath,
                             std::span<const std::byte> bytes, bool append) {
  const std::scoped_lock lock(impl_->operationMutex);
  if (!impl_->allowDataWrites &&
      impl_->safetyPolicy.enforces(SkinSafetyGuard::CatalogWriteAuthorization)) {
    return {.failure = failure(SkinFileError::WrongUse, virtualPath,
                               "Lua data writes are unavailable in this phase")};
  }
  const auto normalized = impl_->normalizeDataWrite(virtualPath);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }

  const fs::path target = pathFromUtf8(*normalized.path);
#if defined(_WIN32)
  {
#else
  if (!impl_->safetyPolicy.enforces(SkinSafetyGuard::VirtualFileContainment)) {
#endif
    std::error_code error;
    if (!target.parent_path().empty()) {
      fs::create_directories(target.parent_path(), error);
    }
    if (error) {
      return {.failure = failure(SkinFileError::IoError, *normalized.path,
                                 "skin file parent directory could not be created")};
    }
  }
  std::FILE *output = impl_->openNormalized(
      *normalized.path,
      append ? LuaSkinFileOpenMode::Append : LuaSkinFileOpenMode::Write);
  if (output == nullptr) {
    return {.failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin file could not be opened for writing")};
  }
  const bool wrote = bytes.empty() ||
                     std::fwrite(bytes.data(), 1, bytes.size(), output) ==
                         bytes.size();
  const bool flushed = wrote && std::fflush(output) == 0;
  const bool closed = std::fclose(output) == 0;
  if (!flushed || !closed) {
    return {.failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin file could not be written")};
  }
  std::error_code error;
  const std::uintmax_t size = fs::file_size(target, error);
  if (error || size > std::numeric_limits<std::uint64_t>::max()) {
    return {.failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin file could not be inspected after writing")};
  }
  return {.resultingBytes = static_cast<std::uint64_t>(size),
          .resultingFiles = 1};
}

SkinFileWriteResult
LuaSkinFileSystem::mkdirData(std::string_view virtualDirectory,
                             bool recursive) {
  const std::scoped_lock lock(impl_->operationMutex);
  if (!impl_->allowDataWrites &&
      impl_->safetyPolicy.enforces(SkinSafetyGuard::CatalogWriteAuthorization)) {
    return {.failure =
                failure(SkinFileError::WrongUse, virtualDirectory,
                        "Lua data writes are unavailable in this phase")};
  }
  const auto normalized = impl_->normalizeDataWrite(virtualDirectory, true);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  const fs::path target = pathFromUtf8(*normalized.path);
#if defined(_WIN32)
  {
#else
  if (!impl_->safetyPolicy.enforces(SkinSafetyGuard::VirtualFileContainment)) {
#endif
    std::error_code error;
    const bool created = recursive ? fs::create_directories(target, error)
                                   : fs::create_directory(target, error);
    if (!error && (recursive || created)) {
      return {.resultingBytes = 0, .resultingFiles = 0};
    }
  }
#if !defined(_WIN32)
  else {
    fs::path leaf;
    auto parent = openVerifiedParentDirectory(impl_->packageRoot, target,
                                              recursive, leaf);
    if (parent && recursive) {
      auto directory = openVerifiedChildDirectory(parent->get(), leaf, true);
      if (directory) {
        return {.resultingBytes = 0, .resultingFiles = 0};
      }
    } else if (parent) {
      const std::string name = leaf.string();
      if (::mkdirat(parent->get(), name.c_str(), 0700) == 0) {
        auto directory =
            openVerifiedChildDirectory(parent->get(), leaf, false);
        if (directory) {
          return {.resultingBytes = 0, .resultingFiles = 0};
        }
        ::unlinkat(parent->get(), name.c_str(), AT_REMOVEDIR);
      }
    }
  }
#endif
  {
    return {.failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin directory could not be created")};
  }
}

SkinFileRenderTransitionResult LuaSkinFileSystem::enterRenderPhase() {
  // Beatoraja does not freeze Lua file access when gameplay begins.
  return {.ok = true};
}

SkinFileActivityCounters LuaSkinFileSystem::activityCounters() const noexcept {
  return {};
}

} // namespace skin
