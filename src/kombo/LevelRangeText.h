#pragma once
#include "KomboModel.h"
#include <algorithm>
#include <optional>

namespace kombo {
struct LevelRange { std::string label; std::vector<Level> levels; };
inline std::string formatLevelRanges(const kombo::Assignment &a,
                      const std::map<std::string, std::map<std::string, int>> &order,
                      std::vector<LevelRange> *ranges = nullptr) {
  struct Entry { std::string name; std::optional<int> position; Level level; };
  struct Group { std::string source, table; std::vector<Entry> levels; };
  std::vector<Group> groups;
  const std::string separator = " · ";
  for (const auto &level : a.levels) {
    const auto split = level.label.rfind(separator);
    const auto table = split == std::string::npos ? std::string{} : level.label.substr(0, split);
    const auto name = split == std::string::npos
        ? (level.label.empty() ? level.level : level.label) : level.label.substr(split + separator.size());
    Entry entry{.name=name,.level=level};
    if (const auto source = order.find(level.source); source != order.end()) {
      if (const auto position = source->second.find(level.level); position != source->second.end())
        entry.position = position->second;
    }
    auto group = std::find_if(groups.begin(), groups.end(), [&](const auto &g) {
      return g.source == level.source && g.table == table;
    });
    if (group == groups.end()) groups.push_back({level.source, table, {std::move(entry)}});
    else group->levels.push_back(std::move(entry));
  }
  std::string text;
  for (auto &group : groups) {
    if (!text.empty()) text += " / ";
    if (!group.table.empty()) text += group.table + separator;
    std::stable_sort(group.levels.begin(), group.levels.end(), [](const auto &left, const auto &right) {
      if (left.position.has_value() != right.position.has_value()) return left.position.has_value();
      return left.position && *left.position < *right.position;
    });
    for (std::size_t first = 0; first < group.levels.size();) {
      std::size_t last = first;
      while (last + 1 < group.levels.size()) {
        const auto &current = group.levels[last];
        const auto &next = group.levels[last + 1];
        if (!current.position || !next.position || *current.position + 1 != *next.position) break;
        ++last;
      }
      if (ranges) {
        LevelRange range;
        range.label = (group.table.empty() ? "" : group.table + separator) + group.levels[first].name;
        if (last > first) range.label += "~" + group.levels[last].name;
        for (auto i = first; i <= last; ++i) range.levels.push_back(group.levels[i].level);
        ranges->push_back(std::move(range));
      }
      if (first > 0) text += ", ";
      text += group.levels[first].name;
      if (last > first) text += "~" + group.levels[last].name;
      first = last + 1;
    }
  }
  return text;
}
} // namespace kombo
