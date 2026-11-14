#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace settings_search {
enum class Destination { Profile, Calibration, Scroll, Input, Background, General, Audio, Display, Tables, Library, Ir, Legal, Skin, BuiltInLane, Notes, Effects, Hud, Overlays, Manage };
struct Result { Destination destination; std::string_view title; std::string_view aliases; };
inline constexpr auto entries = std::to_array<Result>({
  {Destination::Skin, "settings.clarity.skin_edit", "skin options fast slow fast/slow timing +/- ms +-ms ±ms judge detail wmix lr2 beatoraja framing viewport"},
  {Destination::Overlays, "settings.clarity.overlays", "shared hud judgement indicator counter histogram colors colour"},
  {Destination::Hud, "settings.clarity.builtin_hud", "built-in builtin judgement combo gauge pacemaker timing text size bold position"},
  {Destination::BuiltInLane, "settings.clarity.builtin_lane", "built-in builtin lane width length angle opacity scratch judge line measure line"},
  {Destination::Notes, "settings.notes.title", "built-in builtin notes color colour thickness mine long note"},
  {Destination::Effects, "settings.skins.effects.label", "built-in builtin keybomb beam particles color colour"},
  {Destination::Manage, "settings.clarity.manage", "install import skin zip package folder rescan revalidate revision diagnostics troubleshooting sound set toolbar safety"},
  {Destination::Calibration, "settings.navigation.timing.label", "calibration timing offset input audio visual delay bluetooth"},
  {Destination::Scroll, "settings.navigation.lane.label", "scroll visibility lane cover lift hidden green number hispeed speed invisible processed start indicators preview"},
  {Destination::Input, "settings.navigation.input.label", "keyboard controller gamepad touch flick tap bind key mapping scratch"},
  {Destination::Background, "settings.navigation.background.label", "background bga brightness blur video"},
  {Destination::Profile, "settings.navigation.profile.label", "player profile name switch export import"},
  {Destination::General, "settings.navigation.general.label", "language theme dark light"},
  {Destination::Audio, "settings.navigation.audio.label", "audio sound volume device buffer latency mono"},
  {Destination::Display, "settings.navigation.display.label", "display screen resolution fullscreen refresh monitor orientation"},
  {Destination::Tables, "settings.navigation.difficulty_tables.label", "difficulty tables update url"},
  {Destination::Library, "settings.navigation.bms_library.label", "bms library folder scan cache archive storage"},
  {Destination::Ir, "settings.clarity.ir", "internet ranking account login upload sync"},
  {Destination::Legal, "settings.legal.title", "licenses credits legal"},
  {Destination::Overlays, "settings.skins.judgement_indicator.label", "judgement timing histogram indicator"},
  {Destination::Overlays, "settings.skins.judgement_counter.label", "judgement totals counter"},
  {Destination::Overlays, "settings.skins.counter_position.label", "counter position top left right"},
  {Destination::Overlays, "settings.skins.indicator_y.percent_label", "indicator position height"},
  {Destination::Hud, "settings.skins.gauge_position.label", "gauge position"},
  {Destination::Hud, "settings.skins.judgement_feedback.label", "judgement combo timing text"},
  {Destination::BuiltInLane, "settings.skins.lane_length.label", "lane length"},
  {Destination::BuiltInLane, "settings.skins.lane_angle_deg.label", "lane angle"},
  {Destination::Effects, "settings.skins.beam_length.percent_label", "beam length"},
  {Destination::Effects, "settings.keybomb.title", "keybomb"},
  {Destination::Skin, "settings.clarity.timing", "custom timing fast slow milliseconds ±ms"},
  {Destination::Skin, "settings.clarity.framing", "custom framing viewport zoom"},
  {Destination::Skin, "settings.clarity.options", "custom skin options files offsets"},
  {Destination::Scroll, "settings.lane.covers.title", "lane cover lift hidden"},
  {Destination::Scroll, "settings.lane.visible_time.label", "visible time green number speed"},
  {Destination::Scroll, "settings.visual.invisible_notes.label", "invisible notes"},
  {Destination::Scroll, "settings.visual.mark_processed_notes.label", "processed notes"},
  {Destination::Scroll, "settings.visual.start_lane_indicators.label", "start lane indicators"},
});

inline std::string folded(std::string_view text) {
  std::string result(text);
  for (auto &c : result) if (static_cast<unsigned char>(c) < 128)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}

template<class Resolve>
std::vector<Result> search(std::string_view query, Resolve resolve) {
  const auto normalized = folded(query);
  std::vector<std::string> words;
  for (std::size_t start = normalized.find_first_not_of(" \t\r\n"); start != std::string::npos;) {
    const auto end = normalized.find_first_of(" \t\r\n", start);
    words.push_back(normalized.substr(start, end - start));
    start = end == std::string::npos ? end : normalized.find_first_not_of(" \t\r\n", end);
  }
  if (words.empty()) return {};
  std::vector<Result> results;
  for (const auto &entry : entries) {
    const auto haystack = folded(resolve(entry.title)) + " " + std::string(entry.aliases);
    if (std::ranges::all_of(words, [&](const auto &word) { return haystack.find(word) != std::string::npos; }))
      results.push_back(entry);
  }
  return results;
}
} // namespace settings_search
