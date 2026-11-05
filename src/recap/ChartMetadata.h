#pragma once
#include "PlayRecap.h"
#include "../bms_parser.hpp"
#include <sstream>

namespace recap {
struct ChartMetadata {
  std::string title, difficulty = "Lv. —";
  std::filesystem::path jacket;
};
inline ChartMetadata chartMetadata(const bms_parser::ChartMeta &meta,
                                   std::string tableLabels, bool artworkAvailable) {
  ChartMetadata result;
  result.title = meta.Title + (meta.SubTitle.empty() ? "" : " " + meta.SubTitle);
  if (!tableLabels.empty()) result.difficulty = std::move(tableLabels);
  else if (meta.PlayLevel > 0) {
    std::ostringstream level;
    level << "Lv. " << meta.PlayLevel;
    result.difficulty = level.str();
  }
  const auto artwork = !meta.StageFile.empty() ? meta.StageFile : meta.Banner;
  if (artworkAvailable && !artwork.empty()) result.jacket = meta.Folder / artwork;
  return result;
}
inline void applyChartMetadata(Upgrade &upgrade, const ChartMetadata &metadata) {
  if (!metadata.title.empty()) upgrade.title = metadata.title;
  upgrade.difficulty = metadata.difficulty;
  upgrade.jacket = metadata.jacket;
}
} // namespace recap
