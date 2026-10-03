#include "../src/audio/ClubBeat.h"
#include "../src/ChartPlaybackDuration.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

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

void testParsedSaturatedStops() {
  for (const auto *stop : {"Infinity", "1e300"}) {
    const std::string input = std::string("#BPM 120\n#STOP01 ") + stop +
                              "\n#00009:01\n#00111:01\n";
    std::vector<unsigned char> bytes(input.begin(), input.end());
    bms_parser::Parser parser;
    bms_parser::Chart *raw = nullptr;
    std::atomic_bool cancelled{false};
    parser.Parse(bytes, &raw, false, false, cancelled);
    std::unique_ptr<bms_parser::Chart> chart(raw);
    require(chart != nullptr, "extreme STOP fixture parses");
    if (chart) {
      require(club_beat::buildPlan(*chart).empty(),
              "unrepresentable STOP rejects the entire club plan");
    }
  }
}

void testInvalidTiming() {
  for (const double bpm : {std::numeric_limits<double>::denorm_min(), 120.0}) {
    bms_parser::Chart chart;
    chart.Meta.Bpm = bpm;
    chart.Measures = {measure(std::numeric_limits<long long>::max() - 10)};
    require(club_beat::buildPlan(chart).empty(),
            "beat conversion and timestamp addition reject overflow");
  }
  for (const double stop : {-48.0, std::numeric_limits<double>::quiet_NaN()}) {
    bms_parser::Chart chart;
    chart.Meta.Bpm = 120;
    auto *first = measure(0);
    auto *timeline = new bms_parser::TimeLine(1, false);
    timeline->Bpm = 120;
    timeline->StopLength = stop;
    first->TimeLines.push_back(timeline);
    chart.Measures.push_back(first);
    require(club_beat::buildPlan(chart).empty(),
            "negative or NaN authored STOP rejects the entire plan");
  }
}

void testBoundedPlanning() {
  bms_parser::Chart chart;
  chart.Meta.Bpm = 120;
  chart.Measures = {measure(0, 1e300)};
  std::atomic_bool cancelled{true};
  require(club_beat::buildPlan(chart, &cancelled).empty(),
          "cancelled extreme chart produces no beats");
  // Run only after the numeric regression passes so the unfixed implementation
  // fails promptly rather than spending unbounded time producing this plan.
  if (failures == 0) {
    require(club_beat::buildPlan(chart).empty(),
            "extreme measure scale rejects before unbounded event production");
  }
}

void testSaturatedGameplayEndHelpers() {
  bms_parser::Chart chart;
  chart.Meta.TotalLength = std::numeric_limits<long long>::max() - 5;
  require(chart_playback_duration::GameplayEndMicros(chart, 10) ==
              std::numeric_limits<long long>::max(),
          "gameplay end helper saturates a timestamp plus positive offset");
  require(chart_playback_duration::GameplayResultTransitionMicros(chart, -1) ==
              std::numeric_limits<long long>::max(),
          "result helper saturates the transition delay and ignores negative grace");
  chart.Meta.TotalLength = 1'000'000;
  require(chart_playback_duration::GameplayResultTransitionMicros(chart, 100) ==
              3'000'100,
          "ordinary gameplay end helper retains grace and transition delays");
}
} // namespace

int main() {
  testFourFourPattern();
  testThreeFourPattern();
  testTempoChangeAndStop();
  testDeterministicBoundedSynthesis();
  testParsedSaturatedStops();
  testInvalidTiming();
  testBoundedPlanning();
  testSaturatedGameplayEndHelpers();
  return failures == 0 ? 0 : 1;
}
