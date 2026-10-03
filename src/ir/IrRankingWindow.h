#pragma once

#include "IrRankingModels.h"

#include <algorithm>
#include <span>

namespace ir {

inline int nearbyRankingOffset(const std::vector<IrChartRankingEntry> &entries) {
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (entries[index].currentUser) {
      return std::max(0, entries[index].rank - 1 - static_cast<int>(index));
    }
  }
  return 0;
}

// Nearby rows are a separate window, not a dense prefix. Preserve the server's
// ranks, including ties and gaps. At the edge of a sparse window show the
// available neighbors without manufacturing rows for the missing positions.
// Explicit navigation prefers loaded prefix rows; an automatically centered
// own-rank view prefers neighbors, even when ties place own rank in the prefix.
template <class Entry>
std::span<const Entry> rankingWindow(const std::vector<Entry> &prefix,
                                    const std::vector<Entry> &nearby,
                                    int nearbyOffset, int offset,
                                    int count = 10, bool preferNearby = false) {
  if (offset < 0 || count <= 0) return {};
  const std::vector<Entry> *source = &prefix;
  std::size_t begin = static_cast<std::size_t>(offset);
  if ((preferNearby || begin >= prefix.size()) && !nearby.empty() &&
      static_cast<std::int64_t>(offset) + count > nearbyOffset &&
      static_cast<std::int64_t>(offset) < nearbyOffset +
          static_cast<std::int64_t>(nearby.size())) {
    source = &nearby;
    begin = static_cast<std::size_t>(std::max(0, offset - nearbyOffset));
  }
  if (begin >= source->size()) return {};
  return std::span<const Entry>(*source).subspan(
      begin, std::min(static_cast<std::size_t>(count), source->size() - begin));
}

} // namespace ir
