#include "ir/tachi/TachiRankingParser.h"

#include "nlohmann/json.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

ir::IrChartQuery query() {
  return {
      .keyMode = 7,
      .chartMd5 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      .chartSha256 =
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      .totalNotes = 100,
  };
}

Json row(std::string player = "Alice", int score = 150, int clear = 5,
         int minBp = 3, std::int64_t date = 1700000000000LL) {
  return {
      {"player", std::move(player)},
      {"notes", 100},
      {"epg", score / 2},
      {"lpg", 0},
      {"egr", score % 2},
      {"lgr", 0},
      {"clear", clear},
      {"minbp", minBp},
      {"maxcombo", 80},
      {"date", date},
  };
}

std::string response(const Json &rows) {
  return Json{{"success", true}, {"description", "scores"}, {"body", rows}}
      .dump();
}

ir::ChartRankingOutcome parse(const Json &rows) {
  return ir::tachi::parseRankingResponse(response(rows), query());
}

void expectMalformed(Json rows, std::string_view message) {
  const auto outcome = parse(rows);
  expect(outcome.status == ir::ChartRankingStatus::MalformedResponse, message);
  expect(!outcome.ranking.has_value(),
         "malformed ranking never returns partial entries");
}

void testEmptyAndAuthenticatedRanking() {
  auto outcome = parse(Json::array());
  expect(outcome.status == ir::ChartRankingStatus::Succeeded,
         "empty ranking succeeds");
  expect(outcome.ranking && outcome.ranking->entries.empty(),
         "empty ranking returns an empty normalized list");
  expect(outcome.ranking && outcome.ranking->providerId == "tachi" &&
             outcome.ranking->chart == query(),
         "ranking retains provider and canonical chart query");

  auto current = row("", 110, 5, 0);
  current["maxcombo"] = nullptr;
  outcome = parse(Json::array({current}));
  expect(outcome.status == ir::ChartRankingStatus::Succeeded &&
             outcome.ranking && outcome.ranking->entries.size() == 1,
         "one authenticated score parses");
  if (!outcome.ranking || outcome.ranking->entries.empty()) {
    return;
  }
  const auto &entry = outcome.ranking->entries.front();
  expect(entry.rank == 1, "single entry has rank one");
  expect(entry.playerName == "You" && entry.currentUser,
         "empty player denotes the authenticated user");
  expect(entry.score == 110 && entry.maxScore == 200,
         "EX score and maximum are normalized");
  expect(entry.clearType == kClearTypeNormalClearRank,
         "clear index maps to canonical rank");
  expect(entry.badPoints == 0, "zero BP is retained as a real value");
  expect(!entry.maxCombo.has_value(), "null max combo remains absent");
  expect(entry.achievedAtUnixMillis == 1700000000000LL,
         "positive date is retained as Unix milliseconds");
}

void testEveryClearIndex() {
  const std::vector<int> expected{
      kClearTypeFailedRank,
      kClearTypeFailedRank,
      kClearTypeAssistedEasyClearRank,
      kClearTypeAssistedEasyClearRank,
      kClearTypeEasyClearRank,
      kClearTypeNormalClearRank,
      kClearTypeHardClearRank,
      kClearTypeExHardClearRank,
      kClearTypeFullComboRank,
      kClearTypeFullComboRank,
  };
  for (int clear = 0; clear <= 9; ++clear) {
    const auto outcome = parse(Json::array({row("Player", 100, clear)}));
    expect(outcome.status == ir::ChartRankingStatus::Succeeded &&
               outcome.ranking &&
               outcome.ranking->entries.front().clearType == expected[clear],
           std::string("maps beatoraja clear index ") + std::to_string(clear));
  }
  expectMalformed(Json::array({row("Player", 100, 10)}),
                  "unknown clear index rejects the whole ranking");
}

void testPlayerNameValidation() {
  const std::string sixtyFourCodePoints = std::string(63, 'a') + "é";
  auto outcome = parse(Json::array({row(sixtyFourCodePoints)}));
  expect(outcome.status == ir::ChartRankingStatus::Succeeded,
         "valid 64-code-point UTF-8 player name succeeds");

  expectMalformed(Json::array({row(std::string(64, 'a') + "é")}),
                  "65-code-point player name is rejected");

  std::string invalidUtf8 = "bad";
  invalidUtf8.push_back(static_cast<char>(0xc3));
  invalidUtf8.push_back('(');
  std::string invalidBody =
      R"({"success":true,"body":[{"player":")" + invalidUtf8 +
      R"(","notes":100,"epg":50,"lpg":0,"egr":0,"lgr":0,"clear":5,"minbp":1,"maxcombo":80,"date":1}]})";
  outcome = ir::tachi::parseRankingResponse(invalidBody, query());
  expect(outcome.status == ir::ChartRankingStatus::MalformedResponse,
         "invalid UTF-8 player name is rejected");

  Json controlled = row("safe");
  controlled["player"] = std::string("unsafe\x01name", 11);
  expectMalformed(Json::array({controlled}),
                  "control characters in player names are rejected");
}

void testDuplicateCurrentUserAndNoteMismatch() {
  expectMalformed(Json::array({row(""), row("")}),
                  "duplicate authenticated user rows are rejected");

  auto mismatch = row();
  mismatch["notes"] = 99;
  expectMalformed(Json::array({mismatch}),
                  "note-count mismatch rejects the whole ranking");
}

void testNumericAndRowBounds() {
  auto invalid = row();
  invalid["epg"] = std::numeric_limits<int>::max();
  invalid["lpg"] = std::numeric_limits<int>::max();
  expectMalformed(Json::array({invalid}),
                  "EX score arithmetic overflow is rejected");

  invalid = row();
  invalid["epg"] = 101;
  invalid["egr"] = 0;
  expectMalformed(Json::array({invalid}),
                  "EX score above chart maximum is rejected");

  auto lateKpoor = row("Late KPOOR", 2, 0, 102);
  lateKpoor["maxcombo"] = nullptr;
  auto outcome = parse(Json::array({lateKpoor}));
  expect(outcome.status == ir::ChartRankingStatus::Succeeded &&
             outcome.ranking &&
             outcome.ranking->entries.front().badPoints == 102,
         "BP above note count is accepted for LR2 KPOOR accounting");

  auto legacyCombo = row("Legacy Combo", 190, 8, 0);
  legacyCombo["maxcombo"] = 105;
  outcome = parse(Json::array({legacyCombo}));
  expect(outcome.status == ir::ChartRankingStatus::Succeeded &&
             outcome.ranking &&
             outcome.ranking->entries.front().maxCombo == 105,
         "legacy max combo above server note count is accepted");

  invalid = row();
  invalid["minbp"] = -1;
  expectMalformed(Json::array({invalid}), "negative BP is rejected");
  invalid = row();
  invalid["minbp"] =
      static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1;
  expectMalformed(Json::array({invalid}),
                  "BP outside integer range is rejected");
  invalid = row();
  invalid["maxcombo"] = -1;
  expectMalformed(Json::array({invalid}), "negative max combo is rejected");
  invalid = row();
  invalid["maxcombo"] =
      static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1;
  expectMalformed(Json::array({invalid}),
                  "max combo outside integer range is rejected");

  invalid = row();
  invalid["date"] = -1;
  expectMalformed(Json::array({invalid}), "negative date is rejected");
  invalid = row();
  invalid["date"] = std::numeric_limits<std::uint64_t>::max();
  expectMalformed(Json::array({invalid}),
                  "date outside signed Unix-millisecond range is rejected");

  auto unknownTime = row();
  unknownTime["date"] = 0;
  outcome = parse(Json::array({unknownTime}));
  expect(outcome.status == ir::ChartRankingStatus::Succeeded &&
             outcome.ranking &&
             !outcome.ranking->entries.front().achievedAtUnixMillis,
         "zero date is normalized to unknown");

  invalid = row();
  invalid.erase("minbp");
  expectMalformed(Json::array({invalid}),
                  "missing required row field is rejected");
  invalid = row();
  invalid["maxcombo"] = "many";
  expectMalformed(Json::array({invalid}), "wrong row field type is rejected");
}

void testOrderingAndCompetitionRanks() {
  const Json shuffled = Json::array({
      row("Unknown", 150, 5, 3, 0),
      row("Late", 150, 5, 3, 200),
      row("High-Z", 160, 4, 0, 500),
      row("BP", 150, 5, 9, 500),
      row("Clear", 150, 7, 0, 500),
      row("Early", 150, 5, 3, 100),
      row("High-A", 160, 4, 0, 500),
  });
  const auto outcome = parse(shuffled);
  expect(outcome.status == ir::ChartRankingStatus::Succeeded &&
             outcome.ranking && outcome.ranking->entries.size() == 7,
         "shuffled ranking parses");
  if (!outcome.ranking || outcome.ranking->entries.size() != 7) {
    return;
  }
  const std::vector<std::string> expectedNames{
      "High-A", "High-Z", "Clear", "BP", "Early", "Late", "Unknown"};
  const std::vector<int> expectedRanks{1, 1, 3, 4, 5, 6, 7};
  for (std::size_t index = 0; index < expectedNames.size(); ++index) {
    expect(outcome.ranking->entries[index].playerName == expectedNames[index],
           "ranking sorts by score, clear, BP, time, then UTF-8 name");
    expect(outcome.ranking->entries[index].rank == expectedRanks[index],
           "ranking assigns competition ranks from complete tuples");
  }
}

void testEntryAndBodyLimits() {
  Json maximum = Json::array();
  maximum.get_ref<Json::array_t &>().reserve(ir::tachi::kMaximumRankingEntries);
  const Json oneRow = row("Player");
  for (std::size_t index = 0; index < ir::tachi::kMaximumRankingEntries;
       ++index) {
    maximum.push_back(oneRow);
  }
  auto outcome = parse(maximum);
  expect(
      outcome.status == ir::ChartRankingStatus::Succeeded && outcome.ranking &&
          outcome.ranking->entries.size() == ir::tachi::kMaximumRankingEntries,
      "20,000 ranking rows are accepted");

  maximum.push_back(oneRow);
  outcome = parse(maximum);
  expect(outcome.status == ir::ChartRankingStatus::OversizedResponse,
         "20,001 ranking rows are rejected as oversized");

  const std::string oversized(ir::tachi::kMaximumRankingResponseBytes + 1, ' ');
  outcome = ir::tachi::parseRankingResponse(oversized, query());
  expect(outcome.status == ir::ChartRankingStatus::OversizedResponse,
         "body above eight MiB is rejected before parsing");
}

void testEnvelopeValidation() {
  auto outcome = ir::tachi::parseRankingResponse("not-json", query());
  expect(outcome.status == ir::ChartRankingStatus::MalformedResponse,
         "malformed JSON is rejected");
  outcome = ir::tachi::parseRankingResponse(
      R"({"success":false,"description":"nope"})", query());
  expect(outcome.status == ir::ChartRankingStatus::MalformedResponse,
         "success false is rejected");
  outcome =
      ir::tachi::parseRankingResponse(R"({"success":true,"body":{}})", query());
  expect(outcome.status == ir::ChartRankingStatus::MalformedResponse,
         "non-array ranking body is rejected");
}

} // namespace

int main() {
  testEmptyAndAuthenticatedRanking();
  testEveryClearIndex();
  testPlayerNameValidation();
  testDuplicateCurrentUserAndNoteMismatch();
  testNumericAndRowBounds();
  testOrderingAndCompetitionRanks();
  testEntryAndBodyLimits();
  testEnvelopeValidation();

  if (failures != 0) {
    std::cerr << failures << " Tachi ranking parser test(s) failed\n";
    return 1;
  }
  std::cout << "Tachi ranking parser tests passed\n";
  return 0;
}
