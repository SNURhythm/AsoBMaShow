#include "SyntheticReplayGhostOverlay.h"

#include "../../scene/play/StartLaneIndicatorGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>

namespace skin {
namespace {

std::optional<UiLogicalRect>
intersectClip(const std::optional<UiLogicalRect> &clip,
              const UiLogicalRect &bounds, bool &empty) noexcept {
  empty = false;
  if (!clip) {
    return std::nullopt;
  }
  const double left = std::max(clip->x, bounds.x);
  const double top = std::max(clip->y, bounds.y);
  const double right = std::min(clip->x + clip->width, bounds.x + bounds.width);
  const double bottom =
      std::min(clip->y + clip->height, bounds.y + bounds.height);
  if (right <= left || bottom <= top) {
    empty = true;
    return std::nullopt;
  }
  return UiLogicalRect{
      .x = left, .y = top, .width = right - left, .height = bottom - top};
}

std::uint8_t colorByte(float value) noexcept {
  return static_cast<std::uint8_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F);
}

std::uint32_t packAbgr(const std::array<float, 4> &rgba) noexcept {
  const std::uint32_t red = colorByte(rgba[0]);
  const std::uint32_t green = colorByte(rgba[1]);
  const std::uint32_t blue = colorByte(rgba[2]);
  const std::uint32_t alpha = colorByte(rgba[3]);
  return (alpha << 24U) | (blue << 16U) | (green << 8U) | red;
}

std::array<float, 4> ghostColor(const ReplayGhostEvent &event) noexcept {
  if (event.judgement == PGreat) {
    return {1.0F, 1.0F, 1.0F, 220.0F / 255.0F};
  }
  return event.judgeTimeMicros < event.noteTimeMicros
             ? std::array<float, 4>{0.0F, 96.0F / 255.0F, 1.0F,
                                    220.0F / 255.0F}
             : std::array<float, 4>{1.0F, 40.0F / 255.0F, 40.0F / 255.0F,
                                    220.0F / 255.0F};
}

bool validRect(const AuthoredRect &rect) noexcept {
  return std::isfinite(rect.x) && std::isfinite(rect.y) &&
         std::isfinite(rect.width) && std::isfinite(rect.height) &&
         rect.width > 0.0 && rect.height > 0.0;
}

AuthoredPoint transformPoint(const Affine2D &transform, double x,
                             double y) noexcept {
  return {.x = transform.m00 * x + transform.m01 * y + transform.tx,
          .y = transform.m10 * x + transform.m11 * y + transform.ty};
}

std::optional<UiLogicalRect>
projectAuthoredRect(const PlaySkinViewport &viewport,
                    const AuthoredRect &rect) noexcept {
  if (!validRect(rect) || !viewport.valid) {
    return std::nullopt;
  }
  const std::array corners{
      transformPoint(viewport.authoredToUi, rect.x, rect.y),
      transformPoint(viewport.authoredToUi, rect.x + rect.width, rect.y),
      transformPoint(viewport.authoredToUi, rect.x + rect.width,
                     rect.y + rect.height),
      transformPoint(viewport.authoredToUi, rect.x, rect.y + rect.height),
  };
  const auto [minimumX, maximumX] = std::minmax_element(
      corners.begin(), corners.end(),
      [](const AuthoredPoint &left, const AuthoredPoint &right) {
        return left.x < right.x;
      });
  const auto [minimumY, maximumY] = std::minmax_element(
      corners.begin(), corners.end(),
      [](const AuthoredPoint &left, const AuthoredPoint &right) {
        return left.y < right.y;
      });
  const UiLogicalRect projected{.x = minimumX->x,
                                .y = minimumY->y,
                                .width = maximumX->x - minimumX->x,
                                .height = maximumY->y - minimumY->y};
  return std::isfinite(projected.x) && std::isfinite(projected.y) &&
                 std::isfinite(projected.width) &&
                 std::isfinite(projected.height) && projected.width > 0.0 &&
                 projected.height > 0.0
             ? std::optional<UiLogicalRect>(projected)
             : std::nullopt;
}

bool intersects(const AuthoredRect &left, const AuthoredRect &right) noexcept {
  return left.x < right.x + right.width && right.x < left.x + left.width &&
         left.y < right.y + right.height &&
         right.y < left.y + left.height;
}

void appendGhostStrip(SkinCommandBuffer &buffer, const PlaySkinViewport &viewport,
                      const AuthoredRect &rect, const AuthoredRect &clip,
                      const std::array<float, 4> &rgba,
                      std::uint32_t ordinal) {
  AuthoredDestinationGeometry geometry;
  geometry.rect = rect;
  geometry.clip = clip;
  geometry.rgba = rgba;
  geometry.blend = SkinBlendMode::Normal;
  geometry.filter = SkinFilterMode::Nearest;
  geometry.stretch = SkinStretchMode::Stretch;
  const auto projected = projectSkinDestinationToUi(
      geometry,
      {.textureWidth = 1, .textureHeight = 1, .region = {.x = 0, .y = 0, .w = 1, .h = 1}},
      viewport);
  bool emptyClip = false;
  const auto scissor =
      intersectClip(projected.clip, projectedSkinScissorBounds(viewport), emptyClip);
  if (emptyClip) {
    return;
  }

  SkinPrimitiveCommand primitive;
  primitive.kind = SkinPrimitiveKind::SolidQuad;
  primitive.state = {.blend = SkinBlendMode::Normal,
                     .filter = SkinFilterMode::Nearest,
                     .scissor = scissor};
  primitive.vertices.reserve(4);
  const std::uint32_t color = packAbgr(rgba);
  for (const auto &vertex : projected.vertices) {
    primitive.vertices.push_back({.x = static_cast<float>(vertex[0]),
                                  .y = static_cast<float>(vertex[1]),
                                  .rgba = color});
  }
  buffer.commands.push_back({.authoredOrdinal = ordinal,
                             .sourceObject = 0,
                             .payload = std::move(primitive)});
}

void appendStartLaneIndicator(
    SkinCommandBuffer &buffer, const PlaySkinViewport &viewport,
    const SyntheticStartLaneIndicatorLaneGeometry &lane,
    double visibleLaneHeightRatio,
    std::uint32_t ordinal) {
  if (!validRect(lane.laneRegion)) {
    return;
  }
  const float coverEdgeY = static_cast<float>(
      lane.laneRegion.y + lane.laneRegion.height * visibleLaneHeightRatio);
  const auto triangle = start_lane_indicator::placeTriangle(
      static_cast<float>(lane.laneRegion.x),
      static_cast<float>(lane.laneRegion.width),
      static_cast<float>(lane.laneRegion.y), coverEdgeY);
  const AuthoredDestinationGeometry geometry{
      .rect = lane.laneRegion,
      .clip = lane.laneRegion,
      .rgba = lane.rgba,
      .blend = SkinBlendMode::Normal,
      .filter = SkinFilterMode::Nearest,
      .stretch = SkinStretchMode::Stretch};
  const auto projected = projectSkinDestinationToUi(
      geometry,
      {.textureWidth = 1, .textureHeight = 1, .region = {.x = 0, .y = 0, .w = 1, .h = 1}},
      viewport);
  bool emptyClip = false;
  const auto scissor =
      intersectClip(projected.clip, projectedSkinScissorBounds(viewport), emptyClip);
  if (emptyClip) {
    return;
  }
  const std::uint32_t color = packAbgr(lane.rgba);
  const auto projectPoint = [&viewport, color](float x, float y) {
    return SkinVertex{
        .x = static_cast<float>(viewport.authoredToUi.m00 * x +
                                viewport.authoredToUi.m01 * y +
                                viewport.authoredToUi.tx),
        .y = static_cast<float>(viewport.authoredToUi.m10 * x +
                                viewport.authoredToUi.m11 * y +
                                viewport.authoredToUi.ty),
        .rgba = color};
  };
  SkinPrimitiveCommand primitive;
  primitive.kind = SkinPrimitiveKind::TriangleStrip;
  primitive.state = {.blend = SkinBlendMode::Normal,
                     .filter = SkinFilterMode::Nearest,
                     .scissor = scissor};
  primitive.vertices.reserve(3);
  primitive.vertices.push_back(projectPoint(triangle.leftX, triangle.baseY));
  primitive.vertices.push_back(projectPoint(triangle.tipX, triangle.tipY));
  primitive.vertices.push_back(projectPoint(triangle.rightX, triangle.baseY));
  buffer.commands.push_back({.authoredOrdinal = ordinal,
                             .sourceObject = 0,
                             .payload = std::move(primitive)});
}

} // namespace

std::optional<SelectedSkinHudGeometry>
selectedSkinHudGeometry(const SyntheticReplayGhostGeometry &geometry) {
  if (geometry.frameSerial == 0 || !geometry.viewport.valid ||
      geometry.lanes.empty() || !validRect(geometry.lanes.front().clip)) {
    return std::nullopt;
  }

  double left = std::numeric_limits<double>::infinity();
  double right = -std::numeric_limits<double>::infinity();
  std::size_t laneCount = 0;
  for (const auto &lane : geometry.lanes) {
    if (!validRect(lane.normalNote)) {
      continue;
    }
    left = std::min(left, lane.normalNote.x);
    right = std::max(right, lane.normalNote.x + lane.normalNote.width);
    ++laneCount;
  }
  if (laneCount == 0 || !std::isfinite(left) || !std::isfinite(right) ||
      right <= left) {
    return std::nullopt;
  }

  // The same primary lane clip defines the shared vertical play area for the
  // replay overlay and the selected-skin HUD. Every lane still contributes to
  // the horizontal span because note widths and positions may differ.
  const auto &primary = geometry.lanes.front();
  const AuthoredRect authoredPlayArea{.x = left,
                                      .y = primary.clip.y,
                                      .width = right - left,
                                      .height = primary.clip.height};
  const auto playArea =
      projectAuthoredRect(geometry.viewport, authoredPlayArea);
  if (!playArea) {
    return std::nullopt;
  }
  const AuthoredPoint judgement =
      transformPoint(geometry.viewport.authoredToUi, primary.normalNote.x,
                     primary.normalNote.y);
  if (!std::isfinite(judgement.y)) {
    return std::nullopt;
  }
  return SelectedSkinHudGeometry{.frameSerial = geometry.frameSerial,
                                 .playArea = *playArea,
                                 .judgementLineY = judgement.y,
                                 .laneCount = laneCount};
}

SkinCommandBuffer buildSyntheticReplayGhostOverlay(
    const SyntheticReplayGhostGeometry &geometry,
    const SyntheticReplayGhostFrameInput &input) {
  SkinCommandBuffer result{.frameSerial = input.frameSerial};
  if (!input.enabled || input.frameSerial == 0 ||
      input.frameSerial != geometry.frameSerial || !geometry.viewport.valid ||
      !std::isfinite(input.currentScrollPosition) ||
      !std::isfinite(input.hispeed) || input.hispeed <= 0.0 ||
      !std::isfinite(geometry.sharedLaneHeight) ||
      geometry.sharedLaneHeight <= 0.0 || geometry.lanes.empty() ||
      !validRect(geometry.lanes.front().clip)) {
    return result;
  }

  // Pinned LaneRenderer derives the vertical play area from lanes[0] for all
  // lanes. Keep each lane's own horizontal region, but never submit a ghost
  // beyond that shared top/bottom range.
  const auto &sharedPlayArea = geometry.lanes.front().clip;
  const double visibleLaneHeightRatio =
      std::isfinite(input.visibleLaneHeightRatio)
          ? std::clamp(input.visibleLaneHeightRatio, 0.0, 1.0)
          : 1.0;
  const double sharedPlayAreaBottom =
      sharedPlayArea.y + sharedPlayArea.height;
  // BMSRenderer's noteVisibleUpperBound is measured from the judgement line.
  // Map that same retained fraction onto the selected skin's primary lane.
  // With no cover the full authored/shared play area remains authoritative.
  const double laneCoverBottom =
      visibleLaneHeightRatio < 1.0
          ? std::min(sharedPlayAreaBottom,
                     geometry.sharedLaneOriginY +
                         geometry.sharedLaneHeight * visibleLaneHeightRatio)
          : sharedPlayAreaBottom;
  if (!std::isfinite(laneCoverBottom) ||
      laneCoverBottom <= sharedPlayArea.y) {
    return result;
  }

  const double scrollScale = geometry.sharedLaneHeight * input.hispeed;
  if (!std::isfinite(scrollScale) || scrollScale <= 0.0) {
    return result;
  }
  double firstVisibleScrollPosition = std::numeric_limits<double>::infinity();
  double lastVisibleScrollPosition =
      -std::numeric_limits<double>::infinity();
  for (const auto &lane : geometry.lanes) {
    if (!validRect(lane.normalNote) || !validRect(lane.clip)) {
      continue;
    }
    const double clipTop = std::max(lane.clip.y, sharedPlayArea.y);
    const double clipBottom =
        std::min(lane.clip.y + lane.clip.height, laneCoverBottom);
    if (!std::isfinite(clipTop) || !std::isfinite(clipBottom) ||
        clipBottom <= clipTop) {
      continue;
    }
    firstVisibleScrollPosition = std::min(
        firstVisibleScrollPosition,
        input.currentScrollPosition +
            (clipTop - lane.normalNote.y - lane.normalNote.height) /
                scrollScale);
    lastVisibleScrollPosition = std::max(
        lastVisibleScrollPosition,
        input.currentScrollPosition +
            (clipBottom - lane.normalNote.y) / scrollScale);
  }
  if (!std::isfinite(firstVisibleScrollPosition) ||
      !std::isfinite(lastVisibleScrollPosition) ||
      lastVisibleScrollPosition < firstVisibleScrollPosition) {
    return result;
  }

  const auto visibleEvents = replay_ghost::visibleEventsInScrollRange(
      input.events, firstVisibleScrollPosition, lastVisibleScrollPosition);
  for (const ReplayGhostEvent &event : visibleEvents) {
    if (event.judgeTimeMicros < input.visualTimeMicros ||
        !std::isfinite(event.judgeScrollPosition)) {
      continue;
    }
    const auto lane = std::ranges::find(geometry.lanes, event.lane,
                                        &SyntheticReplayGhostLaneGeometry::lane);
    if (lane == geometry.lanes.end() || !validRect(lane->normalNote) ||
        !validRect(lane->clip)) {
      continue;
    }
    const double clipTop = std::max(lane->clip.y, sharedPlayArea.y);
    const double clipBottom =
        std::min(lane->clip.y + lane->clip.height,
                 laneCoverBottom);
    if (!std::isfinite(clipTop) || !std::isfinite(clipBottom) ||
        clipBottom <= clipTop) {
      continue;
    }
    const AuthoredRect playAreaClip{.x = lane->clip.x,
                                    .y = clipTop,
                                    .width = lane->clip.width,
                                    .height = clipBottom - clipTop};
    const double y = lane->normalNote.y +
                     (event.judgeScrollPosition - input.currentScrollPosition) *
                         geometry.sharedLaneHeight * input.hispeed;
    if (!std::isfinite(y)) {
      continue;
    }
    const double thickness =
        std::max(0.015, lane->normalNote.height * 0.12);
    if (!std::isfinite(thickness) || thickness <= 0.0 ||
        thickness * 2.0 > lane->normalNote.height) {
      continue;
    }
    const AuthoredRect outline{
        .x = lane->normalNote.x,
        .y = y,
        .width = lane->normalNote.width,
        .height = lane->normalNote.height,
    };
    if (!intersects(outline, playAreaClip)) {
      continue;
    }
    const auto rgba = ghostColor(event);
    const std::uint32_t ordinal =
        static_cast<std::uint32_t>(result.commands.size());
    appendGhostStrip(result, geometry.viewport,
                     {.x = outline.x,
                      .y = outline.y,
                      .width = outline.width,
                      .height = thickness},
                     playAreaClip, rgba, ordinal);
    appendGhostStrip(result, geometry.viewport,
                     {.x = outline.x,
                      .y = outline.y + outline.height - thickness,
                      .width = outline.width,
                      .height = thickness},
                     playAreaClip, rgba, ordinal + 1U);
    appendGhostStrip(result, geometry.viewport,
                     {.x = outline.x,
                      .y = outline.y,
                      .width = thickness,
                      .height = outline.height},
                     playAreaClip, rgba, ordinal + 2U);
    appendGhostStrip(result, geometry.viewport,
                     {.x = outline.x + outline.width - thickness,
                      .y = outline.y,
                      .width = thickness,
                      .height = outline.height},
                     playAreaClip, rgba, ordinal + 3U);
  }
  return result;
}

SkinCommandBuffer buildSyntheticStartLaneIndicatorOverlay(
    const PlaySkinViewport &viewport,
    std::span<const SyntheticStartLaneIndicatorLaneGeometry> geometry,
    const SyntheticStartLaneIndicatorFrameInput &input) {
  SkinCommandBuffer result{.frameSerial = input.frameSerial};
  if (input.frameSerial == 0 || !viewport.valid || input.lanes.empty() ||
      geometry.empty()) {
    return result;
  }
  const double visibleLaneHeightRatio =
      std::isfinite(input.visibleLaneHeightRatio)
          ? std::clamp(input.visibleLaneHeightRatio, 0.0, 1.0)
          : 1.0;
  for (const int requestedLane : input.lanes) {
    const auto lane = std::ranges::find(geometry, requestedLane,
                                        &SyntheticStartLaneIndicatorLaneGeometry::lane);
    if (lane == geometry.end()) {
      continue;
    }
    appendStartLaneIndicator(result, viewport, *lane, visibleLaneHeightRatio,
                             static_cast<std::uint32_t>(result.commands.size()));
  }
  return result;
}

} // namespace skin
