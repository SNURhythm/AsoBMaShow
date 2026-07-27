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
  replayRequest.replayPlayOptions = practice::ReplayPlayOptions{};
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
  lastUsed.gaugeAutoShift = GaugeAutoShiftMode::None;
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
  request.replayPlayOptions = practice::ReplayPlayOptions{};
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

void testReplayMetadataCannotReplaceTheCurrentPlayedChartIdentityOrPath() {
  bms_parser::ChartMeta current;
  current.SHA256 = std::string(64, 'a');
  current.BmsPath = std::filesystem::path("BMS/current/chart.bms");
  current.Title = "Current parsed chart";

  JudgedPlaybackData staleReplay;
  staleReplay.chartMeta.SHA256 = std::string(64, 'b');
  staleReplay.chartMeta.BmsPath = std::filesystem::path("BMS/stale/chart.bms");
  staleReplay.chartMeta.Title = "Stale replay chart";
  staleReplay.setup.randomSeed = 42;
  staleReplay.setup.randomPrng = "replay-prng";
  staleReplay.setup.randomValues = {2, 1, 3};

  const auto merged =
      practice::mergeReplayLaunchChartMeta(current, staleReplay);
  require(merged.SHA256 == current.SHA256 &&
              merged.BmsPath == current.BmsPath &&
              merged.Title == current.Title,
          "current parsed identity, path, and metadata beat stale replay data");
  require(merged.RandomSeed == staleReplay.setup.randomSeed &&
              merged.RandomPrng == staleReplay.setup.randomPrng &&
              merged.RandomValues == staleReplay.setup.randomValues,
          "missing replay-specific randomization metadata is backfilled");

  staleReplay.chartMeta.BmsPath.clear();
  const auto emptyReplayPath =
      practice::mergeReplayLaunchChartMeta(current, staleReplay);
  require(emptyReplayPath.BmsPath == current.BmsPath &&
              emptyReplayPath.SHA256 == current.SHA256,
          "empty replay path cannot replace the current path or identity");

  current.RandomSeed = 7;
  current.RandomPrng = "current-prng";
  current.RandomValues = {1, 1};
  const auto currentRandomWins =
      practice::mergeReplayLaunchChartMeta(current, staleReplay);
  require(currentRandomWins.RandomSeed == current.RandomSeed &&
              currentRandomWins.RandomPrng == current.RandomPrng &&
              currentRandomWins.RandomValues == current.RandomValues,
          "current parsed randomization metadata remains authoritative");
}

void testParsedChartIdentityGatesLaunchApplication() {
  practice::Configuration lastUsed;
  lastUsed.chartSha256 = std::string(64, 'a');
  lastUsed.startMicros = 1'000'000;
  lastUsed.endMicros = 10'000'000;
  lastUsed.loop = true;
  lastUsed.countInBeats = 8;

  auto request = makeRequest(practice::LaunchSource::NormalResult);
  request.chartMeta.SHA256 = std::string(64, 'A');
  bms_parser::ChartMeta parsed;
  parsed.SHA256 = std::string(64, 'a');

  const auto equivalent = practice::applyLaunchRequestForParsedChart(
      lastUsed, request, parsed, 40'000'000);
  require(equivalent.applied() &&
              equivalent.configuration.startMicros == request.startMicros &&
              equivalent.configuration.endMicros == request.endMicros,
          "case-equivalent request and parsed identities apply the range");

  parsed.SHA256 = std::string(64, 'b');
  const auto conflicting = practice::applyLaunchRequestForParsedChart(
      lastUsed, request, parsed, 40'000'000);
  require(!conflicting.applied() &&
              conflicting.issue ==
                  std::optional<std::string>("Chart identity changed") &&
              conflicting.configuration == lastUsed,
          "conflicting parsed identity rejects without changing configuration");

  parsed.SHA256.clear();
  const auto missing = practice::applyLaunchRequestForParsedChart(
      lastUsed, request, parsed, 40'000'000);
  require(!missing.applied() &&
              missing.issue ==
                  std::optional<std::string>("Chart identity unavailable") &&
              missing.configuration == lastUsed,
          "missing parsed identity rejects without trusting record metadata");
}

void testReplayPlayOptionsAreCarriedIntoSectionLaunch() {
  JudgedPlaybackData replay;
  replay.setup.playOption = "RANDOM";
  replay.setup.playOptionSeed = 1234;
  replay.setup.playOption2 = "MIRROR";
  replay.setup.playOption2Seed = 5678;
  replay.setup.doublePlayOption = replay::DoublePlayOption::Flip;

  const auto options = practice::launchPlayOptionsFromReplay(replay);
  require(options.playOption == replay.setup.playOption &&
              options.playOptionSeed == replay.setup.playOptionSeed &&
              options.playOption2 == replay.setup.playOption2 &&
              options.playOption2Seed == replay.setup.playOption2Seed &&
              options.doublePlayOption == replay::DoublePlayOption::Flip,
          "judged replay section launch retains DP FLIP and both players' "
          "options and seeds");

  replay::ChartPlaybackSetup rawSetup;
  rawSetup.playOption = "R-RANDOM";
  rawSetup.playOptionSeed = 9012;
  rawSetup.playOption2 = "MIRROR";
  rawSetup.playOption2Seed = 3456;
  rawSetup.doublePlayOption = replay::DoublePlayOption::Flip;

  const auto rawOptions = practice::launchPlayOptionsFromReplay(rawSetup);
  require(rawOptions.playOption == rawSetup.playOption &&
              rawOptions.playOptionSeed == rawSetup.playOptionSeed &&
              rawOptions.playOption2 == rawSetup.playOption2 &&
              rawOptions.playOption2Seed == rawSetup.playOption2Seed &&
              rawOptions.doublePlayOption == replay::DoublePlayOption::Flip,
          "raw replay section launch retains DP FLIP and both players' "
          "options");
}

} // namespace

int main() {
  testAllResultSourcesValidateWithDurableMetadata();
  testValidationRejectsUnavailableOrInconsistentMetadata();
  testSelectedRangeMergesWithoutChangingLastUsedOptions();
  testRangeSanitizesAgainstTheParsedChartEnd();
  testReplayMetadataCannotReplaceTheCurrentPlayedChartIdentityOrPath();
  testParsedChartIdentityGatesLaunchApplication();
  testReplayPlayOptionsAreCarriedIntoSectionLaunch();
  return 0;
}
