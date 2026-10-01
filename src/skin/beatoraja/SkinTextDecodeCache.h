#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../../text/Utf8.h"

namespace skin {

// Accessed on the renderer's frame thread, like the layouts it prepares.
class SkinTextDecodeCache final {
public:
  using Codepoints = std::shared_ptr<const std::vector<char32_t>>;

  explicit SkinTextDecodeCache(std::size_t maximumEntries = 256,
                               std::size_t maximumTextBytes = 1024)
      : maximumTextBytes_(maximumTextBytes), entries_(maximumEntries) {}

  SkinTextDecodeCache(const SkinTextDecodeCache &) = delete;
  SkinTextDecodeCache &operator=(const SkinTextDecodeCache &) = delete;
  SkinTextDecodeCache(SkinTextDecodeCache &&) = default;
  SkinTextDecodeCache &operator=(SkinTextDecodeCache &&) = default;

  // Only raw, valid Unicode scalars are cached. Atlas membership, CR removal,
  // and per-frame glyph limits still belong to the renderer. A null result
  // asks it to use its bounded scalar path (including diagnostic precedence).
  Codepoints decode(std::string_view value) {
    if (entries_.empty() || value.size() > maximumTextBytes_) return {};
    auto &entry = entries_[std::hash<std::string_view>{}(value) % entries_.size()];
    if (entry.codepoints && entry.text == value) {
      return entry.codepoints;
    }

    // Reuse storage on changing values only after every prepared layout has
    // released it. Otherwise a collision must allocate separate storage.
    auto decoded = entry.codepoints.use_count() == 1
                       ? std::move(entry.codepoints)
                       : std::make_shared<std::vector<char32_t>>();
    if (!asobmashow::text::decodeUtf8(value, *decoded)) return {};

    entry.text.assign(value);
    entry.codepoints = decoded;
    return decoded;
  }

  void clear() {
    for (auto &entry : entries_) entry = {};
  }

private:
  struct Entry {
    std::string text;
    std::shared_ptr<std::vector<char32_t>> codepoints;
  };

  // A fixed slot table avoids node allocations/eviction bookkeeping during
  // rapid text changes. Hash collisions affect reuse, never correctness:
  // hits compare the full string. Slots and per-entry bytes are both bounded.
  std::size_t maximumTextBytes_;
  std::vector<Entry> entries_;
};

} // namespace skin
