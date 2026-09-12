#pragma once

#include "bms_parser.hpp"

#include <utility>

inline void applyDoublePlayFlipToChart(bms_parser::Chart &chart) {
  if (!chart.Meta.IsDP) {
    return;
  }
  const auto flip = [](auto &notes) {
    const std::size_t playerLaneCount = notes.size() / 2;
    for (std::size_t lane = 0; lane < playerLaneCount; ++lane) {
      std::swap(notes[lane], notes[lane + playerLaneCount]);
    }
    for (std::size_t lane = 0; lane < notes.size(); ++lane) {
      if (notes[lane] != nullptr) {
        notes[lane]->Lane = static_cast<int>(lane);
      }
    }
  };
  for (auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (auto *timeline : measure->TimeLines) {
      if (timeline != nullptr) {
        flip(timeline->Notes);
        flip(timeline->InvisibleNotes);
        flip(timeline->LandmineNotes);
      }
    }
  }
}
