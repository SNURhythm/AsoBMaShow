#pragma once

#include "../ArchiveFile.h"
#include "../FileExtensionResolver.h"
#include <atomic>
#include <array>
#include <cstdint>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <unordered_map>
#include <unordered_set>

namespace audio {

// Preparation-local negative lookup filter. Android emulated storage can make
// missing-file stat calls much slower than a bounded directory listing. Positive
// candidates still use the normal existence check (including symlink handling).
// This owns names only, never audio, and is destroyed before gameplay starts.
class ChartAudioFileLookup {
public:
  explicit ChartAudioFileLookup(const std::atomic_bool &cancelled)
      : cancelled_(cancelled) {}

  std::optional<std::filesystem::path>
  Find(const std::filesystem::path &path,
       const std::vector<std::string_view> &extensions) {
    if (cancelled_) return std::nullopt;
#ifdef _WIN32
    // Windows short-name aliases cannot be ruled out by listing long names.
    return archive_file::findFileWithExtensions(path, extensions);
#else
    if (!path.is_absolute() || archive_file::isVirtualPath(path))
      return archive_file::findFileWithExtensions(path, extensions);
    const auto parent = path.parent_path();
    auto it = directories_.find(parent);
    if (it == directories_.end() && directories_.size() < 32)
      it = directories_.emplace(parent, Directory{}).first;
    Directory *directory = it == directories_.end() ? nullptr : &it->second;
    if (directory && directory->ready) {
      const auto stamp = directoryStamp(parent);
      if (!stamp || *stamp != directory->stamp) {
        directory->names.clear();
        directory->ready = false;
      }
    }
    return file_extension_resolver::find(path, extensions,
        [&](const std::filesystem::path &candidate) -> std::optional<std::filesystem::path> {
          if (cancelled_) return std::nullopt;
          const auto name = foldedAscii(candidate.filename().string());
          if (directory && directory->ready && name && !directory->names.contains(*name))
            return std::nullopt;
          if (archive_file::exists(candidate)) return candidate;
          // Avoid any listing for charts whose original filenames all exist.
          if (directory && !directory->ready && !directory->disabled)
            snapshot(parent, *directory);
          return std::nullopt;
        });
#endif
  }

private:
  using Stamp = std::array<std::uint64_t, 6>;
  static std::optional<Stamp> directoryStamp(const std::filesystem::path &path) {
#ifndef _WIN32
    struct stat status{};
    if (::stat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) return std::nullopt;
#if defined(__APPLE__)
    const auto modified = status.st_mtimespec;
    const auto changed = status.st_ctimespec;
#else
    const auto modified = status.st_mtim;
    const auto changed = status.st_ctim;
#endif
    return Stamp{static_cast<std::uint64_t>(status.st_dev),
                 static_cast<std::uint64_t>(status.st_ino),
                 static_cast<std::uint64_t>(modified.tv_sec),
                 static_cast<std::uint64_t>(modified.tv_nsec),
                 static_cast<std::uint64_t>(changed.tv_sec),
                 static_cast<std::uint64_t>(changed.tv_nsec)};
#else
    return std::nullopt;
#endif
  }
  struct Directory {
    std::unordered_set<std::string> names;
    Stamp stamp{};
    bool ready = false;
    bool disabled = false;
  };
  static std::optional<std::string> foldedAscii(std::string value) {
    for (char &c : value) {
      if (static_cast<unsigned char>(c) >= 128) return std::nullopt;
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return value;
  }
  void snapshot(const std::filesystem::path &parent, Directory &directory) {
    // Bound aggregate enumeration, including refreshes. Oversized, changing,
    // unreadable or Unicode directories retain the original lookup behavior.
    directory.disabled = true;
    std::error_code error;
    const auto before = directoryStamp(parent);
    if (!before) return;
    std::unordered_set<std::string> names;
    std::filesystem::directory_iterator entry(parent, error), end;
    while (!error && entry != end) {
      if (cancelled_ || scanned_ >= 16384 || names.size() >= 4096) return;
      ++scanned_;
      auto name = foldedAscii(entry->path().filename().string());
      if (!name) return;
      names.insert(std::move(*name));
      entry.increment(error);
    }
    if (error) return;
    const auto after = directoryStamp(parent);
    if (!after || before != after) return;
    directory.names = std::move(names);
    directory.stamp = *after;
    directory.ready = true;
    directory.disabled = false;
  }
  const std::atomic_bool &cancelled_;
  std::unordered_map<std::filesystem::path, Directory> directories_;
  std::size_t scanned_ = 0;
};

}
