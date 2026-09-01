#pragma once

#include "MusicSelectTypes.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct MusicSelectExplorerLookups {
  std::function<std::vector<std::filesystem::path>(
      std::span<const std::string>)>
      originalMd5Paths;
  std::function<std::vector<std::filesystem::path>(std::string_view)>
      textPaths;
};

using MusicSelectArchivePathSplitter = std::function<bool(
    const std::filesystem::path &, std::filesystem::path &,
    std::filesystem::path &)>;

[[nodiscard]] std::vector<std::filesystem::path>
musicSelectDocumentPaths(const MusicSelectBar &);

[[nodiscard]] std::string
musicSelectExplorerTitleQuery(std::string_view title);

[[nodiscard]] std::optional<std::filesystem::path>
musicSelectExplorerPath(const MusicSelectBar &,
                        const MusicSelectExplorerLookups &);

[[nodiscard]] std::optional<std::filesystem::path>
musicSelectRefreshPath(const MusicSelectBar &,
                       const MusicSelectArchivePathSplitter &);

[[nodiscard]] std::vector<std::string>
musicSelectDownloadUrls(const MusicSelectBar &);
