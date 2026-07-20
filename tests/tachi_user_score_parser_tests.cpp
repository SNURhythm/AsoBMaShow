#include "ir/tachi/TachiUserScoreParser.h"

#include "scene/play/GameplayGaugeTypes.h"

#include "nlohmann/json.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;

#define REQUIRE(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "Requirement failed at " << __FILE__ << ':' << __LINE__     \
                << ": " #condition << '\n';                                    \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)

constexpr std::string_view kMd5 = "0123456789abcdef0123456789abcdef";
constexpr std::string_view kSha256 =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

Json song(std::string id = "song-1", std::string title = "Song One",
          std::string artist = "Artist One") {
  return {{"id", std::move(id)},
          {"title", std::move(title)},
          {"artist", std::move(artist)},
          {"searchTerms", Json::array()},
          {"altTitles", Json::array()},
          {"data",
           {{"genre", nullptr},
            {"subtitle", nullptr},
            {"subartist", nullptr},
            {"tableString", nullptr}}}};
}

Json chart(std::string chartId = "chart-1", std::string songId = "song-1",
           std::string game = "bms-7k") {
  Json relatedSong = song(std::move(songId));
  return {{"game", std::move(game)},
          {"chartID", std::move(chartId)},
          {"legacyChartID", "legacy-chart-1"},
          {"level", "★12"},
          {"levelNum", 12.75},
          {"isPrimary", true},
          {"difficulty", "CHART"},
          {"data",
           {{"notecount", 1000},
            {"hashMD5", kMd5},
            {"hashSHA256", kSha256},
            {"tableFolders", Json::object()}}},
          {"versions", Json::array()},
          {"song", std::move(relatedSong)}};
}

Json score(std::string scoreId = "score-1", std::string chartId = "chart-1",
           std::string songId = "song-1", std::int64_t userId = 42,
           std::string game = "bms-7k") {
  return {
      {"service", "AsoBMaShow"},
      {"game", std::move(game)},
      {"userID", userId},
      {"scoreData",
       {{"score", 1800},
        {"lamp", "EX HARD CLEAR"},
        {"percent", 90.0},
        {"grade", "AA"},
        {"enumIndexes", {{"lamp", 6}, {"grade", 6}}},
        {"judgements",
         {{"pgreat", 850},
          {"great", 100},
          {"good", 25},
          {"bad", 10},
          {"poor", 15}}},
        {"optional",
         {{"enumIndexes", Json::object()},
          {"epg", 500},
          {"lpg", 350},
          {"egr", 60},
          {"lgr", 40},
          {"egd", 15},
          {"lgd", 10},
          {"ebd", 6},
          {"lbd", 4},
          {"epr", 8},
          {"lpr", 7},
          {"fast", 20},
          {"slow", 21},
          {"maxCombo", 900},
          {"bp", 25},
          {"gauge", 73.5},
          {"gaugeHistory", Json::array({0.0, 12.25, nullptr, 73.5})}}}}},
      {"scoreMeta",
       {{"random", "S-RANDOM"},
        {"gauge", "EX-HARD"},
        {"inputDevice", "KEYBOARD"},
        {"client", "lr2oraja"}}},
      {"calculatedData", Json::object()},
      {"timeAchieved", 1784341271000LL},
      {"songID", std::move(songId)},
      {"chartID", std::move(chartId)},
      {"isPrimary", true},
      {"highlight", false},
      {"comment", nullptr},
      {"timeAdded", 1784341271999LL},
      {"scoreID", std::move(scoreId)},
      {"sessionID", nullptr},
      {"importType", "ir/direct-manual"},
  };
}

Json response(Json charts = Json::array({chart()}),
              Json scores = Json::array({score()}),
              Json songs = Json::array({song()})) {
  return {{"success", true},
          {"description", "Returned scores."},
          {"body",
           {{"charts", std::move(charts)},
            {"scores", std::move(scores)},
            {"songs", std::move(songs)}}}};
}

ir::IrUserScoreSnapshotOutcome parse(const Json &document,
                                     std::string_view game = "bms-7k",
                                     long status = 200) {
  return ir::tachi::parseUserGameScores(game, status, document.dump());
}

void requireMalformed(const Json &document, std::string_view game = "bms-7k",
                      long status = 200) {
  const auto outcome = parse(document, game, status);
  REQUIRE(outcome.status == ir::IrUserScoreSnapshotStatus::MalformedResponse);
  REQUIRE(!outcome.snapshot.has_value());
  REQUIRE(!outcome.code.empty());
  REQUIRE(outcome.diagnostic.size() <=
          ir::kMaximumIrRemoteScoreDiagnosticBytes);
}

void testCompleteOfficialResponse() {
  auto secondSong = song("song-2", "Second Song", "Second Artist");
  auto secondChart = chart("chart-2", "song-2");
  secondChart["song"] = secondSong;
  secondChart["level"] = "sl7";
  secondChart["levelNum"] = 7.25;
  secondChart["data"]["notecount"] = 750;
  secondChart["data"]["hashMD5"] = "fedcba9876543210fedcba9876543210";
  secondChart["data"]["hashSHA256"] =
      "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
  auto secondScore = score("score-2", "chart-2", "song-2");
  secondScore["scoreData"]["score"] = 1400;
  secondScore["scoreData"]["judgements"]["pgreat"] = 650;
  secondScore["scoreData"]["judgements"]["great"] = 100;
  secondScore["scoreData"]["judgements"]["good"] = 0;
  secondScore["scoreData"]["judgements"]["bad"] = 0;
  secondScore["scoreData"]["judgements"]["poor"] = 0;
  secondScore["scoreData"]["optional"]["maxCombo"] = 750;

  const auto outcome =
      parse(response(Json::array({std::move(secondChart), chart()}),
                     Json::array({score(), std::move(secondScore)}),
                     Json::array({std::move(secondSong), song()})));
  REQUIRE(outcome.status == ir::IrUserScoreSnapshotStatus::Succeeded);
  REQUIRE(outcome.snapshot.has_value());
  REQUIRE(outcome.code.empty());
  REQUIRE(outcome.diagnostic.empty());
  REQUIRE(outcome.snapshot->scores.size() == 2);

  const auto &parsed = outcome.snapshot->scores.front();
  REQUIRE(parsed.remoteUserId == 42);
  REQUIRE(parsed.game == "bms-7k");
  REQUIRE(parsed.remoteScoreId == "score-1");
  REQUIRE(parsed.remoteChartId == "chart-1");
  REQUIRE(parsed.chartMd5 == kMd5);
  REQUIRE(parsed.chartSha256 == kSha256);
  REQUIRE(parsed.title == "Song One");
  REQUIRE(parsed.artist == "Artist One");
  REQUIRE(parsed.service == "AsoBMaShow");
  REQUIRE(parsed.difficulty == "CHART");
  REQUIRE(parsed.level == "★12");
  REQUIRE(parsed.levelNumber == 12.75);
  REQUIRE(parsed.noteCount == 1000);
  REQUIRE(parsed.score == 1800);
  REQUIRE(parsed.lampRank == kClearTypeExHardClearRank);
  REQUIRE(parsed.timeAchievedUnixMillis == 1784341271000LL);
  REQUIRE(parsed.timeAddedUnixMillis == 1784341271999LL);
  REQUIRE(parsed.judgements.pGreat == 850);
  REQUIRE(parsed.judgements.great == 100);
  REQUIRE(parsed.judgements.good == 25);
  REQUIRE(parsed.judgements.bad == 10);
  REQUIRE(parsed.judgements.poor == 15);
  REQUIRE(parsed.timing.earlyPGreat == 500);
  REQUIRE(parsed.timing.latePGreat == 350);
  REQUIRE(parsed.timing.earlyGreat == 60);
  REQUIRE(parsed.timing.lateGreat == 40);
  REQUIRE(parsed.timing.earlyGood == 15);
  REQUIRE(parsed.timing.lateGood == 10);
  REQUIRE(parsed.timing.earlyBad == 6);
  REQUIRE(parsed.timing.lateBad == 4);
  REQUIRE(parsed.timing.earlyPoor == 8);
  REQUIRE(parsed.timing.latePoor == 7);
  REQUIRE(parsed.fast == 20);
  REQUIRE(parsed.slow == 21);
  REQUIRE(parsed.maxCombo == 900);
  REQUIRE(parsed.badPoints == 25);
  REQUIRE(parsed.finalGauge.has_value());
  REQUIRE(std::abs(*parsed.finalGauge - 73.5F) < 0.001F);
  REQUIRE(parsed.gaugeHistory.size() == 4);
  REQUIRE(parsed.gaugeHistory[0] == 0.0F);
  REQUIRE(parsed.gaugeHistory[1] == 12.25F);
  REQUIRE(!parsed.gaugeHistory[2].has_value());
  REQUIRE(parsed.gaugeHistory[3] == 73.5F);
  REQUIRE(parsed.random == "S-RANDOM");
  REQUIRE(parsed.gauge == "EX-HARD");
  REQUIRE(parsed.inputDevice == "KEYBOARD");
  REQUIRE(parsed.client == "lr2oraja");

  REQUIRE(outcome.snapshot->scores[1].remoteScoreId == "score-2");
  REQUIRE(outcome.snapshot->scores[1].title == "Second Song");
  REQUIRE(outcome.snapshot->scores[1].artist == "Second Artist");
  REQUIRE(outcome.snapshot->scores[1].noteCount == 750);
}

void testExplicitNullAndMissingOptionalValuesStayUnavailable() {
  auto nullable = score();
  nullable["timeAchieved"] = nullptr;
  nullable["scoreData"]["judgements"] = {
      {"pgreat", nullptr}, {"good", 0}, {"bad", nullptr}};
  nullable["scoreData"]["optional"] = {
      {"enumIndexes", Json::object()},
      {"epg", nullptr},
      {"lpg", 0},
      {"fast", nullptr},
      {"maxCombo", 0},
      {"bp", nullptr},
      {"gauge", nullptr},
      {"gaugeHistory", Json::array({100.0, nullptr})},
  };
  nullable["scoreMeta"] = {
      {"random", nullptr}, {"inputDevice", nullptr}, {"client", nullptr}};

  const auto outcome =
      parse(response(Json::array({chart()}), Json::array({std::move(nullable)}),
                     Json::array({song()})));
  REQUIRE(outcome.status == ir::IrUserScoreSnapshotStatus::Succeeded);
  const auto &parsed = outcome.snapshot->scores.front();
  REQUIRE(!parsed.timeAchievedUnixMillis.has_value());
  REQUIRE(!parsed.judgements.pGreat.has_value());
  REQUIRE(!parsed.judgements.great.has_value());
  REQUIRE(parsed.judgements.good == 0);
  REQUIRE(!parsed.judgements.bad.has_value());
  REQUIRE(!parsed.judgements.poor.has_value());
  REQUIRE(!parsed.timing.earlyPGreat.has_value());
  REQUIRE(parsed.timing.latePGreat == 0);
  REQUIRE(!parsed.timing.earlyGreat.has_value());
  REQUIRE(!parsed.fast.has_value());
  REQUIRE(!parsed.slow.has_value());
  REQUIRE(parsed.maxCombo == 0);
  REQUIRE(!parsed.badPoints.has_value());
  REQUIRE(!parsed.finalGauge.has_value());
  REQUIRE(parsed.gaugeHistory.size() == 2);
  REQUIRE(parsed.gaugeHistory[0] == 100.0F);
  REQUIRE(!parsed.gaugeHistory[1].has_value());
  REQUIRE(!parsed.random.has_value());
  REQUIRE(!parsed.gauge.has_value());
  REQUIRE(!parsed.inputDevice.has_value());
  REQUIRE(!parsed.client.has_value());
}

void testLampMappingsAndFourteenKeyScores() {
  const std::vector<std::pair<std::string, int>> lamps{
      {"NO PLAY", kClearTypeFailedRank},
      {"FAILED", kClearTypeFailedRank},
      {"ASSIST CLEAR", kClearTypeAssistedEasyClearRank},
      {"EASY CLEAR", kClearTypeEasyClearRank},
      {"CLEAR", kClearTypeNormalClearRank},
      {"HARD CLEAR", kClearTypeHardClearRank},
      {"EX HARD CLEAR", kClearTypeExHardClearRank},
      {"FULL COMBO", kClearTypeFullComboRank},
  };
  for (const auto &[lamp, expectedRank] : lamps) {
    auto candidate = score();
    candidate["scoreData"]["lamp"] = lamp;
    const auto outcome = parse(response(Json::array({chart()}),
                                        Json::array({std::move(candidate)}),
                                        Json::array({song()})));
    REQUIRE(outcome.status == ir::IrUserScoreSnapshotStatus::Succeeded);
    REQUIRE(outcome.snapshot->scores.front().lampRank == expectedRank);
  }

  auto chart14 = chart("chart-14", "song-14", "bms-14k");
  chart14["song"] = song("song-14", "Double Song", "Double Artist");
  auto score14 = score("score-14", "chart-14", "song-14", 42, "bms-14k");
  score14["scoreMeta"]["random"] = Json::array({"RANDOM", "MIRROR"});
  const auto outcome14 = parse(
      response(Json::array({std::move(chart14)}),
               Json::array({std::move(score14)}),
               Json::array({song("song-14", "Double Song", "Double Artist")})),
      "bms-14k");
  REQUIRE(outcome14.status == ir::IrUserScoreSnapshotStatus::Succeeded);
  REQUIRE(outcome14.snapshot->scores.front().game == "bms-14k");
  REQUIRE(outcome14.snapshot->scores.front().random == "RANDOM / MIRROR");

  auto missingRandom14 =
      score("score-14", "chart-14", "song-14", 42, "bms-14k");
  missingRandom14["scoreMeta"].erase("random");
  const auto missingOutcome =
      parse(response(Json::array({chart("chart-14", "song-14", "bms-14k")}),
                     Json::array({std::move(missingRandom14)}),
                     Json::array({song("song-14")})),
            "bms-14k");
  REQUIRE(missingOutcome.status == ir::IrUserScoreSnapshotStatus::Succeeded);
  REQUIRE(!missingOutcome.snapshot->scores.front().random.has_value());

  auto invalidTuple14 = score("score-14", "chart-14", "song-14", 42, "bms-14k");
  invalidTuple14["scoreMeta"]["random"] = Json::array({"RANDOM"});
  requireMalformed(
      response(Json::array({chart("chart-14", "song-14", "bms-14k")}),
               Json::array({std::move(invalidTuple14)}),
               Json::array({song("song-14")})),
      "bms-14k");

  auto invalidEnum14 = score("score-14", "chart-14", "song-14", 42, "bms-14k");
  invalidEnum14["scoreMeta"]["random"] = Json::array({"RANDOM", "H-RANDOM"});
  requireMalformed(
      response(Json::array({chart("chart-14", "song-14", "bms-14k")}),
               Json::array({std::move(invalidEnum14)}),
               Json::array({song("song-14")})),
      "bms-14k");

  auto invalidRandom7 = response();
  invalidRandom7["body"]["scores"][0]["scoreMeta"]["random"] = "H-RANDOM";
  requireMalformed(invalidRandom7);
}

void testEmptySuccessfulResponse() {
  const auto outcome =
      parse(response(Json::array(), Json::array(), Json::array()));
  REQUIRE(outcome.status == ir::IrUserScoreSnapshotStatus::Succeeded);
  REQUIRE(outcome.snapshot.has_value());
  REQUIRE(outcome.snapshot->scores.empty());
}

void testRelationAndIdentityFailures() {
  auto duplicateSongs = response();
  duplicateSongs["body"]["songs"].push_back(song());
  requireMalformed(duplicateSongs);

  auto duplicateCharts = response();
  duplicateCharts["body"]["charts"].push_back(chart());
  requireMalformed(duplicateCharts);

  auto duplicateScores = response();
  duplicateScores["body"]["scores"].push_back(score());
  requireMalformed(duplicateScores);

  auto missingChart = response();
  missingChart["body"]["scores"][0]["chartID"] = "missing-chart";
  requireMalformed(missingChart);

  auto missingSong = response();
  missingSong["body"]["charts"][0]["song"]["id"] = "missing-song";
  requireMalformed(missingSong);

  auto scoreSongMismatch = response();
  scoreSongMismatch["body"]["scores"][0]["songID"] = "other-song";
  requireMalformed(scoreSongMismatch);

  auto embeddedSongMismatch = response();
  embeddedSongMismatch["body"]["charts"][0]["song"]["title"] = "Stale";
  requireMalformed(embeddedSongMismatch);

  auto secondScore = score("score-2");
  secondScore["userID"] = 43;
  auto mixedUsers = response(Json::array({chart()}),
                             Json::array({score(), std::move(secondScore)}),
                             Json::array({song()}));
  requireMalformed(mixedUsers);
}

void testGameAndPrimaryFailures() {
  auto nonPrimaryChart = response();
  nonPrimaryChart["body"]["charts"][0]["isPrimary"] = false;
  requireMalformed(nonPrimaryChart);

  auto nonPrimaryScore = response();
  nonPrimaryScore["body"]["scores"][0]["isPrimary"] = false;
  requireMalformed(nonPrimaryScore);

  auto mixedChartGame = response();
  mixedChartGame["body"]["charts"][0]["game"] = "bms-14k";
  requireMalformed(mixedChartGame);

  auto mixedScoreGame = response();
  mixedScoreGame["body"]["scores"][0]["game"] = "bms-14k";
  requireMalformed(mixedScoreGame);

  auto unknownLamp = response();
  unknownLamp["body"]["scores"][0]["scoreData"]["lamp"] = "PERFECT";
  requireMalformed(unknownLamp);

  requireMalformed(response(), "iidx-sp");
}

void testMissingOrWrongRequiredFieldsFail() {
  const std::vector<std::vector<std::string>> erasePaths{
      {"body", "charts"},
      {"body", "scores"},
      {"body", "songs"},
      {"body", "charts", "0", "song"},
      {"body", "charts", "0", "data"},
      {"body", "charts", "0", "difficulty"},
      {"body", "charts", "0", "level"},
      {"body", "charts", "0", "levelNum"},
      {"body", "scores", "0", "scoreData"},
      {"body", "scores", "0", "scoreMeta"},
      {"body", "scores", "0", "timeAchieved"},
      {"body", "scores", "0", "timeAdded"},
  };
  for (const auto &path : erasePaths) {
    auto invalid = response();
    Json *parent = &invalid;
    for (std::size_t index = 0; index + 1 < path.size(); ++index) {
      if (parent->is_array()) {
        parent = &parent->at(static_cast<std::size_t>(std::stoul(path[index])));
      } else {
        parent = &parent->at(path[index]);
      }
    }
    parent->erase(path.back());
    requireMalformed(invalid);
  }

  auto wrongOptional = response();
  wrongOptional["body"]["scores"][0]["scoreData"]["optional"]["fast"] = "20";
  requireMalformed(wrongOptional);

  auto wrongNullability = response();
  wrongNullability["body"]["scores"][0]["timeAdded"] = nullptr;
  requireMalformed(wrongNullability);

  auto wrongMetadata = response();
  wrongMetadata["body"]["scores"][0]["scoreMeta"]["client"] = 10;
  requireMalformed(wrongMetadata);
}

Json oversizedArray(std::size_t size) {
  Json values = Json::array();
  for (std::size_t index = 0; index < size; ++index) {
    values.push_back(nullptr);
  }
  return values;
}

void testBoundsAreEnforced() {
  auto tooManySongs = response();
  tooManySongs["body"]["songs"] =
      oversizedArray(ir::kMaximumIrRemoteScoreSnapshotEntries + 1);
  requireMalformed(tooManySongs);

  auto tooManyCharts = response();
  tooManyCharts["body"]["charts"] =
      oversizedArray(ir::kMaximumIrRemoteScoreSnapshotEntries + 1);
  requireMalformed(tooManyCharts);

  auto tooManyScores = response();
  tooManyScores["body"]["scores"] =
      oversizedArray(ir::kMaximumIrRemoteScoreSnapshotEntries + 1);
  requireMalformed(tooManyScores);

  auto oversizedId = response();
  oversizedId["body"]["scores"][0]["scoreID"] =
      std::string(ir::kMaximumIrRemoteScoreIdBytes + 1, 'x');
  requireMalformed(oversizedId);

  auto oversizedTitle = response();
  oversizedTitle["body"]["songs"][0]["title"] =
      std::string(ir::kMaximumIrRemoteScoreTextBytes + 1, 'x');
  requireMalformed(oversizedTitle);

  auto oversizedHistory = response();
  oversizedHistory["body"]["scores"][0]["scoreData"]["optional"]
                  ["gaugeHistory"] = oversizedArray(
                      ir::kMaximumIrRemoteGaugeHistoryEntries + 1);
  requireMalformed(oversizedHistory);
}

void testNumericConversionsAreStrict() {
  auto invalidScore = response();
  invalidScore["body"]["scores"][0]["scoreData"]["score"] =
      std::numeric_limits<std::uint64_t>::max();
  requireMalformed(invalidScore);

  auto invalidTimestamp = response();
  invalidTimestamp["body"]["scores"][0]["timeAdded"] =
      std::numeric_limits<std::uint64_t>::max();
  requireMalformed(invalidTimestamp);

  auto negativeMetric = response();
  negativeMetric["body"]["scores"][0]["scoreData"]["optional"]["bp"] = -1;
  requireMalformed(negativeMetric);

  auto fractionalMetric = response();
  fractionalMetric["body"]["scores"][0]["scoreData"]["optional"]["epg"] = 1.5;
  requireMalformed(fractionalMetric);

  auto invalidGauge = response();
  invalidGauge["body"]["scores"][0]["scoreData"]["optional"]["gauge"] = 100.01;
  requireMalformed(invalidGauge);

  auto invalidHistory = response();
  invalidHistory["body"]["scores"][0]["scoreData"]["optional"]["gaugeHistory"]
                [0] = -0.01;
  requireMalformed(invalidHistory);

  auto invalidLevel = response();
  invalidLevel["body"]["charts"][0]["levelNum"] = -0.01;
  requireMalformed(invalidLevel);

  std::string nonFiniteLevel = response().dump();
  const auto levelPosition = nonFiniteLevel.find("12.75");
  REQUIRE(levelPosition != std::string::npos);
  nonFiniteLevel.replace(levelPosition, 5, "1e400");
  const auto nonFiniteOutcome =
      ir::tachi::parseUserGameScores("bms-7k", 200, nonFiniteLevel);
  REQUIRE(nonFiniteOutcome.status ==
          ir::IrUserScoreSnapshotStatus::MalformedResponse);
}

void testKnownUnplayed404IsAnEmptySnapshot() {
  const Json notPlayed{
      {"success", false},
      {"description", "The user mutable-name has not played bms-7k"}};
  const auto outcome = parse(notPlayed, "bms-7k", 404);
  REQUIRE(outcome.status == ir::IrUserScoreSnapshotStatus::Succeeded);
  REQUIRE(outcome.snapshot.has_value());
  REQUIRE(outcome.snapshot->scores.empty());
  REQUIRE(outcome.code.empty());
  REQUIRE(outcome.diagnostic.empty());
}

void testUnknown404sAreMalformed() {
  requireMalformed(
      Json{{"success", false}, {"description", "This chart does not exist."}},
      "bms-7k", 404);
  requireMalformed(
      Json{{"success", false},
           {"description", "The user mutable-name has not played bms-14k"}},
      "bms-7k", 404);
  requireMalformed(Json{{"success", false}}, "bms-7k", 404);
  requireMalformed(
      Json{{"success", true},
           {"description", "The user mutable-name has not played bms-7k"}},
      "bms-7k", 404);
  requireMalformed(
      Json{{"success", false},
           {"description", "The user mutable-name has not played bms-7k."}},
      "bms-7k", 404);
  requireMalformed(
      Json{{"success", false},
           {"description",
            std::string(ir::kMaximumIrRemoteScoreDiagnosticBytes + 1, 'x') +
                " has not played bms-7k"}},
      "bms-7k", 404);

  const auto malformedJson =
      ir::tachi::parseUserGameScores("bms-7k", 404, "{not-json");
  REQUIRE(malformedJson.status ==
          ir::IrUserScoreSnapshotStatus::MalformedResponse);
}

void testHttpStatusClassification() {
  const auto authentication =
      ir::tachi::parseUserGameScores("bms-7k", 401, R"({})");
  REQUIRE(authentication.status ==
          ir::IrUserScoreSnapshotStatus::AuthenticationRequired);
  REQUIRE(!authentication.snapshot.has_value());

  const auto forbidden = ir::tachi::parseUserGameScores("bms-7k", 403, R"({})");
  REQUIRE(forbidden.status ==
          ir::IrUserScoreSnapshotStatus::AuthenticationRequired);

  const auto rateLimited =
      ir::tachi::parseUserGameScores("bms-7k", 429, R"({})");
  REQUIRE(rateLimited.status ==
          ir::IrUserScoreSnapshotStatus::TransientFailure);

  const auto serverFailure =
      ir::tachi::parseUserGameScores("bms-7k", 503, R"({})");
  REQUIRE(serverFailure.status ==
          ir::IrUserScoreSnapshotStatus::TransientFailure);

  requireMalformed(response(), "bms-7k", 201);
  requireMalformed(Json{{"success", false}, {"description", "Rejected"}},
                   "bms-7k", 400);
}

} // namespace

int main() {
  testCompleteOfficialResponse();
  testExplicitNullAndMissingOptionalValuesStayUnavailable();
  testLampMappingsAndFourteenKeyScores();
  testEmptySuccessfulResponse();
  testRelationAndIdentityFailures();
  testGameAndPrimaryFailures();
  testMissingOrWrongRequiredFieldsFail();
  testBoundsAreEnforced();
  testNumericConversionsAreStrict();
  testKnownUnplayed404IsAnEmptySnapshot();
  testUnknown404sAreMalformed();
  testHttpStatusClassification();
  return 0;
}
