#include "SkinDecodeCache.h"

namespace skin {

const SkinDecodeCacheEntry *SkinDecodeCache::entry(std::string_view key) const {
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    return nullptr;
  }
  return &found->second;
}

SkinDecodeCacheEntry &SkinDecodeCache::mutableEntry(std::string_view key) {
  return entries_[std::string(key)];
}

void SkinDecodeCache::dropAll() {
  entries_.clear();
}

std::size_t SkinDecodeCache::decodedBytes() const noexcept {
  std::size_t total = 0;
  for (const auto &revision : entries_) {
    total += revision.second.decodedBytes;
  }
  return total;
}

} // namespace skin