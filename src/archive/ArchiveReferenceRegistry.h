#pragma once

#include "ArchiveSourceAccess.h"
#include <cstdint>
#include <unordered_map>

namespace archive_source {

// Registry lifetime is independent of readers. Removing a reference or evicting
// an idle source cannot close a descriptor still owned by a reader lease.
class Registry {
public:
  using Opener = std::function<Access(const std::string &)>;
  explicit Registry(Opener opener, std::size_t idleLimit = 32)
      : opener_(std::move(opener)), idleLimit_(idleLimit) {}

  void registerSource(const std::filesystem::path &path, const std::string &uri) {
    if (!validReference(path) || uri.empty()) return;
    std::lock_guard lock(mutex_);
    auto &entry = entries_[path.generic_string()];
    if (entry.uri != uri) entry = Entry{.uri = uri};
  }

  Access acquire(const std::filesystem::path &path) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(path.generic_string());
    if (found == entries_.end()) return {{}, {}, "Archive reference is unavailable."};
    auto &entry = found->second;
    if (!entry.access) {
      entry.access = opener_(entry.uri);
      if (!entry.access) return entry.access;
    }
    entry.used = ++serial_;
    Access result = entry.access;
    trimIdle();
    return result;
  }

  void invalidate(const std::filesystem::path &path) {
    std::lock_guard lock(mutex_);
    if (auto found = entries_.find(path.generic_string()); found != entries_.end()) found->second.access = {};
  }

  bool referencesUri(const std::string &uri) {
    std::lock_guard lock(mutex_);
    for (const auto &[path, entry] : entries_) if (entry.uri == uri) return true;
    return false;
  }

  std::string remove(const std::filesystem::path &path) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(path.generic_string());
    if (found == entries_.end()) return {};
    auto uri = std::move(found->second.uri);
    entries_.erase(found);
    return uri;
  }

private:
  struct Entry {
    std::string uri;
    Access access;
    std::uint64_t used = 0;
  };
  void trimIdle() {
    for (;;) {
      std::size_t cached = 0;
      auto oldest = entries_.end();
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (!it->second.access) continue;
        ++cached;
        if (it->second.access.owner.use_count() > 1) continue;
        if (oldest == entries_.end() || it->second.used < oldest->second.used) oldest = it;
      }
      if (cached <= idleLimit_ || oldest == entries_.end()) return;
      oldest->second.access = {};
    }
  }
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  Opener opener_;
  std::size_t idleLimit_;
  std::uint64_t serial_ = 0;
};

} // namespace archive_source
