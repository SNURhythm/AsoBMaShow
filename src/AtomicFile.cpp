#include "AtomicFile.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace atomic_file {
namespace {
bool realWriteAndSync(const std::filesystem::path &path,
                      std::span<const std::byte> contents,
                      std::string &errorMessage) {
#ifdef _WIN32
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    errorMessage = "CreateFile failed: " + std::to_string(GetLastError());
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(contents.size() - offset, MAXDWORD));
    DWORD written = 0;
    if (!WriteFile(handle, contents.data() + offset, chunk, &written,
                   nullptr) ||
        written == 0) {
      errorMessage = "WriteFile failed: " + std::to_string(GetLastError());
      CloseHandle(handle);
      return false;
    }
    offset += written;
  }
  if (!FlushFileBuffers(handle)) {
    errorMessage = "FlushFileBuffers failed: " + std::to_string(GetLastError());
    CloseHandle(handle);
    return false;
  }
  if (!CloseHandle(handle)) {
    errorMessage = "CloseHandle failed: " + std::to_string(GetLastError());
    return false;
  }
#else
  const int descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (descriptor < 0) {
    errorMessage = "open failed: " + std::string(std::strerror(errno));
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const auto *data = reinterpret_cast<const char *>(contents.data() + offset);
    const ssize_t written = ::write(descriptor, data, contents.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      errorMessage = "write failed: " + std::string(std::strerror(errno));
      ::close(descriptor);
      return false;
    }
    if (written == 0) {
      errorMessage = "write failed: no progress";
      ::close(descriptor);
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    errorMessage = "fsync failed: " + std::string(std::strerror(errno));
    ::close(descriptor);
    return false;
  }
  if (::close(descriptor) != 0) {
    errorMessage = "close failed: " + std::string(std::strerror(errno));
    return false;
  }
#endif
  return true;
}

bool realReplace(const std::filesystem::path &from,
                 const std::filesystem::path &to, std::string &errorMessage) {
#ifdef _WIN32
  if (!MoveFileExW(from.c_str(), to.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    errorMessage = "MoveFileEx failed: " + std::to_string(GetLastError());
    return false;
  }
#else
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  if (ec) {
    errorMessage = "rename '" + from.string() + "' to '" + to.string() +
                   "' failed: " + ec.message();
    return false;
  }
#endif
  return true;
}

void realRemove(const std::filesystem::path &path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

bool readExistingFile(const std::filesystem::path &path,
                      std::vector<std::byte> &contents,
                      std::string &errorMessage) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input.is_open()) {
    errorMessage = "unable to open existing file for backup";
    return false;
  }
  const std::streamoff end = input.tellg();
  if (end < 0) {
    errorMessage = "unable to determine existing file size";
    return false;
  }
  contents.resize(static_cast<std::size_t>(end));
  input.seekg(0, std::ios::beg);
  if (!contents.empty()) {
    input.read(reinterpret_cast<char *>(contents.data()),
               static_cast<std::streamsize>(contents.size()));
    if (input.gcount() != static_cast<std::streamsize>(contents.size())) {
      errorMessage = "short read while preparing backup";
      return false;
    }
  }
  if (input.bad()) {
    errorMessage = "I/O failure while preparing backup";
    return false;
  }
  return true;
}

bool syncDirectoryMetadata(const std::filesystem::path &path,
                           const std::filesystem::path &syncRoot,
                           std::string &errorMessage) {
#ifdef _WIN32
  // realReplace uses MOVEFILE_WRITE_THROUGH. Windows does not expose a
  // portable directory-fsync equivalent for ordinary application handles.
  (void)path;
  (void)syncRoot;
  (void)errorMessage;
  return true;
#else
  std::filesystem::path directory = path.parent_path().empty()
                                        ? std::filesystem::path(".")
                                        : path.parent_path();
  while (!directory.empty()) {
    if (!syncDirectory(directory, errorMessage)) {
      return false;
    }
    if (directory == syncRoot) {
      break;
    }
    const std::filesystem::path parent = directory.parent_path();
    if (parent.empty() && syncRoot == std::filesystem::path(".")) {
      directory = syncRoot;
      continue;
    }
    if (parent.empty() || parent == directory) {
      errorMessage = "metadata sync root '" + syncRoot.string() +
                     "' is not an ancestor of '" + path.string() + "'";
      return false;
    }
    directory = parent;
  }
  return true;
#endif
}

bool inspectEntryWithoutFollowingLinks(const std::filesystem::path &path,
                                       bool &exists,
                                       std::string &errorMessage) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    exists = false;
    return true;
  }
  if (error) {
    errorMessage = "unable to inspect atomic file artifact '" + path.string() +
                   "': " + error.message();
    return false;
  }
  exists = status.type() != std::filesystem::file_type::not_found;
  return true;
}

bool removeAndVerify(const std::filesystem::path &path,
                     const Operations &operations, bool &removed,
                     std::string &errorMessage) {
  bool existed = false;
  if (!inspectEntryWithoutFollowingLinks(path, existed, errorMessage)) {
    return false;
  }
  operations.remove(path);
  bool remains = false;
  if (!inspectEntryWithoutFollowingLinks(path, remains, errorMessage)) {
    return false;
  }
  removed = existed && !remains;
  if (remains) {
    errorMessage =
        "atomic file artifact remains after removal: " + path.string();
    return false;
  }
  return true;
}

void appendError(std::string &errorMessage, std::string_view prefix,
                 const std::string &detail) {
  if (!errorMessage.empty()) {
    errorMessage += "; ";
  }
  errorMessage += std::string(prefix) + detail;
}
} // namespace

bool syncFile(const std::filesystem::path &path, std::string &errorMessage) {
  errorMessage.clear();
#ifdef _WIN32
  HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    errorMessage =
        "CreateFile for sync failed: " + std::to_string(GetLastError());
    return false;
  }
  if (!FlushFileBuffers(handle)) {
    errorMessage = "FlushFileBuffers failed: " + std::to_string(GetLastError());
    CloseHandle(handle);
    return false;
  }
  if (!CloseHandle(handle)) {
    errorMessage = "CloseHandle failed: " + std::to_string(GetLastError());
    return false;
  }
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    errorMessage = "open file '" + path.string() +
                   "' for sync failed: " + std::string(std::strerror(errno));
    return false;
  }
  if (::fsync(descriptor) != 0) {
    errorMessage = "file fsync for '" + path.string() +
                   "' failed: " + std::string(std::strerror(errno));
    ::close(descriptor);
    return false;
  }
  if (::close(descriptor) != 0) {
    errorMessage = "close file '" + path.string() +
                   "' failed: " + std::string(std::strerror(errno));
    return false;
  }
#endif
  return true;
}

bool syncDirectory(const std::filesystem::path &path,
                   std::string &errorMessage) {
  errorMessage.clear();
#ifdef _WIN32
  // MoveFileExW(..., MOVEFILE_WRITE_THROUGH) is the supported write-through
  // primitive used by renameDurably(). Windows has no portable directory
  // fsync for application handles.
  (void)path;
  return true;
#else
#ifdef O_DIRECTORY
  constexpr int directoryFlag = O_DIRECTORY;
#else
  constexpr int directoryFlag = 0;
#endif
  const int descriptor = ::open(path.c_str(), O_RDONLY | directoryFlag);
  if (descriptor < 0) {
    errorMessage = "open directory '" + path.string() +
                   "' failed: " + std::string(std::strerror(errno));
    return false;
  }
  if (::fsync(descriptor) != 0) {
    errorMessage = "directory fsync for '" + path.string() +
                   "' failed: " + std::string(std::strerror(errno));
    ::close(descriptor);
    return false;
  }
  if (::close(descriptor) != 0) {
    errorMessage = "close directory '" + path.string() +
                   "' failed: " + std::string(std::strerror(errno));
    return false;
  }
  return true;
#endif
}

bool renameDurably(const std::filesystem::path &from,
                   const std::filesystem::path &to, std::string &errorMessage) {
  errorMessage.clear();
  return realReplace(from, to, errorMessage);
}

Operations defaultOperations() {
  return {.writeAndSync = realWriteAndSync,
          .replace = realReplace,
          .remove = realRemove};
}

bool removeBackupArtifacts(const std::filesystem::path &path,
                           std::string &errorMessage,
                           const Operations *operations) {
  errorMessage.clear();
  const Operations defaults = defaultOperations();
  const Operations &ops = operations == nullptr ? defaults : *operations;
  if (!ops.remove) {
    errorMessage = "atomic file operations are incomplete";
    return false;
  }

  bool succeeded = true;
  bool removedAny = false;
  for (const std::string_view suffix :
       {".bak", ".bak.pending", ".bak.previous"}) {
    bool removed = false;
    std::string removalError;
    if (!removeAndVerify(path.string() + std::string(suffix), ops, removed,
                         removalError)) {
      appendError(errorMessage, "backup artifact cleanup failed: ",
                  removalError);
      succeeded = false;
    }
    removedAny = removedAny || removed;
  }

  if (removedAny) {
    const auto parent = path.parent_path().empty()
                            ? std::filesystem::path(".")
                            : path.parent_path();
    std::string syncError;
    if (!syncDirectory(parent, syncError)) {
      appendError(errorMessage, "backup cleanup metadata sync failed: ",
                  syncError);
      succeeded = false;
    }
  }
  return succeeded;
}

bool writeWithBackup(const std::filesystem::path &path,
                     std::span<const std::byte> contents,
                     std::string &errorMessage, const Operations *operations) {
  errorMessage.clear();
  const Operations defaults = defaultOperations();
  const Operations &ops = operations == nullptr ? defaults : *operations;
  if (!ops.writeAndSync || !ops.replace || !ops.remove) {
    errorMessage = "atomic file operations are incomplete";
    return false;
  }

  std::error_code ec;
  std::filesystem::path metadataSyncRoot = path.parent_path().empty()
                                               ? std::filesystem::path(".")
                                               : path.parent_path();
  while (!std::filesystem::exists(metadataSyncRoot, ec)) {
    if (ec) {
      errorMessage =
          "settings ancestor existence check failed: " + ec.message();
      return false;
    }
    const auto parent = metadataSyncRoot.parent_path();
    metadataSyncRoot = parent.empty() ? std::filesystem::path(".") : parent;
  }
  if (ec) {
    errorMessage = "settings ancestor existence check failed: " + ec.message();
    return false;
  }
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      errorMessage = "create settings directory failed: " + ec.message();
      return false;
    }
  }

  const std::filesystem::path temporary = path.string() + ".tmp";
  const std::filesystem::path backup = path.string() + ".bak";
  const std::filesystem::path backupCandidate = path.string() + ".bak.pending";
  const std::filesystem::path savedBackup = path.string() + ".bak.previous";
  ops.remove(temporary);
  ops.remove(backupCandidate);
  ops.remove(savedBackup);
  if (!ops.writeAndSync(temporary, contents, errorMessage)) {
    ops.remove(temporary);
    return false;
  }

  const bool hadBackup = std::filesystem::exists(backup, ec);
  if (ec) {
    errorMessage = "backup existence check failed: " + ec.message();
    ops.remove(temporary);
    return false;
  }
  ec.clear();
  const bool hadDestination = std::filesystem::exists(path, ec);
  if (ec) {
    errorMessage = "destination existence check failed: " + ec.message();
    ops.remove(temporary);
    return false;
  }

  bool previousBackupMoved = false;
  bool backupPrepared = false;
  auto restoreBackupState = [&]() {
    if (!hadDestination) {
      return;
    }
    std::string restoreError;
    if (hadBackup) {
      if (!ops.replace(savedBackup, backup, restoreError)) {
        appendError(errorMessage, "backup restore failed: ", restoreError);
      }
    } else {
      ops.remove(backup);
    }
    restoreError.clear();
    if (!syncDirectoryMetadata(path, metadataSyncRoot, restoreError)) {
      appendError(errorMessage,
                  "backup metadata restore failed: ", restoreError);
    }
  };

  if (hadDestination) {
    std::vector<std::byte> priorContents;
    if (!readExistingFile(path, priorContents, errorMessage) ||
        !ops.writeAndSync(backupCandidate, priorContents, errorMessage)) {
      ops.remove(temporary);
      ops.remove(backupCandidate);
      return false;
    }

    if (hadBackup) {
      if (!ops.replace(backup, savedBackup, errorMessage)) {
        ops.remove(temporary);
        ops.remove(backupCandidate);
        return false;
      }
      previousBackupMoved = true;
      if (!syncDirectoryMetadata(path, metadataSyncRoot, errorMessage)) {
        restoreBackupState();
        ops.remove(temporary);
        ops.remove(backupCandidate);
        return false;
      }
    }

    if (!ops.replace(backupCandidate, backup, errorMessage)) {
      if (previousBackupMoved) {
        restoreBackupState();
      }
      ops.remove(temporary);
      ops.remove(backupCandidate);
      return false;
    }
    backupPrepared = true;
    if (!syncDirectoryMetadata(path, metadataSyncRoot, errorMessage)) {
      restoreBackupState();
      ops.remove(temporary);
      ops.remove(backupCandidate);
      return false;
    }
  }

  if (!ops.replace(temporary, path, errorMessage)) {
    if (backupPrepared) {
      restoreBackupState();
    }
    ops.remove(temporary);
    ops.remove(backupCandidate);
    return false;
  }

  std::string syncError;
  if (!syncDirectoryMetadata(path, metadataSyncRoot, syncError)) {
    appendError(errorMessage,
                "installed-file metadata sync failed: ", syncError);
    std::string rollbackError;
    if (hadDestination) {
      if (!ops.replace(backup, path, rollbackError)) {
        appendError(errorMessage,
                    "destination rollback failed: ", rollbackError);
      } else {
        restoreBackupState();
      }
    } else {
      ops.remove(path);
      if (!syncDirectoryMetadata(path, metadataSyncRoot, rollbackError)) {
        appendError(errorMessage,
                    "new-file rollback sync failed: ", rollbackError);
      }
    }
    return false;
  }

  if (previousBackupMoved) {
    ops.remove(savedBackup);
    syncDirectoryMetadata(path, metadataSyncRoot, syncError);
  }
  return true;
}

bool writeWithoutBackup(const std::filesystem::path &path,
                        std::span<const std::byte> contents,
                        std::string &errorMessage,
                        const Operations *operations) {
  errorMessage.clear();
  const Operations defaults = defaultOperations();
  const Operations &ops = operations == nullptr ? defaults : *operations;
  if (!ops.writeAndSync || !ops.replace || !ops.remove) {
    errorMessage = "atomic file operations are incomplete";
    return false;
  }

  std::error_code ec;
  std::filesystem::path metadataSyncRoot = path.parent_path().empty()
                                               ? std::filesystem::path(".")
                                               : path.parent_path();
  while (!std::filesystem::exists(metadataSyncRoot, ec)) {
    if (ec) {
      errorMessage =
          "file ancestor existence check failed: " + ec.message();
      return false;
    }
    const auto parent = metadataSyncRoot.parent_path();
    metadataSyncRoot = parent.empty() ? std::filesystem::path(".") : parent;
  }
  if (ec) {
    errorMessage = "file ancestor existence check failed: " + ec.message();
    return false;
  }
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      errorMessage = "create file directory failed: " + ec.message();
      return false;
    }
  }

  const std::filesystem::path temporary = path.string() + ".tmp";
  if (!removeBackupArtifacts(path, errorMessage, &ops)) {
    return false;
  }
  bool removed = false;
  if (!removeAndVerify(temporary, ops, removed, errorMessage)) {
    return false;
  }
  if (!ops.writeAndSync(temporary, contents, errorMessage)) {
    std::string cleanupError;
    if (!removeAndVerify(temporary, ops, removed, cleanupError)) {
      appendError(errorMessage, "temporary cleanup failed: ", cleanupError);
    }
    return false;
  }
  if (!ops.replace(temporary, path, errorMessage)) {
    std::string cleanupError;
    if (!removeAndVerify(temporary, ops, removed, cleanupError)) {
      appendError(errorMessage, "temporary cleanup failed: ", cleanupError);
    }
    return false;
  }
  return syncDirectoryMetadata(path, metadataSyncRoot, errorMessage);
}
} // namespace atomic_file
