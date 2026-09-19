#pragma once
#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#endif

#include "utils/ParallelWork.h"

#include <string>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

std::string ws2s_utf8(const std::wstring &wstr);
[[nodiscard]] std::optional<std::string>
cp932_to_utf8(std::string_view value);

class Utils {
public:
  inline static std::string GameName = "AsoBMaShow";
  inline static std::string TeamName = "SNURhythm";
  static std::filesystem::path
  GetDocumentsPath(const std::filesystem::path &SubPath = "");
  static std::filesystem::path GetStoragePathRelativeToDocuments(
      const std::filesystem::path &Path, const std::filesystem::path &SubPath);
  static std::string GetStoragePathUtf8RelativeToDocuments(
      const std::filesystem::path &Path, const std::filesystem::path &SubPath);
  static bool EnsureDirectoryExists(const std::filesystem::path &Path,
                                    std::error_code &Error);
};
