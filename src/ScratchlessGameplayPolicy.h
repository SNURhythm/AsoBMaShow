#pragma once

#include "AppSettings.h"
#include "GameplayKeyMode.h"
#include "repositories/ChartRepository.h"

#include <algorithm>
#include <span>

namespace gameplay {

[[nodiscard]] inline bool scratchlessAllowed(
    const AppSettings &settings, std::span<const std::string> chartTableUrls) {
  switch (settings.scratchlessMode) {
  case AppSettings::ScratchlessMode::Disabled: return false;
  case AppSettings::ScratchlessMode::Enabled: return true;
  case AppSettings::ScratchlessMode::SelectedTables:
    return std::ranges::any_of(chartTableUrls, [&](const auto &url) {
      return std::ranges::find(settings.scratchlessTableUrls, url) !=
             settings.scratchlessTableUrls.end();
    });
  }
  return false;
}

// Resolve membership once per attempt, independently of the selector folder.
[[nodiscard]] inline bool scratchlessAllowed(
    const AppSettings &settings, const bms_parser::ChartMeta &meta,
    ChartRepository &repository) {
  if (settings.scratchlessMode != AppSettings::ScratchlessMode::SelectedTables)
    return scratchlessAllowed(settings, {});
  if (settings.scratchlessTableUrls.empty() || meta.IsDP ||
      (meta.KeyMode != 5 && meta.KeyMode != 7)) return false;
  auto session = repository.OpenSession();
  return session && scratchlessAllowed(settings, session->DifficultyTableSourcesForChart(meta));
}

[[nodiscard]] inline int presentationKeyMode(
    const bms_parser::Chart &chart, const AppSettings &settings,
    ChartRepository &repository) {
  return presentationKeyMode(chart, scratchlessAllowed(settings, chart.Meta, repository));
}

} // namespace gameplay
