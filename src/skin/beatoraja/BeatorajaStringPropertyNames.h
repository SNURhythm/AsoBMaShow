#pragma once

#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

namespace skin {
namespace beatoraja_string_property_detail {

inline std::optional<int> namePatternIndex(std::string_view value,
                                           std::string_view prefix,
                                           std::string_view suffix,
                                           int firstId, int count,
                                           int authoredFirst = 1) noexcept {
  if (!value.starts_with(prefix) || !value.ends_with(suffix) ||
      value.size() <= prefix.size() + suffix.size()) {
    return std::nullopt;
  }
  std::string_view number = value.substr(
      prefix.size(), value.size() - prefix.size() - suffix.size());
  if (number.starts_with('+')) number.remove_prefix(1);
  if (number.empty()) return std::nullopt;
  int parsed = 0;
  for (const char character : number) {
    if (character < '0' || character > '9' ||
        parsed > (std::numeric_limits<int>::max() - (character - '0')) / 10) {
      return std::nullopt;
    }
    parsed = parsed * 10 + (character - '0');
  }
  const int offset = parsed - authoredFirst;
  return offset >= 0 && offset < count ? std::optional<int>(firstId + offset)
                                        : std::nullopt;
}

} // namespace beatoraja_string_property_detail

// Exact StringPropertyFactory name resolution, including every source
// NamePattern spelling that Java Integer.parseInt accepts for a positive
// index. This stays independent from IntegerPropertyFactory: several names
// intentionally share numeric IDs with another property family.
inline std::optional<int>
beatorajaStringPropertySelector(std::string_view name) noexcept {
  constexpr std::array<std::pair<std::string_view, int>, 26> direct{{
      {"rival", 1},          {"player", 2},
      {"target", 3},         {"title", 10},
      {"subtitle", 11},      {"fulltitle", 12},
      {"genre", 13},         {"artist", 14},
      {"subartist", 15},     {"fullartist", 16},
      {"searchword", 30},    {"skinname", 50},
      {"skinauthor", 51},    {"mode", 60},
      {"sort", 61},          {"difficulty", 62},
      {"chartreplication", 86}, {"directory", 1000},
      {"tablename", 1001},   {"tablelevel", 1002},
      {"tablefull", 1003},   {"version", 1010},
      {"irname", 1020},      {"irUserName", 1021},
      {"songhashmd5", 1030}, {"songhashsha256", 1031},
  }};
  for (const auto &[candidate, id] : direct) {
    if (name == candidate) return id;
  }

  using beatoraja_string_property_detail::namePatternIndex;
  if (const auto authored = namePatternIndex(name, "targetnamep", "", 0, 10)) {
    // StringPropertyFactory reverses this family: targetnamep1 is numeric
    // property 209 while targetnamep10 is 200.
    return 209 - *authored;
  }
  constexpr std::array<std::tuple<std::string_view, std::string_view, int,
                                  int, int>,
                       12>
      patterns{{{"key", "", 40, 10, 1},
                {"key", "", 240, 44, 11},
                {"skincategory", "", 100, 10, 1},
                {"skinitem", "", 110, 10, 1},
                {"rankingname", "", 120, 10, 1},
                {"coursetitle", "", 150, 10, 1},
                {"targetnamen", "", 210, 10, 1},
                {"practice_item", "", 1040, 16, 1},
                {"practice_item", "_label", 1060, 16, 1},
                {"practice_item", "_value", 1080, 16, 1},
                {"practice_item_label", "", 1060, 16, 1},
                {"practice_item_value", "", 1080, 16, 1}}};
  for (const auto &[prefix, suffix, first, count, authoredFirst] : patterns) {
    if (const auto id =
            namePatternIndex(name, prefix, suffix, first, count, authoredFirst)) {
      return id;
    }
  }
  return std::nullopt;
}

} // namespace skin
