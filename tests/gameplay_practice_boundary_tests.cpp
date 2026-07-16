#include "scene/play/GamePlayTiming.h"
#include "scene/play/JudgementTimingText.h"

#include <iostream>
#include <limits>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

bool expectTimingText(long long diffMicros, const std::string &expected,
                      const char *message) {
  return expect(gameplay_timing::formatJudgementTimingMilliseconds(
                    diffMicros) == expected,
                message);
}

bool expectBoundary(long long rawSongTimeMicros, long long audioOffsetMicros,
                    bool expectedComplete) {
  constexpr long long endMicros = 1'000'000;
  const auto frame = gameplay_timing::practiceFrameTiming(
      rawSongTimeMicros, audioOffsetMicros, endMicros);
  const std::vector<long long> noteTimes = {
      endMicros - 1,
      endMicros,
      endMicros + 1,
  };
  std::vector<long long> processed;
  for (const long long noteTime : noteTimes) {
    if (noteTime <= frame.chartTimeMicros) {
      processed.push_back(noteTime);
    }
  }

  return expect(frame.sectionComplete == expectedComplete,
                "completion follows offset-adjusted chart time") &&
         expect(frame.chartTimeMicros == endMicros - 1,
                "boundary processing is capped immediately before end") &&
         expect(processed == std::vector<long long>{endMicros - 1},
                "only the timeline immediately before end is processed");
}

} // namespace

int main() {
  if (!expectTimingText(12'340, "12.34ms",
                        "exact hundredths remain unchanged") ||
      !expectTimingText(12'341, "12.35ms", "partial hundredths round upward") ||
      !expectTimingText(-12'341, "12.35ms",
                        "FAST timing uses the absolute magnitude") ||
      !expectTimingText(1, "0.01ms", "sub-hundredth timing rounds upward") ||
      !expectTimingText(0, "0.00ms", "zero retains two decimal places") ||
      !expectTimingText(std::numeric_limits<long long>::min(),
                        "9223372036854775.81ms",
                        "minimum signed timing formats without overflow")) {
    return 1;
  }

  if (!expect(gameplay_timing::realJudgementDiffMicros(
                  20'000, {.percent = 200}) == 10'000,
              "200 percent HUD timing stays in real milliseconds")) {
    return 1;
  }

  if (!expectBoundary(900'000, 100'000, true) ||
      !expectBoundary(1'100'000, -100'000, true)) {
    return 1;
  }

  constexpr long long endMicros = 1'000'000;
  const auto positiveBefore = gameplay_timing::practiceFrameTiming(
      899'999, 100'000, endMicros);
  const auto negativeBefore = gameplay_timing::practiceFrameTiming(
      1'099'999, -100'000, endMicros);
  if (!expect(!positiveBefore.sectionComplete &&
                  positiveBefore.chartTimeMicros == endMicros - 1,
              "positive offset retains the last valid chart microsecond") ||
      !expect(!negativeBefore.sectionComplete &&
                  negativeBefore.chartTimeMicros == endMicros - 1,
              "negative offset retains the last valid chart microsecond")) {
    return 1;
  }

  return 0;
}
