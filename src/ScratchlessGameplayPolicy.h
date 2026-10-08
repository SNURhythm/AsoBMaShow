#pragma once

#include "AppSettings.h"
#include "GameplayKeyMode.h"
#include "repositories/ChartRepository.h"

#include <algorithm>
#include <span>

namespace gameplay {

[[nodiscard]] inline bool scratchlessAllowed(
    const AppSettings &settings, int keyMode,
    std::span<const std::string> chartTableUrls) {
  if (keyMode != 5 && keyMode != 7) return false;
  const auto &policy = settings.scratchlessForKeyMode(keyMode);
  switch (policy.mode) {
  case AppSettings::ScratchlessMode::Disabled: return false;
  case AppSettings::ScratchlessMode::Enabled: return true;
  case AppSettings::ScratchlessMode::SelectedTables:
    return std::ranges::any_of(chartTableUrls, [&](const auto &url) {
      return std::ranges::find(policy.tableUrls, url) !=
             policy.tableUrls.end();
    });
  }
  return false;
}

// Resolve membership once per attempt, independently of the selector folder.
[[nodiscard]] inline bool scratchlessAllowed(
    const AppSettings &settings, const bms_parser::ChartMeta &meta,
    ChartRepository &repository) {
  if (meta.IsDP || (meta.KeyMode != 5 && meta.KeyMode != 7)) return false;
  const auto &policy = settings.scratchlessForKeyMode(meta.KeyMode);
  if (policy.mode != AppSettings::ScratchlessMode::SelectedTables)
    return scratchlessAllowed(settings, meta.KeyMode, {});
  if (policy.tableUrls.empty()) return false;
  auto session = repository.OpenSession();
  return session && scratchlessAllowed(
      settings, meta.KeyMode, session->DifficultyTableSourcesForChart(meta));
}

[[nodiscard]] inline int presentationKeyMode(
    const bms_parser::Chart &chart, const AppSettings &settings,
    ChartRepository &repository) {
  return presentationKeyMode(chart, scratchlessAllowed(settings, chart.Meta, repository));
}

} // namespace gameplay
