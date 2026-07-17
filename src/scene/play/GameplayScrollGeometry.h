#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gameplay_scroll_geometry {

enum class NoteRenderLayer {
  LaneBeam,
  Landmine,
  Invisible,
  LongBody,
  Normal,
  Ghost,
};

inline constexpr uint32_t noteRenderDepth(NoteRenderLayer layer) {
  switch (layer) {
  case NoteRenderLayer::LaneBeam:
    return 180;
  case NoteRenderLayer::Landmine:
    return 184;
  case NoteRenderLayer::Invisible:
    return 185;
  case NoteRenderLayer::LongBody:
    return 190;
  case NoteRenderLayer::Normal:
    return 200;
  case NoteRenderLayer::Ghost:
    return 250;
  }
  return 0;
}

inline long long chartRenderTimeMicros(long long visualTimeMicros) {
  return visualTimeMicros;
}

inline float renderY(double itemScrollPosition,
                     double currentScrollPosition, float rxhs,
                     float judgeY) {
  return judgeY +
         static_cast<float>(itemScrollPosition - currentScrollPosition) * rxhs;
}

inline bool shouldKeepRenderTimeline(
    double previousBpm, double bpm, double stopDurationMicros,
    double previousScroll, double scroll, bool isMeasureLine,
    bool hasPlayableNote, bool hasInvisibleNote, bool hasLandmine) {
  return previousBpm != bpm || stopDurationMicros > 0.0 ||
         previousScroll != scroll || isMeasureLine || hasPlayableNote ||
         hasInvisibleNote || hasLandmine;
}

inline float initialFutureTimelineY(double timelineScrollPosition,
                                    double currentScrollPosition, float rxhs,
                                    float judgeY) {
  return renderY(timelineScrollPosition, currentScrollPosition, rxhs, judgeY);
}

inline double advanceFutureTimelineY(
    double currentY, double beatDistance, double previousScroll,
    long long previousTimeMicros, double previousStopDurationMicros,
    long long timelineTimeMicros, long long currentTimeMicros, double rxhs) {
  if (static_cast<double>(previousTimeMicros) +
          previousStopDurationMicros >
      static_cast<double>(currentTimeMicros)) {
    return currentY + beatDistance * previousScroll * rxhs;
  }
  const double travelDuration =
      static_cast<double>(timelineTimeMicros - previousTimeMicros) -
      previousStopDurationMicros;
  if (travelDuration == 0.0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return currentY + beatDistance * previousScroll *
                        static_cast<double>(timelineTimeMicros -
                                            currentTimeMicros) /
                        travelDuration * rxhs;
}

inline bool futureTimelineTraversalContinues(double y, float upperBound) {
  return y <= static_cast<double>(upperBound);
}

struct ScrollRange {
  double minimum = 0.0;
  double maximum = 0.0;
};

inline ScrollRange visibleScrollRange(double currentScrollPosition, float rxhs,
                                      float lowerBound, float upperBound,
                                      float noteHeight, float judgeY) {
  if (rxhs <= 0.0F) {
    return {-std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity()};
  }
  const double first =
      currentScrollPosition +
      static_cast<double>(lowerBound - judgeY - noteHeight) /
          static_cast<double>(rxhs);
  const double last =
      currentScrollPosition +
      static_cast<double>(upperBound - judgeY) / static_cast<double>(rxhs);
  return {std::min(first, last), std::max(first, last)};
}

inline bool noteRectangleIntersectsViewport(float y, float noteHeight,
                                            float lowerBound,
                                            float upperBound) {
  return y + noteHeight >= lowerBound && y <= upperBound;
}

struct NoteRectangleClip {
  bool visible = false;
  float y = 0.0F;
  float height = 0.0F;
  float bottomTextureFraction = 0.0F;
};

struct RenderRectangle {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
};

struct NoteOutlineRectangles {
  std::array<RenderRectangle, 4> rectangles{};
  std::size_t count = 0;
};

inline NoteRectangleClip clipNoteRectangle(long long noteTimeMicros,
                                           long long currentTimeMicros,
                                           float y, float noteHeight,
                                           float judgeY) {
  if (noteTimeMicros < currentTimeMicros || y >= judgeY) {
    return {.visible = true,
            .y = y,
            .height = noteHeight,
            .bottomTextureFraction = 1.0F};
  }

  const float top = y + noteHeight;
  if (noteHeight <= 0.0F || top <= judgeY) {
    return {};
  }

  const float clippedHeight = top - judgeY;
  return {.visible = true,
          .y = judgeY,
          .height = clippedHeight,
          .bottomTextureFraction = clippedHeight / noteHeight};
}

inline NoteOutlineRectangles noteOutlineRectangles(
    float x, float y, float width, float height, float borderThickness,
    const NoteRectangleClip &clip) {
  NoteOutlineRectangles outline;
  if (!clip.visible || width <= 0.0F || height <= 0.0F ||
      clip.height <= 0.0F) {
    return outline;
  }

  const float border = std::min(
      {borderThickness, width * 0.5F, height * 0.5F});
  if (border <= 0.0F) {
    return outline;
  }

  const std::array<RenderRectangle, 4> candidates{
      RenderRectangle{x, y, width, border},
      RenderRectangle{x, y + height - border, width, border},
      RenderRectangle{x, y, border, height},
      RenderRectangle{x + width - border, y, border, height}};
  const float clipBottom = clip.y;
  const float clipTop = clip.y + clip.height;
  for (const auto &candidate : candidates) {
    const float bottom = std::max(candidate.y, clipBottom);
    const float top = std::min(candidate.y + candidate.height, clipTop);
    if (top <= bottom) {
      continue;
    }
    outline.rectangles[outline.count++] = {
        .x = candidate.x,
        .y = bottom,
        .width = candidate.width,
        .height = top - bottom};
  }
  return outline;
}

inline bool shouldDrawNoteRectangle(long long noteTimeMicros,
                                    long long currentTimeMicros, float y,
                                    float noteHeight, float judgeY) {
  return clipNoteRectangle(noteTimeMicros, currentTimeMicros, y, noteHeight,
                           judgeY)
      .visible;
}

inline bool shouldDrawMeasureLine(long long timelineTimeMicros,
                                  long long currentTimeMicros, float y,
                                  float judgeY, float upperBound) {
  return timelineTimeMicros >= currentTimeMicros && y >= judgeY &&
         y <= upperBound;
}

} // namespace gameplay_scroll_geometry
