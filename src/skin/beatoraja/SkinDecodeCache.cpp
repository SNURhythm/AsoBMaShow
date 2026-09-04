#include "SkinDecodeCache.h"

namespace skin {

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
  return found->second;
}

void SkinDecodeCache::storeSkinImage(Key revisionKey, std::string imageKey,
                                     image_decode::DecodedImageData image) {
  std::unique_lock lock(mutex_);
  entries_[std::move(revisionKey)]
      .skinImages.emplace(std::move(imageKey), std::move(image));
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
  auto &entry = entries_[std::move(revisionKey)];
  entry.fontPages.emplace(physicalKey, std::move(pixels));
  if (encodedBytes) {
    entry.pageEncodedBytes.insert_or_assign(physicalKey, *encodedBytes);
  }
}

void SkinDecodeCache::dropAll() {
  std::unique_lock lock(mutex_);
  entries_.clear();
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
