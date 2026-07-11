#include "practice/PracticeLaunchRequest.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace {

constexpr const char *kChartSha = "0123456789abcdef0123456789abcdef"
                                  "0123456789abcdef0123456789abcdef";

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

practice::LaunchRequest makeRequest(practice::LaunchSource source) {
  bms_parser::ChartMeta chartMeta;
  chartMeta.SHA256 = kChartSha;
  chartMeta.BmsPath = std::filesystem::path("BMS/example/chart.bms");
  chartMeta.TotalLength = 40'000'000;
  return {
      .chartMeta = std::move(chartMeta),
      .startMicros = 12'000'000,
      .endMicros = 20'000'000,
      .source = source,
  };
}

void testAllResultSourcesValidateWithDurableMetadata() {
  for (const auto source : {practice::LaunchSource::ChartViewer,
                            practice::LaunchSource::NormalResult,
                            practice::LaunchSource::PracticeResult}) {
    const auto request = makeRequest(source);
    require(!practice::validateLaunchRequest(request).has_value(),
            "non-replay launch source accepts durable chart metadata");
  }

  auto replayRequest = makeRequest(practice::LaunchSource::ReplayResult);
  replayRequest.replayId = 42;
  require(!practice::validateLaunchRequest(replayRequest).has_value(),
          "replay launch accepts a durable replay id");
}

void testValidationRejectsUnavailableOrInconsistentMetadata() {
  auto request = makeRequest(practice::LaunchSource::NormalResult);
  request.chartMeta.BmsPath.clear();
  require(practice::validateLaunchRequest(request) ==
              std::optional<std::string>("Chart unavailable"),
          "missing chart path has the required visible status");

  request = makeRequest(practice::LaunchSource::NormalResult);
  request.chartMeta.SHA256 = "not-a-durable-identity";
  require(practice::validateLaunchRequest(request) ==
              std::optional<std::string>("Chart identity unavailable"),
          "invalid chart identity is rejected");

  request = makeRequest(practice::LaunchSource::ReplayResult);
  require(practice::validateLaunchRequest(request) ==
              std::optional<std::string>("Replay unavailable"),
          "replay results require replay provenance");

  request = makeRequest(practice::LaunchSource::NormalResult);
  request.replayId = 42;
  require(practice::validateLaunchRequest(request) ==
              std::optional<std::string>("Unexpected replay metadata"),
          "non-replay launches reject replay-only provenance");

  request = makeRequest(static_cast<practice::LaunchSource>(99));
  require(practice::validateLaunchRequest(request) ==
              std::optional<std::string>("Practice source unavailable"),
          "unknown launch sources are rejected");
}

void testSelectedRangeMergesWithoutChangingLastUsedOptions() {
  practice::Configuration lastUsed;
  lastUsed.chartSha256 = kChartSha;
  lastUsed.startMicros = 1'000'000;
  lastUsed.endMicros = 30'000'000;
  lastUsed.loop = true;
  lastUsed.countInBeats = 8;
  lastUsed.gaugeType = GaugeType::Hard;
  lastUsed.gaugeAutoShift = false;
  lastUsed.startingGaugePercent = 62;
  lastUsed.playback = {
      .percent = 75,
      .mode = audio::PlaybackMode::PitchShift,
  };
  lastUsed.judge = {
      .kind = practice::JudgeOverrideKind::Scale,
      .scalePercent = 80,
  };

  auto request = makeRequest(practice::LaunchSource::ReplayResult);
  request.replayId = 42;
  const auto merged =
      practice::applyLaunchRequest(lastUsed, request, 40'000'000);

  require(merged.startMicros == 12'000'000 && merged.endMicros == 20'000'000,
          "selected analytics range replaces only the stored range");
  require(merged.chartSha256 == lastUsed.chartSha256,
          "per-chart identity is preserved");
  require(merged.playback == lastUsed.playback,
          "last-used playback rate and mode are preserved");
  require(merged.judge == lastUsed.judge,
          "last-used judge override is preserved");
  require(merged.gaugeType == lastUsed.gaugeType &&
              merged.gaugeAutoShift == lastUsed.gaugeAutoShift &&
              merged.startingGaugePercent == lastUsed.startingGaugePercent,
          "last-used gauge settings are preserved");
  require(merged.loop == lastUsed.loop &&
              merged.countInBeats == lastUsed.countInBeats,
          "last-used loop and count-in settings are preserved");
}

void testRangeSanitizesAgainstTheParsedChartEnd() {
  practice::Configuration lastUsed;
  lastUsed.chartSha256 = kChartSha;
  lastUsed.startMicros = 1'000'000;
  lastUsed.endMicros = 10'000'000;
  lastUsed.loop = true;
  lastUsed.countInBeats = 3;

  auto request = makeRequest(practice::LaunchSource::PracticeResult);
  request.startMicros = -5'000'000;
  request.endMicros = 50'000'000;
  const auto merged =
      practice::applyLaunchRequest(lastUsed, request, 25'000'000);

  require(merged.startMicros == 0 && merged.endMicros == 25'000'000,
          "launch range clamps to the actual parsed chart timeline");
  require(merged.loop && merged.countInBeats == 3,
          "sanitizing a launch range preserves valid stored options");
}

} // namespace

int main() {
  testAllResultSourcesValidateWithDurableMetadata();
  testValidationRejectsUnavailableOrInconsistentMetadata();
  testSelectedRangeMergesWithoutChangingLastUsedOptions();
  testRangeSanitizesAgainstTheParsedChartEnd();
  return 0;
}
