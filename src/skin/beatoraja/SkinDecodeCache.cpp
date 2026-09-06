#include "SkinDecodeCache.h"

#include <algorithm>
#include <utility>

namespace skin {

void SkinDecodeCache::touchLocked(std::string_view revisionKey,
                                  std::string_view entryKey) const {
  const auto found = std::ranges::find_if(
      order_, [&](const auto &item) {
        return item.first == revisionKey && item.second == entryKey;
      });
  if (found != order_.end()) {
    const auto value = *found;
    order_.erase(found);
    order_.push_back(value);
  }
}

void SkinDecodeCache::evictIfOverBudgetLocked() {
  std::size_t total = 0;
  for (const auto &[revisionKey, entry] : entries_) {
    (void)revisionKey;
    total += entry.decodedBytes;
  }
  while (total > byteBudget_ && !order_.empty()) {
    const auto [revisionKey, entryKey] = order_.front();
    order_.pop_front();
    const auto foundRevision = entries_.find(revisionKey);
    if (foundRevision == entries_.end()) {
      continue;
    }
    auto &entry = foundRevision->second;
    const std::size_t evictedBytes = [&]() {
      if (const auto image = entry.skinImages.find(entryKey);
          image != entry.skinImages.end()) {
        const std::size_t bytes = image->second.byteSize();
        entry.skinImages.erase(image);
        return bytes;
      }
      if (const auto page = entry.fontPages.find(entryKey);
          page != entry.fontPages.end()) {
        const std::size_t bytes = page->second.byteSize();
        entry.fontPages.erase(page);
        entry.pageEncodedBytes.erase(entryKey);
        return bytes;
      }
      return static_cast<std::size_t>(0);
    }();
    entry.decodedBytes =
        entry.decodedBytes > evictedBytes ? entry.decodedBytes - evictedBytes
                                          : 0;
    total = total > evictedBytes ? total - evictedBytes : 0;
    if (entry.skinImages.empty() && entry.fontPages.empty()) {
      entries_.erase(foundRevision);
    }
  }
}

std::optional<image_decode::DecodedImageData>
SkinDecodeCache::findSkinImage(std::string_view revisionKey,
                               std::string_view imageKey) const {
  std::shared_lock lock(mutex_);
  const auto foundRevision = entries_.find(revisionKey);
  if (foundRevision == entries_.end()) {
    return std::nullopt;
  }
  const auto found = foundRevision->second.skinImages.find(imageKey);
  if (found == foundRevision->second.skinImages.end()) {
    return std::nullopt;
  }
  touchLocked(revisionKey, imageKey);
  return found->second;
}

void SkinDecodeCache::storeSkinImage(Key revisionKey, std::string imageKey,
                                     image_decode::DecodedImageData image) {
  std::unique_lock lock(mutex_);
  const std::size_t bytes = image.byteSize();
  auto &entry = entries_[revisionKey];
  const auto previous = entry.skinImages.find(imageKey);
  if (previous != entry.skinImages.end()) {
    const std::size_t previousBytes = previous->second.byteSize();
    entry.decodedBytes =
        entry.decodedBytes > previousBytes ? entry.decodedBytes - previousBytes
                                           : 0;
    entry.skinImages.erase(previous);
    const auto stale = std::ranges::find_if(
        order_, [&](const auto &item) {
          return item.first == revisionKey && item.second == imageKey;
        });
    if (stale != order_.end()) order_.erase(stale);
  }
  entry.skinImages.emplace(imageKey, std::move(image));
  entry.decodedBytes += bytes;
  order_.emplace_back(std::move(revisionKey), std::move(imageKey));
  evictIfOverBudgetLocked();
}

std::optional<image_decode::DecodedImageData>
SkinDecodeCache::findFontPage(std::string_view revisionKey,
                              std::string_view physicalKey) const {
  std::shared_lock lock(mutex_);
  const auto foundRevision = entries_.find(revisionKey);
  if (foundRevision == entries_.end()) {
    return std::nullopt;
  }
  const auto found = foundRevision->second.fontPages.find(physicalKey);
  if (found == foundRevision->second.fontPages.end()) {
    return std::nullopt;
  }
  touchLocked(revisionKey, physicalKey);
  return found->second;
}

std::optional<std::size_t> SkinDecodeCache::findFontPageEncodedBytes(
    std::string_view revisionKey, std::string_view physicalKey) const {
  std::shared_lock lock(mutex_);
  const auto foundRevision = entries_.find(revisionKey);
  if (foundRevision == entries_.end()) {
    return std::nullopt;
  }
  const auto found = foundRevision->second.pageEncodedBytes.find(physicalKey);
  if (found == foundRevision->second.pageEncodedBytes.end()) {
    return std::nullopt;
  }
  return found->second;
}

void SkinDecodeCache::storeFontPage(Key revisionKey, std::string physicalKey,
                                    image_decode::DecodedImageData pixels,
                                    std::optional<std::size_t> encodedBytes) {
  std::unique_lock lock(mutex_);
  const std::size_t bytes = pixels.byteSize();
  auto &entry = entries_[revisionKey];
  const auto previous = entry.fontPages.find(physicalKey);
  if (previous != entry.fontPages.end()) {
    const std::size_t previousBytes = previous->second.byteSize();
    entry.decodedBytes =
        entry.decodedBytes > previousBytes ? entry.decodedBytes - previousBytes
                                           : 0;
    entry.fontPages.erase(previous);
    const auto stale = std::ranges::find_if(
        order_, [&](const auto &item) {
          return item.first == revisionKey && item.second == physicalKey;
        });
    if (stale != order_.end()) order_.erase(stale);
  }
  entry.fontPages.emplace(physicalKey, std::move(pixels));
  entry.decodedBytes += bytes;
  if (encodedBytes) {
    entry.pageEncodedBytes.insert_or_assign(physicalKey, *encodedBytes);
  }
  order_.emplace_back(std::move(revisionKey), std::move(physicalKey));
  evictIfOverBudgetLocked();
}

void SkinDecodeCache::dropAll() {
  std::unique_lock lock(mutex_);
  entries_.clear();
  order_.clear();
}

std::size_t SkinDecodeCache::decodedBytes() const noexcept {
  std::shared_lock lock(mutex_);
  std::size_t total = 0;
  for (const auto &revision : entries_) {
    total += revision.second.decodedBytes;
  }
  return total;
}

} // namespace skin