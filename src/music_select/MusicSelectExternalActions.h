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

using MusicSelectArchiveDocumentResolver = std::function<
    std::optional<std::vector<std::filesystem::path>>(
        const std::filesystem::path &)>;

using MusicSelectArchivePathSplitter = std::function<bool(
    const std::filesystem::path &, std::filesystem::path &,
    std::filesystem::path &)>;

[[nodiscard]] std::vector<std::filesystem::path>
musicSelectDocumentPaths(const MusicSelectBar &,
                         const MusicSelectArchiveDocumentResolver & = {});

[[nodiscard]] std::string
musicSelectExplorerTitleQuery(std::string_view title);

[[nodiscard]] std::optional<std::filesystem::path>
musicSelectExplorerPath(const MusicSelectBar &,
                        const MusicSelectExplorerLookups &,
                        const MusicSelectArchivePathSplitter & = {});

[[nodiscard]] std::optional<std::filesystem::path>
musicSelectRefreshPath(const MusicSelectBar &,
                       const MusicSelectArchivePathSplitter &);

[[nodiscard]] std::vector<std::string>
musicSelectDownloadUrls(const MusicSelectBar &);
