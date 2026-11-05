#pragma once

#include "GameplayGaugeTypes.h"
#include "Judgement.h"

#include <array>
#include <cstdint>
#include <limits>

namespace completion_celebration {
enum class Achievement : std::uint8_t { None, FullCombo, Perfect, Finish, Max };

struct State {
  Achievement achievement = Achievement::None;
  long long startMicros = std::numeric_limits<long long>::min();

  bool operator==(const State &) const = default;
};

// Feed committed scoring judgements, not input edges. The worker owns an
// unbroken stream; replay and legacy presentation use the same reducer.
class Tracker {
public:
  void reset(int totalNotes) noexcept {
    totalNotes_ = totalNotes;
    counts_ = {};
    state_ = {};
  }
  void observe(Judgement judgement, int combo, long long timeMicros,
               bool requireSurvivingCombo) noexcept {
    if (state_.achievement != Achievement::None || judgement == None) return;
    switch (judgement) {
    case PGreat: ++counts_[0]; break;
    case Great: ++counts_[1]; break;
    case Good: ++counts_[2]; break;
    case Bad: ++counts_[3]; break;
    case Poor: ++counts_[4]; break;
    default: break;
    }
    const int rank = clear_policy::completedClearRank(
        kClearTypeAssistedEasyClearRank, totalNotes_, counts_[0], counts_[1],
        counts_[2], counts_[3], counts_[4], 0);
    if (rank >= kClearTypeFullComboRank &&
        (!requireSurvivingCombo || combo >= totalNotes_)) {
      state_ = {rank >= kClearTypeMaxRank ? Achievement::Max :
                    rank >= kClearTypePerfectRank ? Achievement::Perfect : Achievement::FullCombo,
                timeMicros};
    }
  }
  [[nodiscard]] State state() const noexcept { return state_; }
  // Progress counts resolved identities, not judgement samples: PMS BAD can
  // leave a note alive and its later miss can carry no second judgement.
  void observeResolvedNotes(int passedNotes, long long timeMicros) noexcept {
    if (state_.achievement == Achievement::None && totalNotes_ > 0 &&
        passedNotes >= totalNotes_) state_ = {Achievement::Finish, timeMicros};
  }

private:
  int totalNotes_ = 0;
  std::array<int, 5> counts_{};
  State state_;
};
} // namespace completion_celebration
