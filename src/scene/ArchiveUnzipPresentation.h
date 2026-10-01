#pragma once

#include "../i18n/Localization.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace archive_unzip_presentation {

// ArchiveFile's progress strings are a low-level protocol. Translate only its
// exact known statuses; filenames and unrecognized diagnostics remain literal.
inline i18n::Text status(std::string_view value, bool archiveLabel = false) {
  static constexpr std::pair<std::string_view, const char *> statuses[] = {
      {"Preparing unzip", "library.archive.operation.preparing_unzip.label"},
      {"Archive finished", "library.archive.operation.archive_finished.label"},
      {"Indexing extracted folders", "library.archive.operation.indexing_extracted_folders.label"},
      {"Indexing extracted charts", "library.archive.operation.indexing_extracted_charts.label"},
      {"Refreshing library", "library.archive.operation.refreshing_library.label"},
      {"Reading archive index", "library.archive.progress.reading_index"},
      {"Preparing output folder", "library.archive.progress.preparing_folder"},
      {"Using existing unzipped folder", "library.archive.progress.reusing_folder"},
      {"Reading archive files", "library.archive.progress.reading_files"},
      {"Writing unzipped files", "library.archive.progress.writing_files"},
      {"Finalizing unzip", "library.archive.progress.finalizing"},
      {"Unzip complete", "library.archive.unzip_complete.label"},
      {"Unzipping archive", "library.archive.progress.extracting"},
  };
  for (const auto &[literal, key] : statuses) {
    if (value == literal) return i18n::message(key);
    if (archiveLabel && value.size() > literal.size() + 3 &&
        value.ends_with(literal)) {
      const auto prefix = value.substr(0, value.size() - literal.size());
      if (prefix.ends_with(") - ")) {
        return i18n::message("library.archive.progress.archive_status",
            {{"archive", prefix.substr(0, prefix.size() - 3)},
             {"status", i18n::message(key)}});
      }
    }
  }
  return i18n::Text(value);
}

inline i18n::Text progressCount(const std::string &percent,
                                std::uint64_t current, std::uint64_t total,
                                bool archives) {
  return total > 0
      ? i18n::message(archives ? "library.archive.progress.archives_completed"
                               : "library.archive.progress.count",
          {{"percent", percent}, {"current", std::to_string(current)},
           {"total", std::to_string(total)}})
      : i18n::Text(percent);
}

template <typename Progress>
inline i18n::Text progressMessage(const Progress &progress,
                                  bool batch) {
  if (!batch || progress.indexing) return status(progress.message);
  const auto visible = std::min<std::size_t>(3, progress.activeArchives.size());
  i18n::Text message;
  for (std::size_t index = 0; index < visible; ++index) {
    auto line = status(progress.activeArchives[index], true);
    message = index == 0 ? line : i18n::message("library.archive.text.line",
        {{"message", message}, {"detail", line}});
  }
  if (progress.activeArchives.size() > visible) {
    message = i18n::message("library.archive.text.line", {{"message", message},
        {"detail", i18n::message("library.archive.progress.other_active",
            {{"count", std::to_string(progress.activeArchives.size() - visible)}})}});
  }
  return message.empty()
      ? i18n::message("library.archive.finishing_archive_extraction.label") : message;
}

} // namespace archive_unzip_presentation
