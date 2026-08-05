#pragma once

#include "../SkinProfileSettings.h"
#include "../SkinStoragePaths.h"
#include "../package/SkinTreeSnapshotter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skin {

enum class SkinFileUse : std::uint8_t {
  LuaEntry,
  LuaModule,
  Resource,
  DataRead,
  DataWrite,
};

enum class SkinFileError : std::uint8_t {
  InvalidPath,
  EscapesPackage,
  WrongUse,
  Missing,
  NonRegular,
  BinaryChunk,
  LimitExceeded,
  QuotaExceeded,
  RenderPhase,
  IoError,
};

struct SkinFileFailure {
  SkinFileError code = SkinFileError::IoError;
  std::string virtualPath;
  std::string message;
};

struct SkinFileResolveResult {
  std::optional<std::string> normalizedVirtualPath;
  std::optional<SkinFileFailure> failure;
};

struct SkinFileReadResult {
  std::vector<std::byte> bytes;
  std::optional<SkinFileFailure> failure;
};

struct SkinFileListResult {
  std::vector<std::string> entries;
  std::optional<SkinFileFailure> failure;
};

struct SkinFileWriteResult {
  std::uint64_t resultingBytes = 0;
  std::uint64_t resultingFiles = 0;
  std::optional<SkinFileFailure> failure;
};

struct SkinFileActivityCounters {
  std::uint64_t renderReadsPerformed = 0;
  std::uint64_t renderReadsDenied = 0;
  std::uint64_t renderWritesPerformed = 0;
  std::uint64_t renderWritesDenied = 0;
  std::uint64_t renderDirectoryScansPerformed = 0;
  std::uint64_t renderDirectoryScansDenied = 0;
};

struct SkinFileRenderTransitionResult {
  bool ok = false;
  std::optional<SkinFileFailure> failure;
};

struct SkinDataOverlayPolicy {
  static constexpr std::uint64_t maximumPolicyBytes = 16ULL * 1024 * 1024;
  static constexpr std::uint64_t maximumPolicyFiles = 1'024;

  std::uint64_t maximumBytes = maximumPolicyBytes;
  std::uint64_t maximumFiles = maximumPolicyFiles;
};

struct LuaSkinFileSystemOptions {
  SkinRevisionReadView revision;
  SkinEntryId entry;
  SkinStorageRoots storageRoots;
  std::optional<SkinProfileId> profileId;
  bool allowDataWrites = false;
  SkinDataOverlayPolicy dataPolicy;
};

class LuaSkinFileSystem;

struct LuaSkinFileSystemCreateResult {
  std::unique_ptr<LuaSkinFileSystem> fileSystem;
  std::optional<SkinFileFailure> failure;
};

class LuaSkinFileSystem final {
public:
  static LuaSkinFileSystemCreateResult create(LuaSkinFileSystemOptions);

  LuaSkinFileSystem(LuaSkinFileSystem &&) noexcept;
  LuaSkinFileSystem &operator=(LuaSkinFileSystem &&) noexcept;
  LuaSkinFileSystem(const LuaSkinFileSystem &) = delete;
  LuaSkinFileSystem &operator=(const LuaSkinFileSystem &) = delete;
  ~LuaSkinFileSystem();

  SkinFileResolveResult resolve(std::string_view virtualPath,
                                SkinFileUse) const;
  // Normalizes an authored path inside the selected package without requiring
  // that a package file already exists. This is for legacy skin APIs that
  // create their own persistent data through skin_config.get_path.
  SkinFileResolveResult normalizeVirtualPath(std::string_view virtualPath,
                                             bool directoryPath = false) const;
  // Resource preparation may receive either an authored path relative to the
  // selected entry or an already package-normalized path returned by
  // skin_config.get_path. Resolve both through the no-follow package reader;
  // choosing between two different existing files is forbidden.
  SkinFileResolveResult resolveResourceCandidates(
      std::string_view entryRelative,
      std::string_view packageNormalized) const;
  SkinFileReadResult readResolvedResource(std::string_view packageNormalized,
                                          std::uint64_t maximumBytes) const;
  [[nodiscard]] const SkinEntryId &entry() const noexcept;
  [[nodiscard]] const SkinRevision &revision() const noexcept;
  [[nodiscard]] const std::filesystem::path &revisionRoot() const noexcept;
  SkinFileResolveResult resolveModule(std::string_view moduleName) const;
  SkinFileReadResult readEntry(std::uint64_t maximumBytes) const;
  SkinFileReadResult read(std::string_view virtualPath, SkinFileUse,
                          std::uint64_t maximumBytes) const;
  // Lua package-path and dofile references first use the entry directory,
  // then the package root. A legacy `skin/` prefix also names that root.
  SkinFileReadResult readLuaPath(std::string_view virtualPath,
                                 std::uint64_t maximumBytes) const;
  SkinFileReadResult readModule(std::string_view moduleName,
                                std::uint64_t maximumBytes) const;
  SkinFileListResult list(std::string_view virtualDirectory,
                          std::string_view luaPattern,
                          std::size_t maximumEntries) const;
  SkinFileWriteResult writeData(std::string_view virtualPath,
                                std::span<const std::byte>, bool append);
  SkinFileWriteResult mkdirData(std::string_view virtualDirectory);
  SkinFileRenderTransitionResult enterRenderPhase();
  SkinFileActivityCounters activityCounters() const noexcept;

private:
  struct Impl;
  explicit LuaSkinFileSystem(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace skin
