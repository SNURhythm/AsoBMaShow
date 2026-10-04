#pragma once

#include "../bms_parser.hpp"

#include <atomic>
#include <vector>

namespace club_beat {

struct Event {
  long long timeMicros = 0;
  int beatInMeasure = 1;
  bool kick = true;
  bool clap = false;
};

struct StereoSound {
  int sampleRate = 44100;
  std::vector<float> samples;
};

enum class PlanError { None, Cancelled, InvalidTiming, WorkLimit };

// No partial plan is returned on invalid timing, cancellation, or excessive
// expansion. One million beats bounds memory/work even for hostile scales.
inline constexpr std::size_t kMaxPlanEvents = 1'000'000;

[[nodiscard]] std::vector<Event> buildPlan(const bms_parser::Chart &chart,
                                         const std::atomic_bool *cancelled = nullptr,
                                         PlanError *error = nullptr);
[[nodiscard]] StereoSound synthesizeKick(int sampleRate);
[[nodiscard]] StereoSound synthesizeClap(int sampleRate);

} // namespace club_beat
