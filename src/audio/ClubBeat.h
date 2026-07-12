#pragma once

#include "../bms_parser.hpp"

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

[[nodiscard]] std::vector<Event> buildPlan(const bms_parser::Chart &chart);
[[nodiscard]] StereoSound synthesizeKick(int sampleRate);
[[nodiscard]] StereoSound synthesizeClap(int sampleRate);

} // namespace club_beat

