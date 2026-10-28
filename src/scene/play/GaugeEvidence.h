#pragma once
#include "GameplayGaugeTypes.h"
#include <array>
#include <cmath>
#include <cstdint>

// Unknown historical evidence is all zero. Recorded survival gauges use -1
// until their first death, then retain that chart's passed-note count.
struct GaugeEvidence {
  bool recorded = false;
  std::uint16_t survivalMask = 0;
  int passedNotes = 0;
  std::array<int, kGaugeTypeCount> deathPassedNotes{};
  std::array<float, kGaugeTypeCount> values{};
  bool operator==(const GaugeEvidence &) const = default;

  bool valid(int totalNotes) const noexcept {
    if (!recorded) return *this == GaugeEvidence{};
    if (passedNotes < 0 || passedNotes > totalNotes || survivalMask >= (1u << kGaugeTypeCount)) return false;
    for (std::size_t i = 0; i < kGaugeTypeCount; ++i) {
      if (!std::isfinite(values[i]) || values[i] < 0 || values[i] > 120) return false;
      if (survivalMask & (1u << i)) {
        if (deathPassedNotes[i] < -1 || deathPassedNotes[i] > passedNotes) return false;
      } else if (deathPassedNotes[i] != 0) return false;
    }
    return true;
  }
};
