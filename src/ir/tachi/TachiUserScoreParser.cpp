#include "TachiUserScoreParser.h"

#include "../../CanonicalDigest.h"
#include "../../ResultContracts.h"
#include "../../scene/play/GameplayGaugeTypes.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ir::tachi {
namespace {

using Json = nlohmann::json;

struct ParsedSong {
  std::string title;
  std::string artist;
};

struct ParsedChart {
  std::string songId;
  std::string md5;
  std::string sha256;
  std::string title;
  std::string artist;
  std::string difficulty;
  std::string level;
  double levelNumber = 0.0;
  int noteCount = 0;
};

using SongMap = std::map<std::string, ParsedSong, std::less<>>;
using ChartMap = std::map<std::string, ParsedChart, std::less<>>;

IrUserScoreSnapshotOutcome failure(IrUserScoreSnapshotStatus status,
                                   std::string_view code,
                                   std::string_view diagnostic) {
  IrUserScoreSnapshotOutcome outcome;
  outcome.status = status;
  outcome.code.assign(code.substr(0, kMaximumIrRemoteScoreDiagnosticBytes));
  outcome.diagnostic.assign(
      diagnostic.substr(0, kMaximumIrRemoteScoreDiagnosticBytes));
  return outcome;
}

IrUserScoreSnapshotOutcome malformed(std::string_view diagnostic) {
  return failure(IrUserScoreSnapshotStatus::MalformedResponse,
                 "malformed_response", diagnostic);
}

IrUserScoreSnapshotOutcome succeeded(IrUserScoreSnapshot snapshot = {}) {
  IrUserScoreSnapshotOutcome outcome;
  outcome.status = IrUserScoreSnapshotStatus::Succeeded;
  outcome.snapshot = std::move(snapshot);
  return outcome;
}

bool isExpectedGame(std::string_view game) {
  return game == "bms-7k" || game == "bms-14k";
}

bool hasSafeBytes(std::string_view value, std::size_t maximumBytes,
                  bool requireValue = false) {
  return (!requireValue || !value.empty()) && value.size() <= maximumBytes &&
         std::ranges::none_of(value, [](unsigned char character) {
           return character < 0x20U || character == 0x7fU;
         });
}

std::optional<std::string> requiredString(const Json &object,
                                          std::string_view key,
                                          std::size_t maximumBytes,
                                          bool requireValue = false) {
  const auto value = object.find(key);
  if (value == object.end() || !value->is_string()) {
    return std::nullopt;
  }
  const auto &text = value->get_ref<const std::string &>();
  if (!hasSafeBytes(text, maximumBytes, requireValue)) {
    return std::nullopt;
  }
  return text;
}

std::optional<std::int64_t> jsonInteger(const Json &value) {
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(number);
  }
  if (!value.is_number_integer()) {
    return std::nullopt;
  }
  return value.get<std::int64_t>();
}

std::optional<std::int64_t> requiredInteger(const Json &object,
                                            std::string_view key) {
  const auto value = object.find(key);
  return value == object.end() ? std::nullopt : jsonInteger(*value);
}

std::optional<double> requiredFiniteNumber(const Json &object,
                                           std::string_view key) {
  const auto value = object.find(key);
  if (value == object.end() || !value->is_number()) {
    return std::nullopt;
  }
  const double number = value->get<double>();
  return std::isfinite(number) ? std::optional<double>(number) : std::nullopt;
}

bool requiredTrue(const Json &object, std::string_view key) {
  const auto value = object.find(key);
  return value != object.end() && value->is_boolean() && value->get<bool>();
}

std::optional<int> mapBmsLamp(std::string_view lamp) {
  if (lamp == "NO PLAY" || lamp == "FAILED") {
    return kClearTypeFailedRank;
  }
  if (lamp == "ASSIST CLEAR") {
    return kClearTypeAssistedEasyClearRank;
  }
  if (lamp == "EASY CLEAR") {
    return kClearTypeEasyClearRank;
  }
  if (lamp == "CLEAR") {
    return kClearTypeNormalClearRank;
  }
  if (lamp == "HARD CLEAR") {
    return kClearTypeHardClearRank;
  }
  if (lamp == "EX HARD CLEAR") {
    return kClearTypeExHardClearRank;
  }
  if (lamp == "FULL COMBO") {
    return kClearTypeFullComboRank;
  }
  return std::nullopt;
}

bool readOptionalNonnegativeInt(const Json &object, std::string_view key,
                                std::optional<int> &result) {
  const auto value = object.find(key);
  if (value == object.end() || value->is_null()) {
    result.reset();
    return true;
  }
  const auto parsed = jsonInteger(*value);
  if (!parsed || *parsed < 0 || *parsed > std::numeric_limits<int>::max()) {
    return false;
  }
  result = static_cast<int>(*parsed);
  return true;
}

bool readOptionalGauge(const Json &object, std::string_view key,
                       std::optional<float> &result) {
  const auto value = object.find(key);
  if (value == object.end() || value->is_null()) {
    result.reset();
    return true;
  }
  if (!value->is_number()) {
    return false;
  }
  const double number = value->get<double>();
  if (!std::isfinite(number) || number < 0.0 || number > 100.0) {
    return false;
  }
  result = static_cast<float>(number);
  return true;
}

bool readGaugeHistory(const Json &optional, IrRemoteScore &score) {
  const auto value = optional.find("gaugeHistory");
  if (value == optional.end() || value->is_null()) {
    score.gaugeHistory.clear();
    return true;
  }
  if (!value->is_array() ||
      value->size() > kMaximumIrRemoteGaugeHistoryEntries) {
    return false;
  }
  score.gaugeHistory.clear();
  score.gaugeHistory.reserve(value->size());
  for (const auto &point : *value) {
    if (point.is_null()) {
      score.gaugeHistory.emplace_back(std::nullopt);
      continue;
    }
    if (!point.is_number()) {
      return false;
    }
    const double number = point.get<double>();
    if (!std::isfinite(number) || number < 0.0 || number > 100.0) {
      return false;
    }
    score.gaugeHistory.emplace_back(static_cast<float>(number));
  }
  return true;
}

template <std::size_t Size>
bool isAllowed(std::string_view value,
               const std::array<std::string_view, Size> &allowed) {
  return std::ranges::find(allowed, value) != allowed.end();
}

bool readOptionalEnumString(const Json &object, std::string_view key,
                            const auto &allowed,
                            std::optional<std::string> &result) {
  const auto value = object.find(key);
  if (value == object.end() || value->is_null()) {
    result.reset();
    return true;
  }
  if (!value->is_string()) {
    return false;
  }
  const auto &text = value->get_ref<const std::string &>();
  if (!isAllowed(text, allowed)) {
    return false;
  }
  result = text;
  return true;
}

bool readRandom(const Json &scoreMeta, std::string_view game,
                std::optional<std::string> &result) {
  constexpr std::array allowedRandom{
      std::string_view("MIRROR"), std::string_view("NONRAN"),
      std::string_view("R-RANDOM"), std::string_view("RANDOM"),
      std::string_view("S-RANDOM")};
  const auto value = scoreMeta.find("random");
  if (value == scoreMeta.end() || value->is_null()) {
    result.reset();
    return true;
  }
  if (game == "bms-7k") {
    if (!value->is_string()) {
      return false;
    }
    const auto &random = value->get_ref<const std::string &>();
    if (!isAllowed(random, allowedRandom)) {
      return false;
    }
    result = random;
    return true;
  }
  if (!value->is_array() || value->size() != 2 || !(*value)[0].is_string() ||
      !(*value)[1].is_string()) {
    return false;
  }
  const auto &left = (*value)[0].get_ref<const std::string &>();
  const auto &right = (*value)[1].get_ref<const std::string &>();
  if (!isAllowed(left, allowedRandom) || !isAllowed(right, allowedRandom)) {
    return false;
  }
  result = left + " / " + right;
  return true;
}

std::optional<SongMap> parseSongs(const Json &songs) {
  if (!songs.is_array() ||
      songs.size() > kMaximumIrRemoteScoreSnapshotEntries) {
    return std::nullopt;
  }
  SongMap parsed;
  for (const auto &song : songs) {
    if (!song.is_object()) {
      return std::nullopt;
    }
    auto id = requiredString(song, "id", kMaximumIrRemoteScoreIdBytes, true);
    auto title = requiredString(song, "title", kMaximumIrRemoteScoreTextBytes);
    auto artist =
        requiredString(song, "artist", kMaximumIrRemoteScoreTextBytes);
    if (!id || !title || !artist ||
        !parsed
             .emplace(std::move(*id),
                      ParsedSong{std::move(*title), std::move(*artist)})
             .second) {
      return std::nullopt;
    }
  }
  return parsed;
}

std::optional<ChartMap> parseCharts(const Json &charts,
                                    std::string_view expectedGame,
                                    const SongMap &songs) {
  if (!charts.is_array() ||
      charts.size() > kMaximumIrRemoteScoreSnapshotEntries) {
    return std::nullopt;
  }
  ChartMap parsed;
  for (const auto &chart : charts) {
    if (!chart.is_object() || !requiredTrue(chart, "isPrimary")) {
      return std::nullopt;
    }
    auto id =
        requiredString(chart, "chartID", kMaximumIrRemoteScoreIdBytes, true);
    auto game =
        requiredString(chart, "game", kMaximumIrRemoteScoreTextBytes, true);
    auto difficulty =
        requiredString(chart, "difficulty", kMaximumIrRemoteScoreTextBytes);
    auto level = requiredString(chart, "level", kMaximumIrRemoteScoreTextBytes);
    const auto levelNumber = requiredFiniteNumber(chart, "levelNum");
    const auto song = chart.find("song");
    const auto data = chart.find("data");
    if (!id || !game || *game != expectedGame || !difficulty || !level ||
        !levelNumber || *levelNumber < 0.0 || song == chart.end() ||
        !song->is_object() || data == chart.end() || !data->is_object()) {
      return std::nullopt;
    }
    auto songId =
        requiredString(*song, "id", kMaximumIrRemoteScoreIdBytes, true);
    auto embeddedTitle =
        requiredString(*song, "title", kMaximumIrRemoteScoreTextBytes);
    auto embeddedArtist =
        requiredString(*song, "artist", kMaximumIrRemoteScoreTextBytes);
    const auto noteCount = requiredInteger(*data, "notecount");
    auto md5 =
        requiredString(*data, "hashMD5", kMaximumIrRemoteScoreIdBytes, true);
    auto sha256 =
        requiredString(*data, "hashSHA256", kMaximumIrRemoteScoreIdBytes, true);
    if (!songId || !embeddedTitle || !embeddedArtist || !noteCount ||
        !result_contract::maximumScoreForNotes(*noteCount) ||
        !md5 || !sha256 ||
        !canonical_digest::isCanonicalLowerHex(*md5, 32) ||
        !canonical_digest::isCanonicalLowerHex(*sha256, 64)) {
      return std::nullopt;
    }
    const auto relatedSong = songs.find(*songId);
    if (relatedSong == songs.end() ||
        relatedSong->second.title != *embeddedTitle ||
        relatedSong->second.artist != *embeddedArtist) {
      return std::nullopt;
    }
    ParsedChart parsedChart{
        .songId = std::move(*songId),
        .md5 = std::move(*md5),
        .sha256 = std::move(*sha256),
        .title = relatedSong->second.title,
        .artist = relatedSong->second.artist,
        .difficulty = std::move(*difficulty),
        .level = std::move(*level),
        .levelNumber = *levelNumber,
        .noteCount = static_cast<int>(*noteCount),
    };
    if (!parsed.emplace(std::move(*id), std::move(parsedChart)).second) {
      return std::nullopt;
    }
  }
  return parsed;
}

bool parseJudgements(const Json &judgements, IrRemoteScore &score) {
  return judgements.is_object() &&
         readOptionalNonnegativeInt(judgements, "pgreat",
                                    score.judgements.pGreat) &&
         readOptionalNonnegativeInt(judgements, "great",
                                    score.judgements.great) &&
         readOptionalNonnegativeInt(judgements, "good",
                                    score.judgements.good) &&
         readOptionalNonnegativeInt(judgements, "bad", score.judgements.bad) &&
         readOptionalNonnegativeInt(judgements, "poor", score.judgements.poor);
}

bool parseOptionalMetrics(const Json &optional, IrRemoteScore &score) {
  return optional.is_object() &&
         readOptionalNonnegativeInt(optional, "epg",
                                    score.timing.earlyPGreat) &&
         readOptionalNonnegativeInt(optional, "lpg", score.timing.latePGreat) &&
         readOptionalNonnegativeInt(optional, "egr", score.timing.earlyGreat) &&
         readOptionalNonnegativeInt(optional, "lgr", score.timing.lateGreat) &&
         readOptionalNonnegativeInt(optional, "egd", score.timing.earlyGood) &&
         readOptionalNonnegativeInt(optional, "lgd", score.timing.lateGood) &&
         readOptionalNonnegativeInt(optional, "ebd", score.timing.earlyBad) &&
         readOptionalNonnegativeInt(optional, "lbd", score.timing.lateBad) &&
         readOptionalNonnegativeInt(optional, "epr", score.timing.earlyPoor) &&
         readOptionalNonnegativeInt(optional, "lpr", score.timing.latePoor) &&
         readOptionalNonnegativeInt(optional, "fast", score.fast) &&
         readOptionalNonnegativeInt(optional, "slow", score.slow) &&
         readOptionalNonnegativeInt(optional, "maxCombo", score.maxCombo) &&
         readOptionalNonnegativeInt(optional, "bp", score.badPoints) &&
         readOptionalGauge(optional, "gauge", score.finalGauge) &&
         readGaugeHistory(optional, score);
}

bool parseScoreMeta(const Json &scoreMeta, std::string_view game,
                    IrRemoteScore &score) {
  constexpr std::array gaugeValues{
      std::string_view("EASY"), std::string_view("NORMAL"),
      std::string_view("HARD"), std::string_view("EX-HARD")};
  constexpr std::array inputDeviceValues{std::string_view("BM_CONTROLLER"),
                                         std::string_view("KEYBOARD"),
                                         std::string_view("MIDI")};
  constexpr std::array clientValues{std::string_view("lr2oraja"),
                                    std::string_view("LR2")};
  return scoreMeta.is_object() && readRandom(scoreMeta, game, score.random) &&
         readOptionalEnumString(scoreMeta, "gauge", gaugeValues, score.gauge) &&
         readOptionalEnumString(scoreMeta, "inputDevice", inputDeviceValues,
                                score.inputDevice) &&
         readOptionalEnumString(scoreMeta, "client", clientValues,
                                score.client);
}

std::optional<IrRemoteScore>
parseScore(const Json &document, std::string_view expectedGame,
           const ChartMap &charts, std::optional<std::int64_t> &userIdentity) {
  if (!document.is_object() || !requiredTrue(document, "isPrimary")) {
    return std::nullopt;
  }
  auto scoreId =
      requiredString(document, "scoreID", kMaximumIrRemoteScoreIdBytes, true);
  auto chartId =
      requiredString(document, "chartID", kMaximumIrRemoteScoreIdBytes, true);
  auto songId =
      requiredString(document, "songID", kMaximumIrRemoteScoreIdBytes, true);
  auto game =
      requiredString(document, "game", kMaximumIrRemoteScoreTextBytes, true);
  auto service =
      requiredString(document, "service", kMaximumIrRemoteScoreTextBytes);
  const auto userId = requiredInteger(document, "userID");
  const auto timeAdded = requiredInteger(document, "timeAdded");
  const auto timeAchieved = document.find("timeAchieved");
  const auto scoreData = document.find("scoreData");
  const auto scoreMeta = document.find("scoreMeta");
  if (!scoreId || !chartId || !songId || !game || *game != expectedGame ||
      !service || !userId || *userId <= 0 || !timeAdded || *timeAdded <= 0 ||
      timeAchieved == document.end() || scoreData == document.end() ||
      !scoreData->is_object() || scoreMeta == document.end() ||
      !scoreMeta->is_object()) {
    return std::nullopt;
  }
  if (userIdentity && *userIdentity != *userId) {
    return std::nullopt;
  }
  userIdentity = *userId;

  const auto relatedChart = charts.find(*chartId);
  if (relatedChart == charts.end() || relatedChart->second.songId != *songId) {
    return std::nullopt;
  }
  const auto numericScore = requiredInteger(*scoreData, "score");
  auto lamp =
      requiredString(*scoreData, "lamp", kMaximumIrRemoteScoreTextBytes, true);
  const auto judgements = scoreData->find("judgements");
  const auto optional = scoreData->find("optional");
  if (!numericScore || *numericScore < 0 ||
      *numericScore > std::numeric_limits<int>::max() || !lamp ||
      judgements == scoreData->end() || optional == scoreData->end()) {
    return std::nullopt;
  }
  const auto lampRank = mapBmsLamp(*lamp);
  if (!lampRank) {
    return std::nullopt;
  }

  IrRemoteScore score{
      .remoteUserId = *userId,
      .game = std::move(*game),
      .remoteScoreId = std::move(*scoreId),
      .remoteChartId = std::move(*chartId),
      .chartMd5 = relatedChart->second.md5,
      .chartSha256 = relatedChart->second.sha256,
      .title = relatedChart->second.title,
      .artist = relatedChart->second.artist,
      .service = std::move(*service),
      .difficulty = relatedChart->second.difficulty,
      .level = relatedChart->second.level,
      .levelNumber = relatedChart->second.levelNumber,
      .noteCount = relatedChart->second.noteCount,
      .score = static_cast<int>(*numericScore),
      .lampRank = *lampRank,
      .timeAddedUnixMillis = *timeAdded,
  };
  if (timeAchieved->is_null()) {
    score.timeAchievedUnixMillis.reset();
  } else {
    const auto parsedTime = jsonInteger(*timeAchieved);
    if (!parsedTime || *parsedTime <= 0) {
      return std::nullopt;
    }
    score.timeAchievedUnixMillis = *parsedTime;
  }
  if (!parseJudgements(*judgements, score) ||
      !parseOptionalMetrics(*optional, score) ||
      !parseScoreMeta(*scoreMeta, expectedGame, score)) {
    return std::nullopt;
  }
  std::string diagnostic;
  if (!validateIrRemoteScore(score, diagnostic)) {
    return std::nullopt;
  }
  return score;
}

std::optional<IrUserScoreSnapshot>
parseSuccessBody(const Json &body, std::string_view expectedGame) {
  if (!body.is_object()) {
    return std::nullopt;
  }
  const auto songs = body.find("songs");
  const auto charts = body.find("charts");
  const auto scores = body.find("scores");
  if (songs == body.end() || charts == body.end() || scores == body.end()) {
    return std::nullopt;
  }
  auto parsedSongs = parseSongs(*songs);
  if (!parsedSongs) {
    return std::nullopt;
  }
  auto parsedCharts = parseCharts(*charts, expectedGame, *parsedSongs);
  if (!parsedCharts || !scores->is_array() ||
      scores->size() > kMaximumIrRemoteScoreSnapshotEntries) {
    return std::nullopt;
  }
  IrUserScoreSnapshot snapshot;
  snapshot.scores.reserve(scores->size());
  std::optional<std::int64_t> userIdentity;
  for (const auto &score : *scores) {
    auto parsed = parseScore(score, expectedGame, *parsedCharts, userIdentity);
    if (!parsed) {
      return std::nullopt;
    }
    snapshot.scores.emplace_back(std::move(*parsed));
  }
  std::string diagnostic;
  if (!validateIrUserScoreSnapshot(snapshot, diagnostic)) {
    return std::nullopt;
  }
  return snapshot;
}

bool isExpectedUnplayed404(const Json &document,
                           std::string_view expectedGame) {
  if (!document.is_object()) {
    return false;
  }
  const auto success = document.find("success");
  const auto description = document.find("description");
  if (success == document.end() || !success->is_boolean() ||
      success->get<bool>() || description == document.end() ||
      !description->is_string()) {
    return false;
  }
  const auto &text = description->get_ref<const std::string &>();
  if (!hasSafeBytes(text, kMaximumIrRemoteScoreDiagnosticBytes, true)) {
    return false;
  }
  const std::string suffix = " has not played " + std::string(expectedGame);
  return text.size() > suffix.size() && text.ends_with(suffix);
}

} // namespace

IrUserScoreSnapshotOutcome parseUserGameScores(std::string_view expectedGame,
                                               long httpStatus,
                                               std::string_view responseBody) {
  try {
    if (!isExpectedGame(expectedGame)) {
      return malformed("Tachi user score response game is unsupported");
    }
    if (responseBody.size() > kMaximumTachiUserScoreResponseBytes) {
      return failure(IrUserScoreSnapshotStatus::OversizedResponse,
                     "oversized_response",
                     "Tachi user score response exceeds the size limit");
    }
    if (httpStatus == 401 || httpStatus == 403) {
      return failure(IrUserScoreSnapshotStatus::AuthenticationRequired,
                     "authentication_required",
                     "Tachi authentication is required");
    }
    if (httpStatus == 408 || httpStatus == 425 || httpStatus == 429 ||
        (httpStatus >= 500 && httpStatus <= 599)) {
      return failure(IrUserScoreSnapshotStatus::TransientFailure,
                     "transient_failure",
                     "Tachi user score request failed transiently");
    }

    const Json document =
        Json::parse(responseBody.begin(), responseBody.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
      return malformed("Tachi user score response is malformed JSON");
    }
    if (httpStatus == 404) {
      return isExpectedUnplayed404(document, expectedGame)
                 ? succeeded()
                 : malformed("Tachi user score 404 is not an unplayed game");
    }
    if (httpStatus != 200) {
      return malformed("Tachi user score response has an unexpected status");
    }
    const auto success = document.find("success");
    const auto body = document.find("body");
    if (success == document.end() || !success->is_boolean() ||
        !success->get<bool>() || body == document.end()) {
      return malformed("Tachi user score envelope is invalid");
    }
    auto snapshot = parseSuccessBody(*body, expectedGame);
    return snapshot ? succeeded(std::move(*snapshot))
                    : malformed("Tachi user score body is invalid");
  } catch (...) {
    return malformed("Tachi user score response parsing failed");
  }
}

} // namespace ir::tachi
