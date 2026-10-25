#pragma once

#include "../ArchiveFile.h"
#include <atomic>

namespace audio {

// Share the native directory-name cache with BGA, previews and artwork. It
// handles both case fallback and negative lookups without a second listing.
class ChartAudioFileLookup {
public:
  explicit ChartAudioFileLookup(const std::atomic_bool &cancelled)
      : cancelled_(cancelled) {}

  std::optional<std::filesystem::path>
  Find(const std::filesystem::path &path,
       const std::vector<std::string_view> &extensions) {
    if (cancelled_) return std::nullopt;
    const auto result = archive_file::findFileWithExtensions(path, extensions, &cancelled_);
    return cancelled_ ? std::nullopt : result;
  }

private:
  const std::atomic_bool &cancelled_;
};

}
