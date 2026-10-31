#pragma once

#include "Judgement.h"
#include <algorithm>
#include <array>
#include <cstdint>

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

inline std::int64_t remainingHellChargeTicks(std::int64_t start, std::int64_t end,
                                            std::int64_t time, std::int64_t balance) {
  if (end <= time || end <= start) return 0;
  if (time < start) balance = 0;
  // HCN recovery triggers strictly above 200 ms, not at equality.
  return std::max<std::int64_t>(0, (end - std::max(start, time) + balance - 1) / 200'000);
}
} // namespace quick_retry
