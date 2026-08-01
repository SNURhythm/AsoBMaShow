#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace atomic_file {
struct Operations {
  std::function<bool(const std::filesystem::path &, std::span<const std::byte>,
                     std::string &)>
      writeAndSync;
  std::function<bool(const std::filesystem::path &,
                     const std::filesystem::path &, std::string &)>
      replace;
  std::function<void(const std::filesystem::path &)> remove;
};

Operations defaultOperations();

// Uses owner-only POSIX permissions for newly written files. Windows keeps
// the platform's inherited per-user ACL behavior.
Operations privateFileOperations();

// Tightens an existing regular file to owner read/write on POSIX. Missing
// files are accepted so callers can use this before first creation.
bool restrictToOwnerOnly(const std::filesystem::path &path,
                         std::string &errorMessage);

// Flushes an existing file's data to stable storage. This is useful when a
// file was produced by a subsystem that does not provide its own durability
// guarantee (for example SQLite or a legacy atomic writer).
bool syncFile(const std::filesystem::path &path, std::string &errorMessage);

// Persists directory-entry metadata on POSIX. Windows directory handles do
// not provide a portable fsync equivalent, so callers pair this with
// renameDurably(), whose MoveFileExW operation is write-through there.
bool syncDirectory(const std::filesystem::path &path,
                   std::string &errorMessage);

// Renames within a filesystem. Windows uses MOVEFILE_WRITE_THROUGH; POSIX
// callers must sync the destination parent directory after this succeeds.
bool renameDurably(const std::filesystem::path &from,
                   const std::filesystem::path &to, std::string &errorMessage);

bool writeWithBackup(const std::filesystem::path &path,
                     std::span<const std::byte> contents,
                     std::string &errorMessage,
                     const Operations *operations = nullptr);

bool removeBackupArtifacts(const std::filesystem::path &path,
                           std::string &errorMessage,
                           const Operations *operations = nullptr);

bool writeWithoutBackup(const std::filesystem::path &path,
                        std::span<const std::byte> contents,
                        std::string &errorMessage,
                        const Operations *operations = nullptr);
} // namespace atomic_file
