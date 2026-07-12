#include "../src/audio/ClubBeat.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bms_parser::Measure *measure(long long timing, double scale = 1.0) {
  auto *result = new bms_parser::Measure();
  result->Timing = timing;
  result->Scale = scale;
  return result;
}

void testFourFourPattern() {
  bms_parser::Chart chart;
  chart.Meta.Bpm = 120.0;
  chart.Measures = {measure(0), measure(2000000)};

  const auto plan = club_beat::buildPlan(chart);
  require(plan.size() == 8, "two 4/4 measures produce eight beats");
  const long long expected[] = {0, 500000, 1000000, 1500000,
                                2000000, 2500000, 3000000, 3500000};
  for (std::size_t index = 0; index < plan.size(); ++index) {
    require(plan[index].timeMicros == expected[index],
            "120 BPM beat lands on the quarter-note grid");
    require(plan[index].beatInMeasure == static_cast<int>(index % 4) + 1,
            "beat numbering resets at each measure");
    require(plan[index].kick, "every beat contains a kick");
    require(plan[index].clap ==
                (plan[index].beatInMeasure == 2 ||
                 plan[index].beatInMeasure == 4),
            "only beats two and four contain claps");
  }
}

void testThreeFourPattern() {
  bms_parser::Chart chart;
  chart.Meta.Bpm = 120.0;
  chart.Measures = {measure(0, 0.75), measure(1500000, 0.75)};

  const auto plan = club_beat::buildPlan(chart);
  require(plan.size() == 6, "two 3/4 measures produce six beats");
  require(plan[2].beatInMeasure == 3 && !plan[2].clap,
          "third beat in 3/4 has no clap");
  require(plan[3].timeMicros == 1500000 &&
              plan[3].beatInMeasure == 1,
          "next 3/4 measure resets at its parsed barline");
}

void testTempoChangeAndStop() {
  bms_parser::Chart chart;
  chart.Meta.Bpm = 120.0;
  auto *first = measure(0);
  auto *change = new bms_parser::TimeLine(1, false);
  change->Timing = 500000;
  change->BeatPosition = 0.25;
  change->BpmChange = true;
  change->Bpm = 60.0;
  change->StopLength = 48.0;
  first->TimeLines.push_back(change);
  chart.Measures = {first, measure(4500000)};

  const auto plan = club_beat::buildPlan(chart);
  require(plan.size() >= 4, "tempo/stop chart produces its first measure");
  require(plan[0].timeMicros == 0 && plan[1].timeMicros == 500000,
          "tempo-change beat retains parsed timing");
  require(plan[2].timeMicros == 2500000 &&
              plan[3].timeMicros == 3500000,
          "later beats include stop duration and changed BPM");
}

void testDeterministicBoundedSynthesis() {
  const auto kick = club_beat::synthesizeKick(44100);
  const auto clapA = club_beat::synthesizeClap(44100);
  const auto clapB = club_beat::synthesizeClap(44100);
  require(kick.sampleRate == 44100 && !kick.samples.empty(),
          "kick synthesis produces 44.1 kHz samples");
  require(clapA.sampleRate == 44100 && !clapA.samples.empty(),
          "clap synthesis produces 44.1 kHz samples");
  require(clapA.samples == clapB.samples, "clap synthesis is deterministic");
  const auto bounded = [](const club_beat::StereoSound &sound) {
    return std::ranges::all_of(sound.samples, [](float sample) {
      return std::isfinite(sample) && sample >= -1.0f && sample <= 1.0f;
    });
  };
  require(bounded(kick) && bounded(clapA),
          "synthetic PCM is finite and normalized");
  require(kick.samples.size() % 2 == 0 && clapA.samples.size() % 2 == 0,
          "synthetic PCM is stereo interleaved");
}
} // namespace

int main() {
  testFourFourPattern();
  testThreeFourPattern();
  testTempoChangeAndStop();
  testDeterministicBoundedSynthesis();
  return failures == 0 ? 0 : 1;
}
