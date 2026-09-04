#pragma once

#include "../../view/DecodedImageCache.h"

#include <cstddef>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace skin {

// Value-owned decoded bitmap-font pages and skin images produced for one skin
// revision. A later chart attempt with an unchanged revision can reuse these
// pixels instead of re-decoding them on every start.
struct SkinDecodeCacheEntry {
  std::map<std::string, image_decode::DecodedImageData, std::less<>> fontPages;
  std::map<std::string, std::size_t, std::less<>> pageEncodedBytes;
  std::map<std::string, image_decode::DecodedImageData, std::less<>> skinImages;
  std::size_t decodedBytes = 0;
};

// Revision-keyed decode cache shared across threads: the render thread clears
// it (dropAll) while asynchronous planning threads read and populate it
// (find*/store*). Every method synchronizes on an internal mutex, so the
// cache is safe for concurrent access without the caller serializing on the
// owning service. DecodedImageData is shared_ptr-backed, so all values are
// copied out cheaply and no raw pointer or reference escapes a lock.
class SkinDecodeCache {
public:
  using Key = std::string; // lowercased revision sha256

  [[nodiscard]] std::optional<image_decode::DecodedImageData>
  findSkinImage(std::string_view revisionKey, std::string_view imageKey) const;
  void storeSkinImage(Key revisionKey, std::string imageKey,
                      image_decode::DecodedImageData image);

  [[nodiscard]] std::optional<image_decode::DecodedImageData>
  findFontPage(std::string_view revisionKey,
               std::string_view physicalKey) const;
  [[nodiscard]] std::optional<std::size_t> findFontPageEncodedBytes(
      std::string_view revisionKey, std::string_view physicalKey) const;
  void storeFontPage(Key revisionKey, std::string physicalKey,
                     image_decode::DecodedImageData pixels,
                     std::optional<std::size_t> encodedBytes);

  void dropAll();
  [[nodiscard]] std::size_t decodedBytes() const noexcept;

private:
  mutable std::shared_mutex mutex_;
  std::map<Key, SkinDecodeCacheEntry, std::less<>> entries_;
};

} // namespace skin
