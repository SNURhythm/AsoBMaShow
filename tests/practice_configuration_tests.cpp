#include "audio/PlaybackRate.h"
#include "practice/PracticeConfiguration.h"

#include <iostream>
#include <ranges>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testPlaybackRateConversions() {
  audio::PlaybackRate rate{.percent = 75,
                           .mode = audio::PlaybackMode::PitchShift};
  expect(rate.valid(), "75% pitch-shift playback is valid");
  expect(rate.chartMicrosFromReal(20'000) == 15'000,
         "real time converts to chart time exactly");
  expect(rate.realMicrosFromChart(15'000) == 20'000,
         "chart time converts to real time exactly");
  expect(!audio::PlaybackRate{.percent = 73}.valid(),
         "playback percentages must use five-percent steps");
  expect(!audio::PlaybackRate{.percent = 100,
                              .mode = static_cast<audio::PlaybackMode>(99)}
              .valid(),
         "unknown playback modes are invalid");
}

void testFreshCountInUsesChartMeasureSize() {
  expect(practice::defaultCountInBeatsForChart(3) == 3,
         "fresh 3/4 practice defaults to three count-in beats");
  expect(practice::defaultCountInBeatsForChart(4) == 4,
         "fresh 4/4 practice defaults to four count-in beats");
  expect(practice::defaultCountInBeatsForChart(0) == 4,
         "invalid chart measure size falls back to four beats");
}

void testConfigurationSanitization() {
  practice::Configuration input{
      .chartSha256 = "0123456789abcdef0123456789abcdef"
                     "0123456789abcdef0123456789abcdef",
      .startMicros = 8'000'000,
      .endMicros = 2'000'000,
      .loop = true,
      .countInBeats = 99,
      .startingGaugePercent = 120,
      .judge = {.kind = practice::JudgeOverrideKind::Scale, .scalePercent = 17},
      .playback = {.percent = 73, .mode = audio::PlaybackMode::PitchShift},
  };
  const auto sanitized = practice::sanitize(input, 10'000'000);
  expect(sanitized.configuration.startMicros == 2'000'000,
         "crossed section markers are ordered");
  expect(sanitized.configuration.endMicros == 8'000'000,
         "ordered section retains the later marker");
  expect(sanitized.configuration.countInBeats == 16,
         "count-in is clamped to sixteen beats");
  expect(sanitized.configuration.startingGaugePercent == 100,
         "starting gauge is clamped to one hundred percent");
  expect(sanitized.configuration.judge.scalePercent == 25,
         "judge scale is clamped to twenty-five percent");
  expect(sanitized.configuration.playback.percent == 75,
         "playback rate is rounded to a five-percent step");
  expect(sanitized.playable(), "the sanitized ordered range is playable");
  expect(!sanitized.diagnostics.empty(),
         "configuration adjustments are reported diagnostically");
}

void testGaugeAutoShiftDropdownModel() {
  const auto options = practice::practiceGaugeOptions();
  const auto gas = std::ranges::find(options, std::string_view("gas"),
                                     &practice::GaugeOption::id);
  expect(gas != options.end() && gas->label == "Gauge Auto Shift (GAS)" &&
             gas->gaugeType == GaugeType::ExHard && gas->gaugeAutoShift,
         "practice gauge options expose GAS with a safe nonnumeric id");

  practice::Configuration configuration;
  expect(practice::applyPracticeGaugeOption(configuration, "gas") &&
             configuration.gaugeType == GaugeType::ExHard &&
             configuration.gaugeAutoShift &&
             practice::practiceGaugeOptionId(configuration) == "gas",
         "selecting GAS uses the Ex-Hard seed and enables auto shift");
  expect(practice::applyPracticeGaugeOption(configuration, "2") &&
             configuration.gaugeType == GaugeType::Normal &&
             !configuration.gaugeAutoShift &&
             practice::practiceGaugeOptionId(configuration) == "2",
         "selecting a concrete gauge disables auto shift");
  expect(!practice::applyPracticeGaugeOption(configuration, "not-a-gauge"),
         "unknown nonnumeric gauge ids are rejected without parsing");
}

void testGaugeAutoShiftSanitizationUsesExHardSeed() {
  practice::Configuration configuration{
      .chartSha256 = "0123456789abcdef0123456789abcdef"
                     "0123456789abcdef0123456789abcdef",
      .startMicros = 0,
      .endMicros = 1'000'000,
      .gaugeType = GaugeType::Normal,
      .gaugeAutoShift = true,
  };
  const auto sanitized = practice::sanitize(configuration, 1'000'000);
  expect(sanitized.configuration.gaugeType == GaugeType::ExHard &&
             sanitized.configuration.gaugeAutoShift,
         "sanitization canonicalizes GAS to the Ex-Hard seed");
}

void testEmptyConfigurationIsNotPlayable() {
  practice::Configuration input{.startMicros = -10, .endMicros = 2'000'000};
  const auto sanitized = practice::sanitize(input, 0);
  expect(sanitized.configuration.startMicros == 0 &&
             sanitized.configuration.endMicros == 0,
         "markers clamp to an empty chart");
  expect(!sanitized.playable(), "an empty section cannot start practice");
}

void testPlayabilityIssuesExplainBlockingConfiguration() {
  practice::Configuration input{
      .chartSha256 = "0123456789abcdef0123456789abcdef"
                     "0123456789abcdef0123456789abcdef",
      .startMicros = 1'000'000,
      .endMicros = 5'000'000,
  };
  expect(!practice::firstPlayabilityIssue(input, 8'000'000),
         "a valid practice configuration has no blocking issue");

  const std::string validHash = input.chartSha256;
  input.chartSha256 = "invalid";
  expect(practice::firstPlayabilityIssue(input, 8'000'000) ==
             "Chart SHA-256 is unavailable or invalid.",
         "an invalid chart hash receives an identity explanation");
  input.chartSha256 = validHash;

  input.gaugeType = static_cast<GaugeType>(99);
  expect(practice::firstPlayabilityIssue(input, 8'000'000) ==
             "Gauge selection is invalid.",
         "an invalid gauge receives a selection explanation");
  input.gaugeType = GaugeType::Normal;

  input.endMicros = input.startMicros;
  expect(practice::firstPlayabilityIssue(input, 8'000'000) ==
             "Practice range must be non-empty.",
         "an empty range receives an actionable explanation");
  input.endMicros = 5'000'000;
  input.judge.kind = practice::JudgeOverrideKind::Custom;
  expect(practice::firstPlayabilityIssue(input, 8'000'000) ==
             "Custom judge windows are not available.",
         "custom judge windows receive an availability explanation");
  input.judge.kind = practice::JudgeOverrideKind::Scale;
  input.playback.mode = audio::PlaybackMode::TimeStretch;
  expect(practice::firstPlayabilityIssue(input, 8'000'000) ==
             "Time Stretch is not available.",
         "time stretch receives an availability explanation");
  input.playback.mode = audio::PlaybackMode::PitchShift;
  input.playback.percent = 73;
  expect(practice::firstPlayabilityIssue(input, 8'000'000) ==
             "Playback rate must be 50-200% in 5% steps.",
         "invalid playback rate receives a bounds explanation");
}
} // namespace

int main() {
  testPlaybackRateConversions();
  testFreshCountInUsesChartMeasureSize();
  testConfigurationSanitization();
  testGaugeAutoShiftDropdownModel();
  testGaugeAutoShiftSanitizationUsesExHardSeed();
  testEmptyConfigurationIsNotPlayable();
  testPlayabilityIssuesExplainBlockingConfiguration();
  if (failures == 0) {
    std::cout << "practice configuration tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
