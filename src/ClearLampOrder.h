#pragma once
#include <string>
#include <string_view>

// Permanently reserved legacy alias. Never assign 150 to another clear lamp:
// old results, profiles and signed replay facts use it for Light Assist Easy.
inline constexpr int kLegacyLightAssistedEasyClearRank = 150;

template <int Rank> consteval int clearLampRank() {
  static_assert(Rank != kLegacyLightAssistedEasyClearRank,
                "150 is permanently reserved for legacy Light Assist Easy");
  return Rank;
}

inline constexpr int normalizeClearLamp(int lamp) {
  return lamp == kLegacyLightAssistedEasyClearRank ? 50 : lamp;
}
inline constexpr int clearLampOrder(int lamp) {
  return normalizeClearLamp(lamp);
}
inline constexpr int bestClearLamp(int a, int b) {
  return clearLampOrder(a) >= clearLampOrder(b) ? normalizeClearLamp(a) : normalizeClearLamp(b);
}
inline constexpr int worstClearLamp(int a, int b) {
  return clearLampOrder(a) <= clearLampOrder(b) ? normalizeClearLamp(a) : normalizeClearLamp(b);
}
inline std::string clearLampOrderSql(std::string_view expression) {
  const std::string value(expression);
  return "(CASE " + value + " WHEN 150 THEN 50 ELSE " + value + " END)";
}
