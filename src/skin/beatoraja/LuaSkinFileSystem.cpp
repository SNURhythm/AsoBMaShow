#include "LuaSkinFileSystem.h"

#include "../package/SkinAliasDetector.h"
#include "../package/SkinPathPolicy.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <regex>
#include <string>
#include <utility>
#include <vector>

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
  if (!result.bytes.empty()) {
    input.read(reinterpret_cast<char *>(result.bytes.data()),
               static_cast<std::streamsize>(result.bytes.size()));
  }
  if (!input && !input.eof()) {
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
  mutable std::mutex operationMutex;

  std::optional<SkinFileFailure>
  rejectAliasedPath(const fs::path &root, const fs::path &resolved,
                    std::string_view authored) const {
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

  SkinFileReadResult readNormalized(std::string_view normalized,
                                    SkinFileUse,
                                    std::uint64_t maximumBytes) const {
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
      .aliases = createPlatformSkinAliasDetector(),
  });
  if (!impl->aliases) {
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
  const auto normalized = impl_->normalize(virtualPath);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  if (use == SkinFileUse::LuaEntry &&
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
  const auto normalized = impl_->normalize(virtualPath);
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
  if (const auto rejected = impl_->rejectAliasedPath(
          impl_->packageRoot, impl_->entryPath,
          impl_->entry.packageRelativePath)) {
    return {.failure = rejected};
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
  if (use == SkinFileUse::LuaEntry &&
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

SkinFileListResult LuaSkinFileSystem::list(std::string_view virtualDirectory,
                                           std::string_view luaPattern,
                                           std::size_t maximumEntries) const {
  const std::scoped_lock lock(impl_->operationMutex);
  (void)maximumEntries;
  const auto normalized = impl_->normalize(virtualDirectory, true);
  // SkinFileLuaApiExporter catches resolver/listing errors and returns an
  // empty result.  It does not cap or sort the host directory stream.
  if (!normalized.path) {
    return {.entries = {}};
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
    return {.entries = {}};
  }
  return {.entries = std::move(entries)};
}

SkinFileWriteResult
LuaSkinFileSystem::writeData(std::string_view virtualPath,
                             std::span<const std::byte> bytes, bool append) {
  const std::scoped_lock lock(impl_->operationMutex);
  const auto normalized = impl_->normalize(virtualPath);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }

  const fs::path target = pathFromUtf8(*normalized.path);
  std::error_code error;
  if (!target.parent_path().empty()) {
    fs::create_directories(target.parent_path(), error);
  }
  if (error) {
    return {.failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin file parent directory could not be created")};
  }
  std::ofstream output(target, std::ios::binary |
                                    (append ? std::ios::app : std::ios::trunc));
  if (!output) {
    return {.failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin file could not be opened for writing")};
  }
  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  output.close();
  if (!output) {
    return {.failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin file could not be written")};
  }
  const HostStatResult written = statDirectPath(target);
  if (written.kind != HostEntryKind::Regular) {
    return {.failure = failure(errorForKind(written.kind), *normalized.path,
                               messageForKind(written.kind))};
  }
  return {.resultingBytes = written.size, .resultingFiles = 1};
}

SkinFileWriteResult
LuaSkinFileSystem::mkdirData(std::string_view virtualDirectory) {
  const std::scoped_lock lock(impl_->operationMutex);
  const auto normalized = impl_->normalize(virtualDirectory, true);
  if (!normalized.path) {
    return {.failure = normalized.failure};
  }
  std::error_code error;
  fs::create_directories(pathFromUtf8(*normalized.path), error);
  if (error) {
    return {.failure = failure(SkinFileError::IoError, *normalized.path,
                               "skin directory could not be created")};
  }
  return {.resultingBytes = 0, .resultingFiles = 0};
}

SkinFileRenderTransitionResult LuaSkinFileSystem::enterRenderPhase() {
  // Beatoraja does not freeze Lua file access when gameplay begins.
  return {.ok = true};
}

SkinFileActivityCounters LuaSkinFileSystem::activityCounters() const noexcept {
  return {};
}

} // namespace skin
