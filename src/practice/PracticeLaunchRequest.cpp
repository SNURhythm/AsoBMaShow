#include "PracticeLaunchRequest.h"

#include "../CanonicalDigest.h"
#include "../Uuid.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace practice {
namespace {

bool isKnownSource(LaunchSource source) {
  switch (source) {
  case LaunchSource::ChartViewer:
  case LaunchSource::NormalResult:
  case LaunchSource::PracticeResult:
  case LaunchSource::ReplayResult:
    return true;
  }
  return false;
}

std::string normalizedSha256(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

bool isSha256(std::string_view value) {
  return canonical_digest::isCanonicalLowerHex(
      normalizedSha256(std::string(value)), 64);
}

} // namespace

bms_parser::ChartMeta
mergeReplayLaunchChartMeta(const bms_parser::ChartMeta &authoritative,
                           const ReplayData &replay) {
  bms_parser::ChartMeta result = authoritative;
  if (!result.RandomSeed.has_value()) {
    result.RandomSeed = replay.randomSeed;
  }
  if (!result.RandomPrng.has_value()) {
    result.RandomPrng = replay.randomPrng;
  }
  if (result.RandomValues.empty()) {
    result.RandomValues = replay.randomValues;
  }
  return result;
}

ReplayPlayOptions launchPlayOptionsFromReplay(const ReplayData &replay) {
  return {
      .playOption = replay.playOption,
      .playOptionSeed = replay.playOptionSeed,
      .playOption2 = replay.playOption2,
      .playOption2Seed = replay.playOption2Seed,
  };
}

std::optional<std::string> validateLaunchRequest(const LaunchRequest &request) {
  if (!isKnownSource(request.source)) {
    return "Practice source unavailable";
  }
  if (request.chartMeta.BmsPath.empty()) {
    return "Chart unavailable";
  }
  if (!isSha256(request.chartMeta.SHA256)) {
    return "Chart identity unavailable";
  }
  if (request.startMicros >= request.endMicros) {
    return "Practice range unavailable";
  }
  if (request.source == LaunchSource::ReplayResult) {
    const bool validLegacy =
        request.replayId.has_value() && *request.replayId > 0;
    const bool validModern = request.modernReplayAttemptId.has_value() &&
                             uuid::isCanonicalLowerV4(
                                 *request.modernReplayAttemptId);
    if (request.replayId.has_value() &&
        request.modernReplayAttemptId.has_value()) {
      return "Replay identity is ambiguous";
    }
    if (!validLegacy && !validModern) {
      return "Replay unavailable";
    }
    if (!request.replayPlayOptions.has_value()) {
      return "Replay options unavailable";
    }
  } else if (request.replayId.has_value() ||
             request.modernReplayAttemptId.has_value() ||
             request.replayPlayOptions.has_value()) {
    return "Unexpected replay metadata";
  }
  return std::nullopt;
}

Configuration applyLaunchRequest(const Configuration &lastUsed,
                                 const LaunchRequest &request,
                                 long long chartEndMicros) {
  Configuration merged = lastUsed;
  merged.startMicros = request.startMicros;
  merged.endMicros = request.endMicros;
  return sanitize(std::move(merged), chartEndMicros).configuration;
}

ParsedLaunchApplication applyLaunchRequestForParsedChart(
    const Configuration &lastUsed, const LaunchRequest &request,
    const bms_parser::ChartMeta &parsedChartMeta, long long chartEndMicros) {
  if (const auto issue = validateLaunchRequest(request); issue.has_value()) {
    return {.configuration = lastUsed, .issue = issue};
  }
  if (!isSha256(parsedChartMeta.SHA256)) {
    return {.configuration = lastUsed, .issue = "Chart identity unavailable"};
  }
  if (normalizedSha256(request.chartMeta.SHA256) !=
      normalizedSha256(parsedChartMeta.SHA256)) {
    return {.configuration = lastUsed, .issue = "Chart identity changed"};
  }
  return {.configuration =
              applyLaunchRequest(lastUsed, request, chartEndMicros)};
}

} // namespace practice
