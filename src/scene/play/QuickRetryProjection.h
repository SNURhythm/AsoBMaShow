#pragma once

#include "Judgement.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace quick_retry {
struct Projection {
  std::int64_t hellChargeTicks = 0;
  std::array<int, JudgementCount> pendingClassicJudgements{};

  void acceptClassicHead(Judgement judgement) {
    // Classic tails normalize POOR/empty judgements to BAD.
    if (judgement == None || judgement == Kpoor || judgement == Poor)
      judgement = Bad;
    ++pendingClassicJudgements[judgement];
  }
};

// Allocated once before play. Each changed identity replaces its contribution;
// snapshots copy only the aggregate, never traverse chart-sized collections.
class ProjectionCache {
public:
  void reset(std::size_t count) { entries_.assign(count, {}); projection_ = {}; }
  const Projection &get() const noexcept { return projection_; }
  bool enabled() const noexcept { return !entries_.empty(); }
  void setClassic(std::size_t id, Judgement judgement) {
    if (id >= entries_.size()) return;
    if (judgement == None || judgement == Kpoor || judgement == Poor) judgement = Bad;
    resolveClassic(id);
    entries_[id].classic = judgement;
    ++projection_.pendingClassicJudgements[judgement];
  }
  void resolveClassic(std::size_t id) {
    if (id >= entries_.size() || entries_[id].classic == None) return;
    --projection_.pendingClassicJudgements[entries_[id].classic];
    entries_[id].classic = None;
  }
  void setHellCharge(std::size_t id, std::int64_t ticks) {
    if (id >= entries_.size()) return;
    projection_.hellChargeTicks += ticks - entries_[id].ticks;
    entries_[id].ticks = ticks;
  }
private:
  struct Entry { std::int64_t ticks = 0; Judgement classic = None; };
  std::vector<Entry> entries_;
  Projection projection_;
};

inline std::int64_t remainingHellChargeTicks(std::int64_t start, std::int64_t end,
                                            std::int64_t time, std::int64_t balance) {
  if (end <= time || end <= start) return 0;
  if (time < start) balance = 0;
  // HCN recovery triggers strictly above 200 ms, not at equality.
  return std::max<std::int64_t>(0, (end - std::max(start, time) + balance - 1) / 200'000);
}
} // namespace quick_retry
