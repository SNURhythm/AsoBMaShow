#pragma once

#include "../bms_parser.hpp"

#include <string_view>

// Music Select uses Beatoraja song modes: keyboard charts map to 25/50,
// and unknown modes remain eligible under every filter.
[[nodiscard]] inline int musicSelectSongMode(const bms_parser::ChartMeta &meta) noexcept {
  if (meta.KeyMode == 5 && !meta.IsDP) return 5;
  if (meta.KeyMode == 7 && !meta.IsDP) return 7;
  if (meta.KeyMode == 9 && !meta.IsDP) return 9;
  if (meta.KeyMode == 10 || (meta.KeyMode == 5 && meta.IsDP)) return 10;
  if (meta.KeyMode == 14 || (meta.KeyMode == 7 && meta.IsDP)) return 14;
  if (meta.KeyMode == 24 && !meta.IsDP) return 25;
  if (meta.KeyMode == 48 || (meta.KeyMode == 24 && meta.IsDP)) return 50;
  return 0;
}

[[nodiscard]] inline bool musicSelectModeMatches(std::string_view filter, int mode) noexcept {
  if (mode == 0 || filter == "ALL") return true;
  if (filter == "7KEY") return mode == 7;
  if (filter == "14KEY") return mode == 14;
  if (filter == "9KEY") return mode == 9;
  if (filter == "5KEY") return mode == 5;
  if (filter == "10KEY") return mode == 10;
  if (filter == "24KEY") return mode == 25;
  if (filter == "48KEY") return mode == 50;
  if (filter == "SINGLE") return mode == 5 || mode == 7;
  if (filter == "DOUBLE") return mode == 10 || mode == 14;
  return false;
}
