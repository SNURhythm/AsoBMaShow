#pragma once
#include "scene/play/GaugeEvidence.h"
#include "nlohmann/json.hpp"
#include <limits>
#include <optional>
#include <string>
#include <string_view>

inline std::string serializeGaugeEvidence(const GaugeEvidence &evidence) {
  if (!evidence.recorded) return "{}";
  return nlohmann::ordered_json{{"version", 1}, {"passedNotes", evidence.passedNotes},
      {"survivalMask", evidence.survivalMask}, {"deathNotes", evidence.deathPassedNotes},
      {"values", evidence.values}}.dump();
}
inline std::optional<GaugeEvidence> deserializeGaugeEvidence(std::string_view text) {
  if (text == "{}") return GaugeEvidence{}; // Migrated records have zero evidence.
  if (text.size() > 2048) return {};
  try {
    const auto json = nlohmann::ordered_json::parse(text);
    if (!json.is_object() || json.size() != 5 || json.at("version") != 1 ||
        !json.at("passedNotes").is_number_integer() || !json.at("survivalMask").is_number_integer() ||
        json.at("passedNotes") < 0 || json.at("passedNotes") > std::numeric_limits<int>::max() ||
        json.at("survivalMask") < 0 || json.at("survivalMask") >= (1u << kGaugeTypeCount) ||
        !json.at("deathNotes").is_array() || json.at("deathNotes").size() != kGaugeTypeCount ||
        !json.at("values").is_array() || json.at("values").size() != kGaugeTypeCount) return {};
    GaugeEvidence result{.recorded=true, .survivalMask=json.at("survivalMask").get<std::uint16_t>(),
                         .passedNotes=json.at("passedNotes").get<int>()};
    for (std::size_t i = 0; i < kGaugeTypeCount; ++i) {
      const auto &death = json.at("deathNotes").at(i);
      const auto &value = json.at("values").at(i);
      if (!death.is_number_integer() || death < -1 || death > result.passedNotes || !value.is_number()) return {};
      result.deathPassedNotes[i] = death.get<int>();
      result.values[i] = value.get<float>();
    }
    if (!result.valid(std::numeric_limits<int>::max()) || serializeGaugeEvidence(result) != text) return {};
    return result;
  } catch (...) { return {}; }
}
