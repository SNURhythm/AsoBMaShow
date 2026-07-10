#include "AtomicFile.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>

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
    if (!WriteFile(handle, contents.data() + offset, chunk, &written, nullptr) ||
        written == 0) {
      errorMessage = "WriteFile failed: " + std::to_string(GetLastError());
      CloseHandle(handle);
      return false;
    }
    offset += written;
  }
  if (!FlushFileBuffers(handle)) {
    errorMessage = "FlushFileBuffers failed: " +
                   std::to_string(GetLastError());
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
                 const std::filesystem::path &to,
                 std::string &errorMessage) {
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  if (ec) {
    errorMessage = "rename '" + from.string() + "' to '" + to.string() +
                   "' failed: " + ec.message();
    return false;
  }
  return true;
}

void realRemove(const std::filesystem::path &path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}
} // namespace

Operations defaultOperations() {
  return {.writeAndSync = realWriteAndSync,
          .replace = realReplace,
          .remove = realRemove};
}

bool writeWithBackup(const std::filesystem::path &path,
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
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      errorMessage = "create settings directory failed: " + ec.message();
      return false;
    }
  }

  const std::filesystem::path temporary = path.string() + ".tmp";
  const std::filesystem::path backup = path.string() + ".bak";
  const std::filesystem::path savedBackup = path.string() + ".bak.previous";
  ops.remove(temporary);
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
  if (hadDestination && hadBackup &&
      !ops.replace(backup, savedBackup, errorMessage)) {
    ops.remove(temporary);
    return false;
  }

  if (hadDestination && !ops.replace(path, backup, errorMessage)) {
    if (hadDestination && hadBackup) {
      std::string ignored;
      ops.replace(savedBackup, backup, ignored);
    }
    ops.remove(temporary);
    return false;
  }

  if (!ops.replace(temporary, path, errorMessage)) {
    std::string restoreError;
    if (hadDestination && !ops.replace(backup, path, restoreError)) {
      errorMessage += "; destination restore failed: " + restoreError;
    }
    if (hadDestination && hadBackup) {
      restoreError.clear();
      if (!ops.replace(savedBackup, backup, restoreError)) {
        errorMessage += "; backup restore failed: " + restoreError;
      }
    }
    ops.remove(temporary);
    return false;
  }

  if (hadDestination && hadBackup) {
    ops.remove(savedBackup);
  }
  return true;
}
} // namespace atomic_file
