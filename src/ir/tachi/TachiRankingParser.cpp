#include "TachiRankingParser.h"

#include "../IrOutboxModels.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ir::tachi {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumRankingPageEntries = 100;
constexpr std::size_t kMaximumChartIdBytes = 256;

std::string malformedDiagnostic(std::string_view message) {
  return sanitizeDiagnostic(message);
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

const Json *successfulBody(const Json &document) {
  if (!document.is_object()) {
    return nullptr;
  }
  const auto success = document.find("success");
  const auto body = document.find("body");
  if (success == document.end() || !success->is_boolean() ||
      !success->get<bool>() || body == document.end() || !body->is_object()) {
    return nullptr;
  }
  return &*body;
}

std::optional<std::string_view> gameFor(const IrChartQuery &query) {
  if (query.keyMode == 7) {
    return "bms-7k";
  }
  if (query.keyMode == 14) {
    return "bms-14k";
  }
  return std::nullopt;
}

bool validChartId(std::string_view value) {
  if (value.empty() || value.size() > kMaximumChartIdBytes) {
    return false;
  }
  return std::ranges::none_of(value, [](unsigned char character) {
    return character <= 0x20U || character == 0x7fU;
  });
}

bool validUtf8Name(std::string_view value) {
  std::size_t codePoints = 0;
  for (std::size_t offset = 0; offset < value.size();) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::uint32_t codePoint = 0;
    std::size_t width = 0;
    if (first <= 0x7fU) {
      codePoint = first;
      width = 1;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      codePoint = first & 0x1fU;
      width = 2;
    } else if (first >= 0xe0U && first <= 0xefU) {
      codePoint = first & 0x0fU;
      width = 3;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      codePoint = first & 0x07U;
      width = 4;
    } else {
      return false;
    }
    if (offset + width > value.size()) {
      return false;
    }
    for (std::size_t index = 1; index < width; ++index) {
      const auto continuation =
          static_cast<unsigned char>(value[offset + index]);
      if ((continuation & 0xc0U) != 0x80U) {
        return false;
      }
      codePoint = (codePoint << 6U) | (continuation & 0x3fU);
    }
    if ((width == 3 && codePoint < 0x800U) ||
        (width == 3 && codePoint >= 0xd800U && codePoint <= 0xdfffU) ||
        (width == 4 && codePoint < 0x10000U) || codePoint > 0x10ffffU ||
        codePoint <= 0x1fU || (codePoint >= 0x7fU && codePoint <= 0x9fU)) {
      return false;
    }
    ++codePoints;
    if (codePoints > kMaximumPlayerNameCodePoints) {
      return false;
    }
    offset += width;
  }
  return codePoints > 0;
}

std::optional<int> mapLamp(std::string_view lamp) {
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

std::optional<std::map<std::int64_t, std::string>>
parseUsers(const Json &users) {
  if (!users.is_array() || users.size() > kMaximumRankingPageEntries) {
    return std::nullopt;
  }
  std::map<std::int64_t, std::string> parsed;
  for (const auto &user : users) {
    if (!user.is_object()) {
      return std::nullopt;
    }
    const auto id = requiredInteger(user, "id");
    const auto username = user.find("username");
    if (!id || *id <= 0 || username == user.end() || !username->is_string()) {
      return std::nullopt;
    }
    const auto &name = username->get_ref<const std::string &>();
    if (!validUtf8Name(name) || !parsed.emplace(*id, name).second) {
      return std::nullopt;
    }
  }
  return parsed;
}

std::optional<IrChartRankingEntry>
parsePb(const Json &pb, const IrChartQuery &query,
        std::string_view expectedChartId, std::int64_t authenticatedUserId,
        const std::map<std::int64_t, std::string> &users, int &outOf) {
  if (!pb.is_object()) {
    return std::nullopt;
  }
  const auto expectedGame = gameFor(query);
  const auto game = pb.find("game");
  const auto chartId = pb.find("chartID");
  const auto userId = requiredInteger(pb, "userID");
  const auto rankingData = pb.find("rankingData");
  const auto scoreData = pb.find("scoreData");
  if (!expectedGame || game == pb.end() || !game->is_string() ||
      game->get_ref<const std::string &>() != *expectedGame ||
      chartId == pb.end() || !chartId->is_string() ||
      chartId->get_ref<const std::string &>() != expectedChartId || !userId ||
      *userId <= 0 || rankingData == pb.end() || !rankingData->is_object() ||
      scoreData == pb.end() || !scoreData->is_object()) {
    return std::nullopt;
  }
  const auto user = users.find(*userId);
  if (user == users.end()) {
    return std::nullopt;
  }

  const auto rank = requiredInteger(*rankingData, "rank");
  const auto rowOutOf = requiredInteger(*rankingData, "outOf");
  if (!rank || !rowOutOf || *rank <= 0 || *rowOutOf <= 0 || *rank > *rowOutOf ||
      *rank > std::numeric_limits<int>::max() ||
      *rowOutOf > static_cast<std::int64_t>(kMaximumRankingEntries)) {
    return std::nullopt;
  }
  if (outOf == 0) {
    outOf = static_cast<int>(*rowOutOf);
  } else if (outOf != *rowOutOf) {
    return std::nullopt;
  }

  const auto score = requiredInteger(*scoreData, "score");
  const auto lamp = scoreData->find("lamp");
  const auto maximumScore = static_cast<std::int64_t>(query.totalNotes) * 2;
  if (!score || *score < 0 || *score > maximumScore ||
      *score > std::numeric_limits<int>::max() || lamp == scoreData->end() ||
      !lamp->is_string()) {
    return std::nullopt;
  }
  const auto clearType = mapLamp(lamp->get_ref<const std::string &>());
  if (!clearType) {
    return std::nullopt;
  }

  const auto optional = scoreData->find("optional");
  if (optional == scoreData->end() || !optional->is_object()) {
    return std::nullopt;
  }
  std::optional<int> pGreat;
  std::optional<int> great;
  std::optional<int> good;
  std::optional<int> bad;
  std::optional<int> poor;
  const auto judgements = scoreData->find("judgements");
  if (judgements != scoreData->end() && !judgements->is_null()) {
    if (!judgements->is_object() ||
        !readOptionalNonnegativeInt(*judgements, "pgreat", pGreat) ||
        !readOptionalNonnegativeInt(*judgements, "great", great) ||
        !readOptionalNonnegativeInt(*judgements, "good", good) ||
        !readOptionalNonnegativeInt(*judgements, "bad", bad) ||
        !readOptionalNonnegativeInt(*judgements, "poor", poor)) {
      return std::nullopt;
    }
  }
  std::optional<int> epg;
  std::optional<int> lpg;
  std::optional<int> egr;
  std::optional<int> lgr;
  std::optional<int> egd;
  std::optional<int> lgd;
  std::optional<int> ebd;
  std::optional<int> lbd;
  std::optional<int> epr;
  std::optional<int> lpr;
  std::optional<int> badPoints;
  std::optional<int> maxCombo;
  if (!readOptionalNonnegativeInt(*optional, "epg", epg) ||
      !readOptionalNonnegativeInt(*optional, "lpg", lpg) ||
      !readOptionalNonnegativeInt(*optional, "egr", egr) ||
      !readOptionalNonnegativeInt(*optional, "lgr", lgr) ||
      !readOptionalNonnegativeInt(*optional, "egd", egd) ||
      !readOptionalNonnegativeInt(*optional, "lgd", lgd) ||
      !readOptionalNonnegativeInt(*optional, "ebd", ebd) ||
      !readOptionalNonnegativeInt(*optional, "lbd", lbd) ||
      !readOptionalNonnegativeInt(*optional, "epr", epr) ||
      !readOptionalNonnegativeInt(*optional, "lpr", lpr) ||
      !readOptionalNonnegativeInt(*optional, "bp", badPoints) ||
      !readOptionalNonnegativeInt(*optional, "maxCombo", maxCombo)) {
    return std::nullopt;
  }

  const auto validatePair =
      [totalNotes = query.totalNotes](std::optional<int> &early,
                                     std::optional<int> &late,
                                     const std::optional<int> &total) {
        if (early.has_value() != late.has_value()) {
          early.reset();
          late.reset();
          return;
        }
        if (!early) {
          return;
        }
        const std::int64_t sum = static_cast<std::int64_t>(*early) + *late;
        if (sum > totalNotes || (total && sum != *total)) {
          early.reset();
          late.reset();
        }
      };
  validatePair(epg, lpg, pGreat);
  validatePair(egr, lgr, great);
  validatePair(egd, lgd, good);
  validatePair(ebd, lbd, bad);
  validatePair(epr, lpr, poor);

  if (!pGreat && !great) {
    const std::array timingPresent{epg.has_value(), lpg.has_value(),
                                   egr.has_value(), lgr.has_value()};
    const bool hasAnyTiming =
        std::ranges::any_of(timingPresent, [](bool value) { return value; });
    const bool hasAllTiming =
        std::ranges::all_of(timingPresent, [](bool value) { return value; });
    if (hasAllTiming) {
      const std::int64_t timingPGreat =
          static_cast<std::int64_t>(*epg) + *lpg;
      const std::int64_t timingGreat =
          static_cast<std::int64_t>(*egr) + *lgr;
      if (timingPGreat + timingGreat > query.totalNotes ||
          timingPGreat * 2 + timingGreat != *score) {
        epg.reset();
        lpg.reset();
        egr.reset();
        lgr.reset();
      }
    } else if (hasAnyTiming) {
      epg.reset();
      lpg.reset();
      egr.reset();
      lgr.reset();
    }
  }

  std::optional<std::int64_t> achievedAt;
  const auto time = pb.find("timeAchieved");
  if (time == pb.end()) {
    return std::nullopt;
  }
  if (!time->is_null()) {
    const auto parsed = jsonInteger(*time);
    if (!parsed || *parsed < 0) {
      return std::nullopt;
    }
    if (*parsed > 0) {
      achievedAt = *parsed;
    }
  }

  return IrChartRankingEntry{
      .rank = static_cast<int>(*rank),
      .providerEntryId = std::to_string(*userId),
      .playerName = user->second,
      .score = static_cast<int>(*score),
      .maxScore = static_cast<int>(maximumScore),
      .pGreat = pGreat,
      .great = great,
      .good = good,
      .bad = bad,
      .poor = poor,
      .earlyPGreat = epg,
      .latePGreat = lpg,
      .earlyGreat = egr,
      .lateGreat = lgr,
      .earlyGood = egd,
      .lateGood = lgd,
      .earlyBad = ebd,
      .lateBad = lbd,
      .earlyPoor = epr,
      .latePoor = lpr,
      .clearType = *clearType,
      .badPoints = badPoints,
      .maxCombo = maxCombo,
      .achievedAtUnixMillis = achievedAt,
      .currentUser = *userId == authenticatedUserId,
  };
}

} // namespace

TachiRankingIdentityOutcome
parseRankingIdentityResponse(std::string_view body) noexcept {
  try {
    if (body.size() > kMaximumRankingResponseBytes) {
      return {.status = ChartRankingStatus::OversizedResponse,
              .diagnostic = malformedDiagnostic(
                  "Tachi identity response exceeded the size limit")};
    }
    const Json document = Json::parse(body);
    const Json *responseBody = successfulBody(document);
    if (!responseBody) {
      return {.diagnostic = malformedDiagnostic(
                  "Tachi identity response envelope is invalid")};
    }
    const auto userId = requiredInteger(*responseBody, "whoami");
    if (!userId || *userId <= 0) {
      return {.diagnostic = malformedDiagnostic(
                  "Tachi identity response has no authenticated user")};
    }
    return {.status = ChartRankingStatus::Succeeded, .userId = *userId};
  } catch (...) {
    return {.diagnostic =
                malformedDiagnostic("Tachi identity response parsing failed")};
  }
}

TachiChartResolveOutcome
parseChartResolveResponse(std::string_view body,
                          const IrChartQuery &query) noexcept {
  try {
    if (body.size() > kMaximumRankingResponseBytes) {
      return {.status = ChartRankingStatus::OversizedResponse,
              .diagnostic = malformedDiagnostic(
                  "Tachi chart response exceeded the size limit")};
    }
    const Json document = Json::parse(body);
    const Json *responseBody = successfulBody(document);
    const auto expectedGame = gameFor(query);
    if (!responseBody || !expectedGame) {
      return {.diagnostic = malformedDiagnostic(
                  "Tachi chart response envelope is invalid")};
    }
    const auto chart = responseBody->find("chart");
    if (chart == responseBody->end() || !chart->is_object()) {
      return {.diagnostic =
                  malformedDiagnostic("Tachi chart response is invalid")};
    }
    const auto chartId = chart->find("chartID");
    const auto game = chart->find("game");
    const auto data = chart->find("data");
    if (chartId == chart->end() || !chartId->is_string() ||
        !validChartId(chartId->get_ref<const std::string &>()) ||
        game == chart->end() || !game->is_string() ||
        game->get_ref<const std::string &>() != *expectedGame ||
        data == chart->end() || !data->is_object()) {
      return {.diagnostic =
                  malformedDiagnostic("Tachi resolved chart is invalid")};
    }
    const auto hash = data->find("hashSHA256");
    const auto noteCount = requiredInteger(*data, "notecount");
    if (hash == data->end() || !hash->is_string() ||
        hash->get_ref<const std::string &>() != query.chartSha256 ||
        !noteCount || *noteCount != query.totalNotes) {
      return {.diagnostic = malformedDiagnostic(
                  "Tachi resolved chart does not match the request")};
    }
    return {.status = ChartRankingStatus::Succeeded,
            .chartId = chartId->get<std::string>()};
  } catch (...) {
    return {.diagnostic =
                malformedDiagnostic("Tachi chart response parsing failed")};
  }
}

TachiRankingPageOutcome
parseRankingPageResponse(std::string_view body, const IrChartQuery &query,
                         std::string_view expectedChartId,
                         std::int64_t authenticatedUserId) noexcept {
  try {
    if (body.size() > kMaximumRankingResponseBytes) {
      return {.status = ChartRankingStatus::OversizedResponse,
              .diagnostic = malformedDiagnostic(
                  "Tachi ranking response exceeded the size limit")};
    }
    if (query.totalNotes <= 0 ||
        query.totalNotes > std::numeric_limits<int>::max() / 2 ||
        !gameFor(query) || !validChartId(expectedChartId) ||
        authenticatedUserId <= 0) {
      return {.diagnostic =
                  malformedDiagnostic("Tachi ranking request is invalid")};
    }
    const Json document = Json::parse(body);
    const Json *responseBody = successfulBody(document);
    if (!responseBody) {
      return {.diagnostic = malformedDiagnostic(
                  "Tachi ranking response envelope is invalid")};
    }
    const auto pbs = responseBody->find("pbs");
    const auto usersJson = responseBody->find("users");
    if (pbs == responseBody->end() || !pbs->is_array() ||
        usersJson == responseBody->end() ||
        pbs->size() > kMaximumRankingPageEntries) {
      return {.diagnostic =
                  malformedDiagnostic("Tachi ranking page is invalid")};
    }
    const auto users = parseUsers(*usersJson);
    if (!users) {
      return {.diagnostic =
                  malformedDiagnostic("Tachi ranking user data is invalid")};
    }

    TachiRankingPage page;
    page.entries.reserve(pbs->size());
    page.userIds.reserve(pbs->size());
    std::set<std::int64_t> seenUsers;
    int previousRank = 0;
    for (const auto &pb : *pbs) {
      const auto userId = requiredInteger(pb, "userID");
      if (!userId || !seenUsers.insert(*userId).second) {
        return {.diagnostic = malformedDiagnostic(
                    "Tachi ranking page has duplicate users")};
      }
      const auto rankingData = pb.find("rankingData");
      if (rankingData != pb.end() && rankingData->is_object()) {
        const auto outOf = requiredInteger(*rankingData, "outOf");
        if (outOf &&
            *outOf > static_cast<std::int64_t>(kMaximumRankingEntries)) {
          return {.status = ChartRankingStatus::OversizedResponse,
                  .diagnostic = malformedDiagnostic(
                      "Tachi ranking has too many entries")};
        }
      }
      auto entry = parsePb(pb, query, expectedChartId, authenticatedUserId,
                           *users, page.outOf);
      if (!entry || entry->rank < previousRank) {
        return {.diagnostic =
                    malformedDiagnostic("Tachi ranking row is invalid")};
      }
      previousRank = entry->rank;
      page.userIds.push_back(*userId);
      page.entries.push_back(std::move(*entry));
    }
    return {.status = ChartRankingStatus::Succeeded, .page = std::move(page)};
  } catch (...) {
    return {.diagnostic =
                malformedDiagnostic("Tachi ranking response parsing failed")};
  }
}

} // namespace ir::tachi
