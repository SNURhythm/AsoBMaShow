#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace resource_path {

// Read-only resource lookup. Never use this to identify mutation destinations.
// Exact native lookup wins; ASCII-folded aliases must be unique in each parent.
class CaseInsensitivePathLookup {
public:
  std::optional<std::filesystem::path> Find(const std::filesystem::path &path,
                                         const std::atomic_bool *cancelled = nullptr) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (stopped(cancelled) || knownMissing(path, cancelled)) return std::nullopt;
    if (fs::exists(path, error)) return path;
    auto parent = fs::absolute(path, error);
    if (error || parent.empty()) return std::nullopt;
    std::vector<fs::path> missing;
    // Find the nearest existing ancestor without collapsing symlink/.. pairs.
    while (!fs::exists(parent, error)) {
      if (stopped(cancelled)) return std::nullopt;
      const auto next = parent.parent_path();
      if (next == parent || parent.filename().empty()) return std::nullopt;
      missing.push_back(parent.filename());
      parent = next;
    }
    for (auto part = missing.rbegin(); part != missing.rend(); ++part) {
      if (stopped(cancelled)) return std::nullopt;
      auto candidate = parent / *part;
      if (!fs::exists(candidate, error)) {
        const auto name = findName(parent, *part, cancelled);
        if (!name) return std::nullopt;
        candidate = parent / *name;
      }
      parent = std::move(candidate);
    }
    return fs::exists(parent, error) ? std::optional{parent} : std::nullopt;
  }

private:
  static bool stopped(const std::atomic_bool *cancelled) {
    return cancelled && cancelled->load(std::memory_order_relaxed);
  }
  std::unique_lock<std::timed_mutex> acquire(const std::atomic_bool *cancelled) {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    if (!cancelled) lock.lock();
    else while (!stopped(cancelled)) {
      if (lock.try_lock_for(std::chrono::milliseconds(10))) break;
    }
    return lock;
  }
  using Stamp = std::array<std::uint64_t, 6>;
  static std::optional<Stamp> stamp(const std::filesystem::path &path) {
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
    const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    FILE_BASIC_INFO basic{};
    BY_HANDLE_FILE_INFORMATION identity{};
    const bool valid = GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) &&
                       GetFileInformationByHandle(handle, &identity);
    CloseHandle(handle);
    if (!valid || !(basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return std::nullopt;
    return Stamp{identity.dwVolumeSerialNumber,
                 (static_cast<std::uint64_t>(identity.nFileIndexHigh) << 32) | identity.nFileIndexLow,
                 static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart), 0,
                 static_cast<std::uint64_t>(basic.ChangeTime.QuadPart), 0};
#endif
  }
  static std::string folded(const std::filesystem::path &path) {
    const auto utf8 = path.u8string();
    std::string result(utf8.begin(), utf8.end());
    for (char &c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
  }
  struct Directory {
    Stamp stamp;
    // Empty path means multiple spellings share this folded key.
    std::unordered_map<std::string, std::filesystem::path> names;
    std::size_t bytes = 0;
    std::uint64_t used = 0;
    bool asciiOnly = true;
  };
  bool knownMissing(const std::filesystem::path &path, const std::atomic_bool *cancelled) {
#ifdef _WIN32
    return false; // A long-name listing cannot rule out native 8.3 aliases.
#else
    // Preserve native Unicode normalization/case aliases, including on macOS.
    // Only a complete ASCII directory listing can rule out an ASCII name.
    const auto name = folded(path.filename());
    for (const unsigned char c : name) if (c >= 128) return false;
    std::error_code error;
    const auto parent = std::filesystem::absolute(path.has_parent_path() ? path.parent_path() : ".", error);
    if (error) return false;
    auto lock = acquire(cancelled);
    if (!lock || stopped(cancelled)) return true;
    const auto it = directories_.find(parent);
    if (it == directories_.end() || !it->second.asciiOnly) return false;
    const auto current = stamp(parent);
    if (!current || *current != it->second.stamp) { erase(it); return false; }
    it->second.used = ++clock_;
    return !it->second.names.contains(name);
#endif
  }
  std::optional<std::filesystem::path> findName(
      const std::filesystem::path &parent, const std::filesystem::path &requested,
      const std::atomic_bool *cancelled) {
    auto lock = acquire(cancelled);
    if (!lock || stopped(cancelled)) return std::nullopt;
    const auto before = stamp(parent);
    if (!before) return std::nullopt;
    const auto key = folded(requested);
    auto cached = directories_.find(parent);
    if (cached != directories_.end()) {
      if (cached->second.stamp == *before) {
        cached->second.used = ++clock_;
        const auto found = cached->second.names.find(key);
        return found != cached->second.names.end() && !found->second.empty()
                   ? std::optional{found->second} : std::nullopt;
      }
      erase(cached);
    }
    Directory directory{.stamp = *before, .used = ++clock_};
    bool retain = true, ambiguous = false;
#ifndef _WIN32
    // HFS/FAT can report only whole seconds. Do not cache a directory while
    // another mutation could receive the very same change timestamp.
    if ((*before)[5] == 0 && static_cast<std::uint64_t>(std::time(nullptr)) <= (*before)[4])
      retain = false;
#endif
    std::optional<std::filesystem::path> match;
    std::error_code error;
    std::filesystem::directory_iterator entry(parent, error), end;
    for (; !error && entry != end; entry.increment(error)) {
      if (stopped(cancelled)) return std::nullopt;
      const auto name = entry->path().filename();
      auto lower = folded(name);
      for (const unsigned char c : lower) if (c >= 128) directory.asciiOnly = false;
      if (lower == key) {
        if (match) ambiguous = true;
        match = name;
      }
      if (!retain) continue;
      directory.bytes += lower.size() + name.native().size() * sizeof(std::filesystem::path::value_type);
      if (directory.names.size() >= maxNames || directory.bytes > maxBytes) {
        directory.names.clear();
        retain = false; // Finish this lookup by streaming; never cache a partial index.
        continue;
      }
      const auto [item, inserted] = directory.names.emplace(std::move(lower), name);
      if (!inserted) item->second.clear();
    }
    if (error || stopped(cancelled) || stamp(parent) != before) return std::nullopt;
    if (retain) {
      while (!directories_.empty() &&
             (directories_.size() >= maxDirectories || names_ + directory.names.size() > maxNames ||
              bytes_ + directory.bytes > maxBytes)) {
        auto oldest = directories_.begin();
        for (auto it = directories_.begin(); it != directories_.end(); ++it)
          if (it->second.used < oldest->second.used) oldest = it;
        erase(oldest);
      }
      names_ += directory.names.size();
      bytes_ += directory.bytes;
      directories_.emplace(parent, std::move(directory));
    }
    return ambiguous ? std::nullopt : match;
  }
  using Directories = std::unordered_map<std::filesystem::path, Directory>;
  void erase(Directories::iterator it) {
    names_ -= it->second.names.size();
    bytes_ -= it->second.bytes;
    directories_.erase(it);
  }
  static constexpr std::size_t maxDirectories = 64, maxNames = 65536, maxBytes = 4 * 1024 * 1024;
  std::timed_mutex mutex_;
  Directories directories_;
  std::size_t names_ = 0, bytes_ = 0;
  std::uint64_t clock_ = 0;
};

} // namespace resource_path
