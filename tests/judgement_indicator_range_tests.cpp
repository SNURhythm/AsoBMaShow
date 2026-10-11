#include "../src/JudgementIndicatorRange.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void expectNear(float actual, float expected, const std::string &message) {
  expect(std::abs(actual - expected) < 0.0001f, message);
}

void testRangeRules() {
  using namespace judgement_indicator;
  expect(sanitizeStoredRangeMilliseconds(-1) == 180,
         "negative stored range uses the default");
  expect(sanitizeStoredRangeMilliseconds(0) == 180,
         "zero stored range uses the default");
  expect(sanitizeStoredRangeMilliseconds(1) == 1,
         "one millisecond is accepted");
  expect(sanitizeStoredRangeMilliseconds(1000) == 1000,
         "the hard cap is accepted");
  expect(sanitizeStoredRangeMilliseconds(1001) == 1000,
         "stored range is capped at 1000 ms");
  expect(clampEditableRangeMilliseconds(-10) == 1,
         "interactive edits clamp to one millisecond");
  expect(clampEditableRangeMilliseconds(1001) == 1000,
         "interactive edits clamp to the hard cap");
  expect(rangeMicros(180) == 180000,
         "milliseconds convert to microseconds");
  expect(rangeMicros(0) == 180000,
         "invalid renderer input uses the default range");
  expect(formatRangeLabel(180) == "+/-180 ms",
         "range label displays a symmetric extent");
}

void testPositionMapping() {
  using namespace judgement_indicator;
  expectNear(normalizedOffset(0, 180000), 0.0f, "zero remains centered");
  expectNear(normalizedOffset(-180000, 180000), -1.0f,
             "early boundary maps left");
  expectNear(normalizedOffset(180000, 180000), 1.0f,
             "late boundary maps right");
  expectNear(normalizedOffset(-300000, 180000), -1.0f,
             "early outlier clamps left");
  expectNear(normalizedOffset(300000, 180000), 1.0f,
             "late outlier clamps right");
}

void testRawAverageThenPositionClamp() {
  using namespace judgement_indicator;
  ExponentialAverageAccumulator average;
  average.add(300000, 0);
  average.add(0, 0);
  expect(average.hasSamples(), "average tracks included samples");
  expect(average.value() == 150000,
         "average uses raw values before display clamping");
  expectNear(normalizedOffset(average.value(), 180000), 5.0f / 6.0f,
             "raw average maps inside the configured range");
  average.add(300000, 0);
  expect(average.value() == 200000,
         "raw average can exceed the configured range");
  expectNear(normalizedOffset(average.value(), 180000), 1.0f,
             "only the final average marker position clamps");
}

void testContinuousExponentialAverage() {
  judgement_indicator::ExponentialAverageAccumulator average;
  average.add(-60'000, 0);
  average.add(60'000, 1'800'000);
  expect(average.value() == 20'000,
         "one half-life gives the older hit half the newer hit's weight");
  average.add(-60'000, 0);
  expect(average.value() == 0,
         "out-of-order samples receive their timestamp's exponential weight");
  average.add(15'000, 10'000'000'000LL);
  expect(average.value() == 15'000 &&
             average.latestSampleTimeMicros() == 10'000'000'000LL,
         "a new hit after a long gap remains finite when old weights underflow");
}

void testTimeFilteredAverageHistory() {
  judgement_indicator::TimingAverageHistory history;
  expect(!history.at(0).hasSamples(), "empty history has no average");
  history.add(-60'000, 0);
  history.add(60'000, 1'800'000);
  const auto captured = history;
  history.add(-60'000, 0);
  expect(history.at(0).value() == -60'000 &&
             history.at(1'800'000).value() == 0,
         "filtering preserves earlier inputs on both sides of a future input");
  expect(!history.at(-1).hasSamples(),
         "a frame before every input has no average");
  expect(captured.at(0).value() == -60'000 &&
             captured.at(1'800'000).value() == 20'000,
         "later appends cannot alter a captured frame's filtered or full average");

  auto replayed = captured;
  replayed.add(-60'000, 0);
  expect(replayed.at(1'800'000).value() == 0,
         "replaying a captured prefix includes each input exactly once");
  auto branched = captured;
  branched.add(60'000, 0);
  expect(branched.at(0).value() == 0 &&
             branched.at(1'800'000).value() == 30'000 &&
             history.at(1'800'000).value() == 0 &&
             captured.at(1'800'000).value() == 20'000,
         "a divergent timeline preserves the original history and earlier snapshots");
  history = {};
  expect(!history.at(1'800'000).hasSamples() &&
             captured.at(1'800'000).value() == 20'000,
         "reset clears the active history while retained frames remain immutable");
}

void testSegmentClipping() {
  using namespace judgement_indicator;
  const auto partial = clipSegment(-200000, -120000, 180000);
  expect(partial.has_value(), "partially visible BAD segment remains");
  expect(partial && partial->startMicros == -180000 &&
             partial->endMicros == -120000,
         "partially visible segment clips at the bar edge");
  expect(!clipSegment(-400000, -200000, 180000).has_value(),
         "fully hidden early segment is omitted");
  expect(!clipSegment(200000, 400000, 180000).has_value(),
         "fully hidden late segment is omitted");
}
} // namespace

int main() {
  testRangeRules();
  testPositionMapping();
  testRawAverageThenPositionClamp();
  testContinuousExponentialAverage();
  testTimeFilteredAverageHistory();
  testSegmentClipping();
  if (failures != 0) {
    std::cerr << failures << " judgement indicator range assertion(s) failed\n";
    return 1;
  }
  std::cout << "judgement indicator range tests passed\n";
  return 0;
}
