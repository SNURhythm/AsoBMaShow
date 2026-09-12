#pragma once

#include "ArchiveFile.h"

#include <cstdint>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace archive_source_identity {

inline std::string KeyForPath(const std::filesystem::path &path) {
  const auto cacheKey = archive_file::cacheKeyForPath(path);
#if defined(_WIN32)
  const HANDLE handle = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return {};
  BY_HANDLE_FILE_INFORMATION information{};
  FILE_BASIC_INFO basic{};
  const bool valid = GetFileType(handle) == FILE_TYPE_DISK &&
      GetFileInformationByHandle(handle, &information) &&
      GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) &&
      (information.dwFileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0;
  CloseHandle(handle);
  if (!valid) return {};
  const auto fileId = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) |
                      information.nFileIndexLow;
  return cacheKey + "|source:windows:" +
         std::to_string(information.dwVolumeSerialNumber) + ':' +
         std::to_string(fileId) + ':' + std::to_string(basic.ChangeTime.QuadPart);
#else
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) return {};
#if defined(__APPLE__)
  const auto changed = status.st_ctimespec;
#else
  const auto changed = status.st_ctim;
#endif
  return cacheKey + "|source:posix:" +
         std::to_string(static_cast<std::uint64_t>(status.st_dev)) + ':' +
         std::to_string(static_cast<std::uint64_t>(status.st_ino)) + ':' +
         std::to_string(changed.tv_sec) + ':' + std::to_string(changed.tv_nsec);
#endif
}

}
