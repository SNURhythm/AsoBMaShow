#include "MusicSelectExternalActions.h"

#include <algorithm>
#include <cctype>

namespace {

bool endsWithTxt(std::filesystem::path path) {
  std::string text = path.string();
  std::ranges::transform(text, text.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return text.ends_with(".txt");
}

std::size_t firstCodePointLength(std::string_view text) {
  if (text.empty()) return 0;
  const unsigned char first = static_cast<unsigned char>(text.front());
  if ((first & 0x80U) == 0) return 1;
  if ((first & 0xe0U) == 0xc0U) return std::min<std::size_t>(2, text.size());
  if ((first & 0xf0U) == 0xe0U) return std::min<std::size_t>(3, text.size());
  if ((first & 0xf8U) == 0xf0U) return std::min<std::size_t>(4, text.size());
  return 1;
}

std::optional<std::filesystem::path>
firstParent(const std::vector<std::filesystem::path> &paths) {
  for (const auto &path : paths) {
    if (!path.empty()) return path.parent_path();
  }
  return std::nullopt;
}

} // namespace

std::vector<std::filesystem::path>
musicSelectDocumentPaths(const MusicSelectBar &bar) {
  std::vector<std::filesystem::path> result;
  if (bar.kind != skin::MusicSelectBarKind::Song || !bar.chart ||
      !bar.presentation.exists) {
    return result;
  }
  try {
    const auto parent = bar.chart->meta.BmsPath.parent_path();
    for (const auto &entry : std::filesystem::directory_iterator(parent)) {
      std::error_code error;
      const bool directory = std::filesystem::is_directory(entry.path(), error);
      if (!directory && endsWithTxt(entry.path())) result.push_back(entry.path());
    }
  } catch (...) {
  }
  return result;
}

std::string musicSelectExplorerTitleQuery(std::string_view title) {
  if (title.empty()) return {};
  const std::size_t firstLength = firstCodePointLength(title);
  std::size_t end = title.size();
  for (const std::string_view delimiter : {"(", "[", "~", "～"}) {
    const auto found = title.find(delimiter, firstLength);
    if (found != std::string_view::npos) end = std::min(end, found);
  }
  return std::string(title.substr(0, end));
}

std::optional<std::filesystem::path>
musicSelectExplorerPath(const MusicSelectBar &bar,
                        const MusicSelectExplorerLookups &lookups) {
  if (bar.kind == skin::MusicSelectBarKind::Folder) {
    return bar.directoryPath;
  }
  if (bar.kind != skin::MusicSelectBarKind::Song || !bar.chart) {
    return std::nullopt;
  }
  if (bar.presentation.exists) return bar.chart->meta.BmsPath.parent_path();
  if (bar.chart->originalMd5s) {
    return lookups.originalMd5Paths
               ? firstParent(lookups.originalMd5Paths(*bar.chart->originalMd5s))
               : std::nullopt;
  }
  const std::string query = musicSelectExplorerTitleQuery(bar.title);
  if (query.empty() || !lookups.textPaths) return std::nullopt;
  return firstParent(lookups.textPaths(query));
}

std::vector<std::string> musicSelectDownloadUrls(const MusicSelectBar &bar) {
  std::vector<std::string> urls;
  if (bar.kind != skin::MusicSelectBarKind::Song || !bar.chart) return urls;
  if (!bar.chart->downloadUrl.empty()) urls.push_back(bar.chart->downloadUrl);
  if (!bar.chart->appendDownloadUrl.empty() &&
      bar.chart->appendDownloadUrl != bar.chart->downloadUrl) {
    urls.push_back(bar.chart->appendDownloadUrl);
  }
  return urls;
}
