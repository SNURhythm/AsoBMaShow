#include "TachiRankingParser.h"

#include "TachiBatchManual.h"
#include "../IrOutboxModels.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ir::tachi {
namespace {

using Json = nlohmann::json;

struct ParsedRankingEntry {
  IrChartRankingEntry entry;
  RankingTuple tuple;
};

ChartRankingOutcome malformed(std::string_view diagnostic) {
  return {.status = ChartRankingStatus::MalformedResponse,
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

ChartRankingOutcome oversized(std::string_view diagnostic) {
  return {.status = ChartRankingStatus::OversizedResponse,
          .diagnostic = sanitizeDiagnostic(diagnostic)};
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
        (width == 4 && codePoint < 0x10000U) || codePoint > 0x10ffffU) {
      return false;
    }
    if (codePoint <= 0x1fU || (codePoint >= 0x7fU && codePoint <= 0x9fU)) {
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

std::optional<int> mapClearIndex(std::int64_t clearIndex) {
  switch (clearIndex) {
  case 0:
  case 1:
    return kClearTypeFailedRank;
  case 2:
  case 3:
    return kClearTypeAssistedEasyClearRank;
  case 4:
    return kClearTypeEasyClearRank;
  case 5:
    return kClearTypeNormalClearRank;
  case 6:
    return kClearTypeHardClearRank;
  case 7:
    return kClearTypeExHardClearRank;
  case 8:
  case 9:
    return kClearTypeFullComboRank;
  default:
    return std::nullopt;
  }
}

std::optional<ParsedRankingEntry>
parseRow(const Json &row, const IrChartQuery &query, bool &currentUserSeen) {
  if (!row.is_object()) {
    return std::nullopt;
  }
  const auto player = row.find("player");
  if (player == row.end() || !player->is_string()) {
    return std::nullopt;
  }
  const auto &rawPlayer = player->get_ref<const std::string &>();
  const bool currentUser = rawPlayer.empty();
  if ((!currentUser && !validUtf8Name(rawPlayer)) ||
      (currentUser && currentUserSeen)) {
    return std::nullopt;
  }

  const auto notes = requiredInteger(row, "notes");
  const auto epg = requiredInteger(row, "epg");
  const auto lpg = requiredInteger(row, "lpg");
  const auto egr = requiredInteger(row, "egr");
  const auto lgr = requiredInteger(row, "lgr");
  const auto clear = requiredInteger(row, "clear");
  const auto minBp = requiredInteger(row, "minbp");
  const auto date = requiredInteger(row, "date");
  if (!notes || !epg || !lpg || !egr || !lgr || !clear || !minBp || !date ||
      *notes != query.totalNotes) {
    return std::nullopt;
  }
  const std::array judgementCounts{*epg, *lpg, *egr, *lgr};
  if (std::ranges::any_of(judgementCounts, [&](std::int64_t count) {
        return count < 0 || count > query.totalNotes;
      })) {
    return std::nullopt;
  }
  const std::int64_t perfectGreat = *epg + *lpg;
  const std::int64_t great = *egr + *lgr;
  const std::int64_t score = perfectGreat * 2 + great;
  const std::int64_t maximumScore =
      static_cast<std::int64_t>(query.totalNotes) * 2;
  if (perfectGreat + great > query.totalNotes || score < 0 ||
      score > maximumScore || score > std::numeric_limits<int>::max() ||
      *minBp < 0 || *minBp > std::numeric_limits<int>::max() || *date < 0) {
    return std::nullopt;
  }
  const auto clearType = mapClearIndex(*clear);
  if (!clearType) {
    return std::nullopt;
  }

  const auto maxComboValue = row.find("maxcombo");
  if (maxComboValue == row.end()) {
    return std::nullopt;
  }
  std::optional<int> maxCombo;
  if (!maxComboValue->is_null()) {
    const auto parsed = jsonInteger(*maxComboValue);
    if (!parsed || *parsed < 0 || *parsed > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    maxCombo = static_cast<int>(*parsed);
  }

  currentUserSeen = currentUserSeen || currentUser;
  const auto achievedAt =
      *date == 0 ? std::nullopt : std::optional<std::int64_t>(*date);
  return ParsedRankingEntry{
      .entry = {.playerName = currentUser ? "You" : rawPlayer,
                .score = static_cast<int>(score),
                .maxScore = static_cast<int>(maximumScore),
                .clearType = *clearType,
                .badPoints = static_cast<int>(*minBp),
                .maxCombo = maxCombo,
                .achievedAtUnixMillis = achievedAt,
                .currentUser = currentUser},
      .tuple = {.score = static_cast<int>(score),
                .clearIndex = static_cast<int>(*clear),
                .badPoints = static_cast<int>(*minBp),
                .achievedAt = achievedAt}};
}

bool utf8ByteOrder(std::string_view left, std::string_view right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](char leftByte, char rightByte) {
        return static_cast<unsigned char>(leftByte) <
               static_cast<unsigned char>(rightByte);
      });
}

bool rankingLess(const ParsedRankingEntry &left,
                 const ParsedRankingEntry &right) {
  if (left.tuple.score != right.tuple.score) {
    return left.tuple.score > right.tuple.score;
  }
  if (left.tuple.clearIndex != right.tuple.clearIndex) {
    return left.tuple.clearIndex > right.tuple.clearIndex;
  }
  if (left.tuple.badPoints != right.tuple.badPoints) {
    return left.tuple.badPoints > right.tuple.badPoints;
  }
  if (left.tuple.achievedAt != right.tuple.achievedAt) {
    if (!left.tuple.achievedAt) {
      return false;
    }
    if (!right.tuple.achievedAt) {
      return true;
    }
    return *left.tuple.achievedAt < *right.tuple.achievedAt;
  }
  return utf8ByteOrder(left.entry.playerName, right.entry.playerName);
}

} // namespace

ChartRankingOutcome parseRankingResponse(std::string_view body,
                                         const IrChartQuery &query) noexcept {
  try {
    if (body.size() > kMaximumRankingResponseBytes) {
      return oversized("Tachi ranking response exceeded the size limit");
    }
    const Json document = Json::parse(body);
    if (!document.is_object()) {
      return malformed("Tachi ranking response is not an object");
    }
    const auto success = document.find("success");
    const auto responseBody = document.find("body");
    if (success == document.end() || !success->is_boolean() ||
        !success->get<bool>() || responseBody == document.end() ||
        !responseBody->is_array()) {
      return malformed("Tachi ranking response envelope is invalid");
    }
    if (responseBody->size() > kMaximumRankingEntries) {
      return oversized("Tachi ranking response has too many entries");
    }
    if (query.totalNotes <= 0 ||
        query.totalNotes > std::numeric_limits<int>::max() / 2) {
      return malformed("Tachi ranking chart note count is invalid");
    }

    std::vector<ParsedRankingEntry> parsed;
    parsed.reserve(responseBody->size());
    bool currentUserSeen = false;
    for (const auto &row : *responseBody) {
      auto entry = parseRow(row, query, currentUserSeen);
      if (!entry) {
        return malformed("Tachi ranking row is invalid");
      }
      parsed.push_back(std::move(*entry));
    }
    std::sort(parsed.begin(), parsed.end(), rankingLess);

    IrChartRanking ranking{.providerId = std::string(kProviderId),
                           .chart = query};
    ranking.entries.reserve(parsed.size());
    int rank = 1;
    for (std::size_t index = 0; index < parsed.size(); ++index) {
      if (index != 0 && parsed[index].tuple != parsed[index - 1].tuple) {
        rank = static_cast<int>(index + 1);
      }
      parsed[index].entry.rank = rank;
      ranking.entries.push_back(std::move(parsed[index].entry));
    }
    return {.status = ChartRankingStatus::Succeeded,
            .ranking = std::move(ranking)};
  } catch (...) {
    return malformed("Tachi ranking response parsing failed");
  }
}

} // namespace ir::tachi
