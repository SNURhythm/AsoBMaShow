#pragma once

#include "MusicSelectBarManager.h"

struct MusicSelectDirectoryRequest {
  MusicSelectBar directory;
  std::uint64_t generation = 0;
  std::uint64_t rowsRevision = 0;
  std::uint64_t libraryRevision = 0;
  std::uint64_t scoreRevision = 0;
  bool autoplay = false;

  [[nodiscard]] bool matches(
      std::uint64_t resultGeneration, const MusicSelectBarManagerReadView &view,
      std::uint64_t currentLibraryRevision,
      std::uint64_t currentScoreRevision) const {
    return generation == resultGeneration && rowsRevision == view.rowsRevision &&
           libraryRevision == currentLibraryRevision &&
           scoreRevision == currentScoreRevision &&
           view.selectedIndex < view.rowCount() &&
           view.rowAt(view.selectedIndex).id == directory.id;
  }
};
