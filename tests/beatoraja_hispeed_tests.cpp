#include "scene/play/BeatorajaHiSpeed.h"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void expectNear(float actual, float expected, std::string_view message) {
  expect(std::abs(actual - expected) < 0.0001F, message);
}

constexpr gameplay_hispeed::ChartBpmSummary kChart{
    .start = 120.0,
    .minimum = 90.0,
    .maximum = 240.0,
    .main = 150.0,
};

void verifyFixedModeTransitions() {
  gameplay_hispeed::State state(
      {.mode = gameplay_hispeed::FixMode::Main,
       .durationMilliseconds = 500,
       .hispeed = 1.0F,
       .margin = 0.25F,
       .laneCoverPercent = 20,
       .laneCoverEnabled = true},
      kChart);

  // LaneRenderer.init: setLanecover() resets against MAIN, then captures the
  // resulting value as basehispeed for Start+key increments.
  expectNear(state.hispeed(), 2.56F,
             "MAIN initializes against note-count-weighted BPM and cover");
  expectNear(state.baseHispeed(), 2.56F,
             "fixed mode captures the initialized speed as basehispeed");

  state.changeHispeed(true);
  expectNear(state.hispeed(), 3.20F,
             "fixed manual change uses basehispeed * margin");

  state.setLaneCover(40, 240.0, true);
  expectNear(state.hispeed(), 1.20F,
             "cover change with auto adjust resets against current BPM");

  state.setLaneCoverEnabled(false);
  expectNear(state.hispeed(), 1.20F,
             "cover enable toggle leaves live Hi-Speed untouched");
  const auto disabledCoverDuration = gameplay_hispeed::liveDurationMilliseconds(
      150.0, state.hispeed(), 40, false);
  expect(disabledCoverDuration.has_value() && *disabledCoverDuration == 1333,
         "disabled lane cover removes only cover from current green duration");

  state.setDurationMilliseconds(600);
  expectNear(state.hispeed(), 2.6666667F,
             "duration change resets fixed Hi-Speed against selected BPM");
  state.changeHispeed(true);
  expectNear(state.hispeed(), 3.3066666F,
             "duration change does not overwrite fixed basehispeed");
}

void verifyOffModeTransitions() {
  gameplay_hispeed::State state(
      {.mode = gameplay_hispeed::FixMode::Off,
       .durationMilliseconds = 500,
       .hispeed = 1.50F,
       .margin = 0.25F,
       .laneCoverPercent = 20,
       .laneCoverEnabled = true},
      kChart);

  expectNear(state.hispeed(), 1.50F, "OFF preserves stored raw Hi-Speed");
  state.setLaneCover(40, 240.0, true);
  expectNear(state.hispeed(), 1.50F,
             "OFF ignores cover-triggered fixed-speed resets");
  state.setDurationMilliseconds(600);
  expectNear(state.hispeed(), 1.50F,
             "OFF ignores duration-triggered fixed-speed resets");
  state.changeHispeed(false);
  expectNear(state.hispeed(), 1.25F,
             "OFF manual change uses raw margin rather than basehispeed");
}

void verifyLiveDurationAndGreenNumber() {
  const auto duration = gameplay_hispeed::liveDurationMilliseconds(
      120.0, 6.06F, 0, true, 1.0);
  expect(duration.has_value() && *duration == 330,
         "live duration uses the exact Hi-Speed driving note travel");
  expect(duration.has_value() &&
             gameplay_hispeed::durationToGreenNumber(*duration) == 198,
         "green number is duration * 3 / 5 after live-duration rounding");

  const auto scrolledDuration = gameplay_hispeed::liveDurationMilliseconds(
      120.0, 6.06F, 0, true, 0.5);
  expect(scrolledDuration.has_value() && *scrolledDuration == 660,
         "live duration divides by the current static #SCROLL rate");
}

} // namespace

int main() {
  verifyFixedModeTransitions();
  verifyOffModeTransitions();
  verifyLiveDurationAndGreenNumber();
  if (failures != 0) {
    std::cerr << failures << " Beatoraja Hi-Speed expectation(s) failed\n";
    return 1;
  }
  std::cout << "Beatoraja Hi-Speed tests passed\n";
  return 0;
}
