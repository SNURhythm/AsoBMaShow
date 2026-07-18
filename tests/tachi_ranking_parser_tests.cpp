#include "ir/tachi/TachiRankingParser.h"

#include "nlohmann/json.hpp"

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

#define REQUIRE(condition) require((condition), #condition, __LINE__)

void require(bool condition, const char *expression, int line) {
  if (condition) {
    return;
  }
  std::cerr << "requirement failed at line " << line << ": " << expression
            << '\n';
  std::exit(1);
}

ir::IrChartQuery query(int keyMode = 7) {
  return {
      .keyMode = keyMode,
      .chartMd5 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      .chartSha256 =
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      .totalNotes = 826,
  };
}

Json user(std::int64_t id, std::string username) {
  return {{"id", id}, {"username", std::move(username)}};
}

Json pb(std::int64_t userId = 42, int rank = 110, int outOf = 512,
        int score = 1284, std::string lamp = "CLEAR") {
  return {
      {"composedFrom",
       Json::array({{{"name", "Best Score"}, {"scoreID", "score-42"}}})},
      {"rankingData",
       {{"rank", rank}, {"outOf", outOf}, {"rivalRank", nullptr}}},
      {"userID", userId},
      {"chartID", "chart-id"},
      {"game", "bms-7k"},
      {"timeAchieved", 1784341271000LL},
      {"scoreData",
       {{"score", score},
        {"lamp", std::move(lamp)},
        {"enumIndexes", {{"lamp", 4}}},
        {"judgements",
         {{"pgreat", 511},
          {"great", 262},
          {"good", 31},
          {"bad", 14},
          {"poor", 8}}},
        {"optional",
         {{"enumIndexes", Json::object()},
          {"epg", 336},
          {"lpg", 175},
          {"egr", 201},
          {"lgr", 61},
          {"egd", 13},
          {"lgd", 18},
          {"ebd", 11},
          {"lbd", 3},
          {"epr", 0},
          {"lpr", 8},
          {"bp", 62},
          {"maxCombo", 109}}}}},
  };
}

std::string page(const Json &pbs, const Json &users) {
  return Json{{"success", true},
              {"description", "Returned scores."},
              {"body", {{"pbs", pbs}, {"users", users}}}}
      .dump();
}

void testIdentityAndChartResolution() {
  const auto identity = ir::tachi::parseRankingIdentityResponse(
      R"({"success":true,"body":{"whoami":42}})");
  REQUIRE(identity.status == ir::ChartRankingStatus::Succeeded);
  REQUIRE(identity.userId == 42);

  const std::string resolveBody = Json{
      {"success", true},
      {"body",
       {{"chart",
         {{"chartID", "chart-id"},
          {"game", "bms-7k"},
          {"data", {{"hashSHA256", query().chartSha256}, {"notecount", 826}}}}},
        {"song",
         {{"id", "song-id"}}}}}}.dump();
  const auto resolved =
      ir::tachi::parseChartResolveResponse(resolveBody, query());
  REQUIRE(resolved.status == ir::ChartRankingStatus::Succeeded);
  REQUIRE(resolved.chartId == "chart-id");

  auto wrongMode = Json::parse(resolveBody);
  wrongMode["body"]["chart"]["game"] = "bms-14k";
  REQUIRE(
      ir::tachi::parseChartResolveResponse(wrongMode.dump(), query()).status ==
      ir::ChartRankingStatus::MalformedResponse);

  auto wrongHash = Json::parse(resolveBody);
  wrongHash["body"]["chart"]["data"]["hashSHA256"] = std::string(64, 'c');
  REQUIRE(
      ir::tachi::parseChartResolveResponse(wrongHash.dump(), query()).status ==
      ir::ChartRankingStatus::MalformedResponse);

  auto wrongNotes = Json::parse(resolveBody);
  wrongNotes["body"]["chart"]["data"]["notecount"] = 825;
  REQUIRE(
      ir::tachi::parseChartResolveResponse(wrongNotes.dump(), query()).status ==
      ir::ChartRankingStatus::MalformedResponse);

  REQUIRE(ir::tachi::parseRankingIdentityResponse(
              R"({"success":true,"body":{"whoami":null}})")
              .status == ir::ChartRankingStatus::MalformedResponse);
}

void testNativePbRetainsAuthenticJudgements() {
  const auto result = ir::tachi::parseRankingPageResponse(
      page(Json::array({pb()}), Json::array({user(42, "YouName")})), query(),
      "chart-id", 42);
  REQUIRE(result.status == ir::ChartRankingStatus::Succeeded);
  REQUIRE(result.page.has_value());
  REQUIRE(result.page->outOf == 512);
  REQUIRE(result.page->entries.size() == 1);

  const auto &entry = result.page->entries.front();
  REQUIRE(entry.rank == 110);
  REQUIRE(entry.playerName == "YouName");
  REQUIRE(entry.currentUser);
  REQUIRE(entry.score == 1284);
  REQUIRE(entry.maxScore == 1652);
  REQUIRE(entry.pGreat == 511);
  REQUIRE(entry.great == 262);
  REQUIRE(entry.good == 31);
  REQUIRE(entry.bad == 14);
  REQUIRE(entry.poor == 8);
  REQUIRE(entry.earlyPGreat == 336);
  REQUIRE(entry.latePGreat == 175);
  REQUIRE(entry.earlyGreat == 201);
  REQUIRE(entry.lateGreat == 61);
  REQUIRE(entry.earlyGood == 13);
  REQUIRE(entry.lateGood == 18);
  REQUIRE(entry.earlyBad == 11);
  REQUIRE(entry.lateBad == 3);
  REQUIRE(entry.earlyPoor == 0);
  REQUIRE(entry.latePoor == 8);
  REQUIRE(entry.clearType == kClearTypeNormalClearRank);
  REQUIRE(entry.badPoints == 62);
  REQUIRE(entry.maxCombo == 109);
  REQUIRE(entry.achievedAtUnixMillis == 1784341271000LL);
}

void testAnonymousPageNeverMarksCurrentUser() {
  const auto result = ir::tachi::parseRankingPageResponse(
      page(Json::array({pb(42, 1, 2), pb(7, 2, 2)}),
           Json::array({user(42, "CachedUser"), user(7, "OtherUser")})),
      query(), "chart-id", std::nullopt);
  REQUIRE(result.status == ir::ChartRankingStatus::Succeeded);
  REQUIRE(result.page.has_value());
  REQUIRE(result.page->entries.size() == 2);
  REQUIRE(!result.page->entries[0].currentUser);
  REQUIRE(!result.page->entries[1].currentUser);
}

void testMissingOptionalMetricsStayUnavailable() {
  auto score = pb(7, 1, 1, 1000, "HARD CLEAR");
  score["scoreData"]["optional"] = {{"enumIndexes", Json::object()}};
  score["timeAchieved"] = nullptr;

  const auto result = ir::tachi::parseRankingPageResponse(
      page(Json::array({score}), Json::array({user(7, "OldScore")})), query(),
      "chart-id", 42);
  REQUIRE(result.status == ir::ChartRankingStatus::Succeeded);
  const auto &entry = result.page->entries.front();
  REQUIRE(!entry.currentUser);
  REQUIRE(entry.pGreat == 511);
  REQUIRE(entry.great == 262);
  REQUIRE(entry.good == 31);
  REQUIRE(entry.bad == 14);
  REQUIRE(entry.poor == 8);
  REQUIRE(!entry.earlyPGreat.has_value());
  REQUIRE(!entry.latePGreat.has_value());
  REQUIRE(!entry.earlyGreat.has_value());
  REQUIRE(!entry.lateGreat.has_value());
  REQUIRE(!entry.earlyGood.has_value());
  REQUIRE(!entry.lateGood.has_value());
  REQUIRE(!entry.earlyBad.has_value());
  REQUIRE(!entry.lateBad.has_value());
  REQUIRE(!entry.earlyPoor.has_value());
  REQUIRE(!entry.latePoor.has_value());
  REQUIRE(!entry.badPoints.has_value());
  REQUIRE(!entry.maxCombo.has_value());
  REQUIRE(!entry.achievedAtUnixMillis.has_value());
}

void testInconsistentOrPartialTimingStaysUnavailable() {
  auto inconsistent = pb(7, 164, 332, 1235, "CLEAR");
  inconsistent["scoreData"].erase("judgements");
  inconsistent["scoreData"]["optional"]["epg"] = 300;
  inconsistent["scoreData"]["optional"]["lpg"] = 100;
  inconsistent["scoreData"]["optional"]["egr"] = 200;
  inconsistent["scoreData"]["optional"]["lgr"] = 34;

  auto result = ir::tachi::parseRankingPageResponse(
      page(Json::array({inconsistent}), Json::array({user(7, "LegacyScore")})),
      query(), "chart-id", 42);
  REQUIRE(result.status == ir::ChartRankingStatus::Succeeded);
  REQUIRE(result.page.has_value());
  const auto &inconsistentEntry = result.page->entries.front();
  REQUIRE(!inconsistentEntry.earlyPGreat.has_value());
  REQUIRE(!inconsistentEntry.latePGreat.has_value());
  REQUIRE(!inconsistentEntry.earlyGreat.has_value());
  REQUIRE(!inconsistentEntry.lateGreat.has_value());

  auto partial = pb(8, 165, 332, 1234, "CLEAR");
  partial["scoreData"].erase("judgements");
  partial["scoreData"]["optional"]["lgr"] = nullptr;
  result = ir::tachi::parseRankingPageResponse(
      page(Json::array({partial}), Json::array({user(8, "PartialScore")})),
      query(), "chart-id", 42);
  REQUIRE(result.status == ir::ChartRankingStatus::Succeeded);
  REQUIRE(!result.page->entries.front().earlyPGreat.has_value());
  REQUIRE(!result.page->entries.front().latePGreat.has_value());
  REQUIRE(!result.page->entries.front().earlyGreat.has_value());
  REQUIRE(!result.page->entries.front().lateGreat.has_value());
}

void testEachTimingPairDegradesIndependently() {
  auto partial = pb();
  partial["scoreData"]["optional"]["lgd"] = nullptr;
  auto result = ir::tachi::parseRankingPageResponse(
      page(Json::array({partial}), Json::array({user(42, "PartialGood")})),
      query(), "chart-id", 42);
  REQUIRE(result.status == ir::ChartRankingStatus::Succeeded);
  const auto &entry = result.page->entries.front();
  REQUIRE(entry.pGreat == 511);
  REQUIRE(entry.good == 31);
  REQUIRE(entry.earlyPGreat == 336);
  REQUIRE(entry.latePGreat == 175);
  REQUIRE(!entry.earlyGood.has_value());
  REQUIRE(!entry.lateGood.has_value());
  REQUIRE(entry.earlyBad == 11);
  REQUIRE(entry.lateBad == 3);

  auto inconsistent = pb();
  inconsistent["scoreData"]["optional"]["egd"] = 12;
  result = ir::tachi::parseRankingPageResponse(
      page(Json::array({inconsistent}),
           Json::array({user(42, "InconsistentGood")})),
      query(), "chart-id", 42);
  REQUIRE(result.status == ir::ChartRankingStatus::Succeeded);
  REQUIRE(!result.page->entries.front().earlyGood.has_value());
  REQUIRE(!result.page->entries.front().lateGood.has_value());
  REQUIRE(result.page->entries.front().earlyBad == 11);

  auto negative = pb();
  negative["scoreData"]["optional"]["ebd"] = -1;
  REQUIRE(ir::tachi::parseRankingPageResponse(
              page(Json::array({negative}), Json::array({user(42, "Bad")})),
              query(), "chart-id", 42)
              .status == ir::ChartRankingStatus::MalformedResponse);

  auto nonInteger = pb();
  nonInteger["scoreData"]["optional"]["epr"] = 0.5;
  REQUIRE(ir::tachi::parseRankingPageResponse(
              page(Json::array({nonInteger}), Json::array({user(42, "Bad")})),
              query(), "chart-id", 42)
              .status == ir::ChartRankingStatus::MalformedResponse);
}

void testLampMappingsAndFourteenKeyGame() {
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
  for (std::size_t index = 0; index < lamps.size(); ++index) {
    auto row =
        pb(static_cast<std::int64_t>(index + 1), static_cast<int>(index + 1),
           static_cast<int>(lamps.size()), 1284, lamps[index].first);
    const auto result = ir::tachi::parseRankingPageResponse(
        page(Json::array({row}),
             Json::array({user(static_cast<std::int64_t>(index + 1),
                               "Player" + std::to_string(index))})),
        query(), "chart-id", 99);
    REQUIRE(result.status == ir::ChartRankingStatus::Succeeded);
    REQUIRE(result.page->entries.front().clearType == lamps[index].second);
  }

  auto row14 = pb();
  row14["game"] = "bms-14k";
  const auto result14 = ir::tachi::parseRankingPageResponse(
      page(Json::array({row14}), Json::array({user(42, "DPPlayer")})),
      query(14), "chart-id", 42);
  REQUIRE(result14.status == ir::ChartRankingStatus::Succeeded);
}

void testPageValidation() {
  auto invalid = pb();
  invalid["scoreData"]["score"] = 1653;
  REQUIRE(ir::tachi::parseRankingPageResponse(
              page(Json::array({invalid}), Json::array({user(42, "Player")})),
              query(), "chart-id", 42)
              .status == ir::ChartRankingStatus::MalformedResponse);

  invalid = pb();
  invalid["chartID"] = "other-chart";
  REQUIRE(ir::tachi::parseRankingPageResponse(
              page(Json::array({invalid}), Json::array({user(42, "Player")})),
              query(), "chart-id", 42)
              .status == ir::ChartRankingStatus::MalformedResponse);

  invalid = pb();
  invalid["scoreData"]["optional"]["bp"] = -1;
  REQUIRE(ir::tachi::parseRankingPageResponse(
              page(Json::array({invalid}), Json::array({user(42, "Player")})),
              query(), "chart-id", 42)
              .status == ir::ChartRankingStatus::MalformedResponse);

  invalid = pb();
  invalid["rankingData"]["outOf"] = ir::tachi::kMaximumRankingEntries + 1;
  REQUIRE(ir::tachi::parseRankingPageResponse(
              page(Json::array({invalid}), Json::array({user(42, "Player")})),
              query(), "chart-id", 42)
              .status == ir::ChartRankingStatus::OversizedResponse);

  REQUIRE(ir::tachi::parseRankingPageResponse(
              page(Json::array({pb()}), Json::array()), query(), "chart-id", 42)
              .status == ir::ChartRankingStatus::MalformedResponse);

  REQUIRE(ir::tachi::parseRankingPageResponse(
              R"({"success":true,"body":{"pbs":[],"users":[]}})", query(),
              "chart-id", 42)
              .status == ir::ChartRankingStatus::Succeeded);

  const std::string oversized(ir::tachi::kMaximumRankingResponseBytes + 1, ' ');
  REQUIRE(
      ir::tachi::parseRankingPageResponse(oversized, query(), "chart-id", 42)
          .status == ir::ChartRankingStatus::OversizedResponse);
}

void testNamesAndPageOrdering() {
  auto first = pb(1, 101, 200, 1284);
  auto second = pb(2, 102, 200, 1200);
  second["scoreData"]["optional"] = {{"enumIndexes", Json::object()},
                                     {"epg", 300},
                                     {"lpg", 200},
                                     {"egr", 100},
                                     {"lgr", 100}};
  const auto result = ir::tachi::parseRankingPageResponse(
      page(Json::array({first, second}),
           Json::array({user(1, "Alice"), user(2, "Bob")})),
      query(), "chart-id", 2);
  REQUIRE(result.status == ir::ChartRankingStatus::Succeeded);
  REQUIRE(result.page->entries[0].playerName == "Alice");
  REQUIRE(result.page->entries[1].playerName == "Bob");
  REQUIRE(result.page->entries[1].currentUser);

  auto reversed = Json::array({second, first});
  REQUIRE(ir::tachi::parseRankingPageResponse(
              page(reversed, Json::array({user(1, "Alice"), user(2, "Bob")})),
              query(), "chart-id", 2)
              .status == ir::ChartRankingStatus::MalformedResponse);

  const std::string valid64 = std::string(63, 'a') + "é";
  REQUIRE(ir::tachi::parseRankingPageResponse(
              page(Json::array({pb()}), Json::array({user(42, valid64)})),
              query(), "chart-id", 42)
              .status == ir::ChartRankingStatus::Succeeded);
  REQUIRE(ir::tachi::parseRankingPageResponse(
              page(Json::array({pb()}),
                   Json::array({user(42, std::string(64, 'a') + "é")})),
              query(), "chart-id", 42)
              .status == ir::ChartRankingStatus::MalformedResponse);
}

} // namespace

int main() {
  testIdentityAndChartResolution();
  testNativePbRetainsAuthenticJudgements();
  testAnonymousPageNeverMarksCurrentUser();
  testMissingOptionalMetricsStayUnavailable();
  testInconsistentOrPartialTimingStaysUnavailable();
  testEachTimingPairDegradesIndependently();
  testLampMappingsAndFourteenKeyGame();
  testPageValidation();
  testNamesAndPageOrdering();
  std::cout << "Tachi native ranking parser tests passed\n";
  return 0;
}
