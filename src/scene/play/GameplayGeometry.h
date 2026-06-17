#pragma once

namespace gameplay_geometry {

constexpr float kDefaultPlayAreaWidth = 8.0f;
constexpr float kPlayAreaWidth = kDefaultPlayAreaWidth;
constexpr float kPlayAreaCenterX = kDefaultPlayAreaWidth * 0.5f;
constexpr float kSevenKeyScratchLaneCount = 8.0f;
constexpr float kStandardNoteWidth =
    kDefaultPlayAreaWidth / kSevenKeyScratchLaneCount;

inline float playAreaLeft(float playAreaWidth) {
  return kPlayAreaCenterX - playAreaWidth * 0.5f;
}

inline float standardNoteWidth(float playAreaWidth) {
  return playAreaWidth / kSevenKeyScratchLaneCount;
}

} // namespace gameplay_geometry
