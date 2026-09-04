#pragma once

#include "view/DecodedImageCache.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace skin {

// Value-owned decoded bitmap-font pages and skin images produced for one skin
// revision. A later chart attempt with an unchanged revision can reuse these
// pixels instead of re-decoding them on every start.
struct SkinDecodeCacheEntry {
  std::map<std::string, image_decode::DecodedImageData, std::less<>> fontPages;
  std::map<std::string, image_decode::DecodedImageData, std::less<>> skinImages;
  std::size_t decodedBytes = 0;
};

class SkinDecodeCache {
public:
  using Key = std::string; // lowercased revision sha256

  [[nodiscard]] const SkinDecodeCacheEntry *entry(std::string_view key) const;
  SkinDecodeCacheEntry &mutableEntry(std::string_view key);
  void dropAll();
  [[nodiscard]] std::size_t decodedBytes() const noexcept;

private:
  std::map<Key, SkinDecodeCacheEntry, std::less<>> entries_;
};

} // namespace skin