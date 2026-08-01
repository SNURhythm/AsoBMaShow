#include "DecodedImageCache.h"

#include <limits>

namespace image_decode {

std::size_t DecodedImageData::byteSize() const noexcept {
  return rgba == nullptr ? 0 : rgba->size();
}

bool DecodedImageData::valid() const noexcept {
  if (width <= 0 || height <= 0 || rgba == nullptr ||
      width > std::numeric_limits<std::uint16_t>::max() ||
      height > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  const auto pixels = static_cast<std::uint64_t>(width) *
                      static_cast<std::uint64_t>(height);
  return pixels <= std::numeric_limits<std::size_t>::max() / 4U &&
         rgba->size() == static_cast<std::size_t>(pixels) * 4U;
}

DecodedImageCache::DecodedImageCache(std::size_t byteBudget)
    : byteBudget_(byteBudget) {}

std::optional<DecodedImageData> DecodedImageCache::get(std::string_view key) {
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    return std::nullopt;
  }
  touch(found);
  return found->second.image;
}

bool DecodedImageCache::put(std::string key, DecodedImageData image) {
  if (key.empty() || !image.valid()) {
    return false;
  }
  std::size_t retainedPins = 0;
  if (const auto existing = entries_.find(key); existing != entries_.end()) {
    retainedPins = existing->second.pinCount;
    bytes_ -= existing->second.image.byteSize();
    recency_.erase(existing->second.recency);
    entries_.erase(existing);
  }
  recency_.push_front(key);
  const auto recency = recency_.begin();
  bytes_ += image.byteSize();
  entries_.emplace(std::move(key),
                   Entry{.image = std::move(image),
                         .pinCount = retainedPins,
                         .recency = recency});
  evictToBudget(*recency);
  return true;
}

bool DecodedImageCache::pin(std::string_view key) {
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    return false;
  }
  ++found->second.pinCount;
  touch(found);
  return true;
}

bool DecodedImageCache::unpin(std::string_view key) {
  const auto found = entries_.find(key);
  if (found == entries_.end() || found->second.pinCount == 0) {
    return false;
  }
  --found->second.pinCount;
  evictToBudget();
  return true;
}

bool DecodedImageCache::erase(std::string_view key) {
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    return false;
  }
  bytes_ -= found->second.image.byteSize();
  recency_.erase(found->second.recency);
  entries_.erase(found);
  return true;
}

std::size_t DecodedImageCache::erasePrefix(std::string_view prefix) {
  std::vector<std::string> keys;
  for (const auto &[key, entry] : entries_) {
    (void)entry;
    if (key.starts_with(prefix)) {
      keys.push_back(key);
    }
  }
  for (const auto &key : keys) {
    (void)erase(key);
  }
  return keys.size();
}

void DecodedImageCache::clearEvictable() {
  for (auto iterator = entries_.begin(); iterator != entries_.end();) {
    if (iterator->second.pinCount != 0) {
      ++iterator;
      continue;
    }
    bytes_ -= iterator->second.image.byteSize();
    recency_.erase(iterator->second.recency);
    iterator = entries_.erase(iterator);
  }
}

void DecodedImageCache::clear() {
  entries_.clear();
  recency_.clear();
  bytes_ = 0;
}

bool DecodedImageCache::contains(std::string_view key) const {
  return entries_.contains(key);
}

std::size_t DecodedImageCache::size() const noexcept { return entries_.size(); }
std::size_t DecodedImageCache::bytes() const noexcept { return bytes_; }
std::size_t DecodedImageCache::budget() const noexcept { return byteBudget_; }

void DecodedImageCache::touch(
    std::map<std::string, Entry, std::less<>>::iterator entry) {
  if (entry->second.recency == recency_.begin()) {
    return;
  }
  recency_.splice(recency_.begin(), recency_, entry->second.recency);
  entry->second.recency = recency_.begin();
}

void DecodedImageCache::evictToBudget(std::string_view protectedKey) {
  while (bytes_ > byteBudget_) {
    auto candidate = recency_.rend();
    for (auto iterator = recency_.rbegin(); iterator != recency_.rend();
         ++iterator) {
      const auto entry = entries_.find(*iterator);
      if (*iterator != protectedKey && entry != entries_.end() &&
          entry->second.pinCount == 0) {
        candidate = iterator;
        break;
      }
    }
    if (candidate == recency_.rend()) {
      break;
    }
    const std::string key = *candidate;
    (void)erase(key);
  }
}

} // namespace image_decode
