#pragma once

#include "../music_select/MusicSelectBarManager.h"

#include <algorithm>
#include <functional>

inline void restoreMusicSelectDirectory(
    MusicSelectBarManager &bars, const MusicSelectBarManagerReadView &previous,
    const std::function<bool(const MusicSelectBar &)> &loadDirectory,
    const std::function<bool(const MusicSelectBarId &)> &reopenSameFolder) {
  for (const auto &directoryId : previous.directory) {
    const auto current = bars.readView();
    const auto found = std::ranges::find(current.rows, directoryId,
                                        &MusicSelectBar::id);
    if (found == current.rows.end()) {
      const auto transient = std::ranges::find(
          previous.directoryBars, directoryId, &MusicSelectBar::id);
      constexpr std::string_view prefix = "same-folder:";
      if (transient == previous.directoryBars.end() ||
          transient->kind != skin::MusicSelectBarKind::SameFolder ||
          !directoryId.value.starts_with(prefix) ||
          !reopenSameFolder({directoryId.value.substr(prefix.size())})) break;
      continue;
    }
    if (!found->childrenLoaded && !loadDirectory(*found)) break;
    if (!bars.select(directoryId) || !bars.open(directoryId)) break;
  }
  if (previous.selectedIndex < previous.rows.size()) {
    (void)bars.select(previous.rows[previous.selectedIndex].id);
  }
}
