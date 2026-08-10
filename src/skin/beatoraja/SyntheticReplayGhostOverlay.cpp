#include "SyntheticReplayGhostOverlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
      intersectClip(projected.clip, viewport.safeUiBounds, emptyClip);
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

} // namespace

SkinCommandBuffer buildSyntheticReplayGhostOverlay(
    const SyntheticReplayGhostGeometry &geometry,
    const SyntheticReplayGhostFrameInput &input) {
  SkinCommandBuffer result{.frameSerial = input.frameSerial};
  if (!input.enabled || input.frameSerial == 0 ||
      input.frameSerial != geometry.frameSerial || !geometry.viewport.valid ||
      !std::isfinite(input.currentScrollPosition) ||
      !std::isfinite(input.hispeed) || input.hispeed <= 0.0 ||
      !std::isfinite(geometry.sharedLaneHeight) ||
      geometry.sharedLaneHeight <= 0.0) {
    return result;
  }

  for (const ReplayGhostEvent &event : input.events) {
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
    if (!intersects(outline, lane->clip)) {
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
                     lane->clip, rgba, ordinal);
    appendGhostStrip(result, geometry.viewport,
                     {.x = outline.x,
                      .y = outline.y + outline.height - thickness,
                      .width = outline.width,
                      .height = thickness},
                     lane->clip, rgba, ordinal + 1U);
    appendGhostStrip(result, geometry.viewport,
                     {.x = outline.x,
                      .y = outline.y,
                      .width = thickness,
                      .height = outline.height},
                     lane->clip, rgba, ordinal + 2U);
    appendGhostStrip(result, geometry.viewport,
                     {.x = outline.x + outline.width - thickness,
                      .y = outline.y,
                      .width = thickness,
                      .height = outline.height},
                     lane->clip, rgba, ordinal + 3U);
  }
  return result;
}

} // namespace skin
