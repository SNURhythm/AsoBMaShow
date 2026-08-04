#include "SkinNoteLineNormalization.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
#include <cmath>
#include <utility>

namespace skin {
namespace {

SkinNoteLineNormalizationResult
failure(SkinNoteLineNormalizationError error) {
  return {.lines = std::nullopt, .error = error};
}

bool finiteRect(const SkinAuthoredRect &rect) noexcept {
  return std::isfinite(rect.x) && std::isfinite(rect.y) &&
         std::isfinite(rect.width) && std::isfinite(rect.height);
}

bool finiteDestination(const SkinDestinationBody &destination) noexcept {
  if (destination.mouseRect && !finiteRect(*destination.mouseRect)) {
    return false;
  }
  for (const auto &frame : destination.frames) {
    if (!std::isfinite(frame.x) || !std::isfinite(frame.y) ||
        !std::isfinite(frame.width) || !std::isfinite(frame.height) ||
        !std::isfinite(frame.angleDegrees)) {
      return false;
    }
  }
  return true;
}

bool validSprite(const SkinSpriteFrames &sprite) noexcept {
  if (sprite.resource == 0 || sprite.frames.empty() ||
      sprite.frames.size() > SkinNoteLineNormalizationPolicy::maxFramesPerSprite ||
      (sprite.timer && !*sprite.timer)) {
    return false;
  }
  for (const auto &frame : sprite.frames) {
    if (frame.gridColumns <= 0 || frame.gridRows <= 0 || frame.gridColumn < 0 ||
        frame.gridRow < 0 || frame.gridColumn >= frame.gridColumns ||
        frame.gridRow >= frame.gridRows || frame.w < -1 || frame.h < -1) {
      return false;
    }
  }
  return true;
}

SkinNoteLineNormalizationError
validateSlot(const SkinAuthoredNoteLineSlot &slot, std::size_t &frameCount) {
  if (slot.image) {
    if (slot.image->frames.size() >
        SkinNoteLineNormalizationPolicy::maxFramesPerSprite) {
      return SkinNoteLineNormalizationError::FrameLimitExceeded;
    }
    if (!validSprite(*slot.image)) {
      return SkinNoteLineNormalizationError::InvalidSprite;
    }
    if (slot.image->frames.size() >
        SkinNoteLineNormalizationPolicy::maxMaterializedFrames - frameCount) {
      return SkinNoteLineNormalizationError::FrameLimitExceeded;
    }
    frameCount += slot.image->frames.size();
  }
  if (slot.destination) {
    if (slot.destination->frames.size() >
            SkinNoteLineNormalizationPolicy::maxFramesPerDestination ||
        slot.destination->conditions.size() >
            SkinNoteLineNormalizationPolicy::maxDestinationConditions ||
        slot.destination->offsetIds.size() >
            SkinNoteLineNormalizationPolicy::maxDestinationOffsets) {
      return SkinNoteLineNormalizationError::UnsafeCardinality;
    }
    if (!finiteDestination(*slot.destination)) {
      return SkinNoteLineNormalizationError::NonFiniteGeometry;
    }
    if (slot.destination->frames.size() >
        SkinNoteLineNormalizationPolicy::maxMaterializedFrames - frameCount) {
      return SkinNoteLineNormalizationError::FrameLimitExceeded;
    }
    frameCount += slot.destination->frames.size();
  }
  return SkinNoteLineNormalizationError::None;
}

SkinNormalizedNoteLine normalizeSlot(const SkinAuthoredNoteLineSlot *slot,
                                     SkinNoteLineKind kind,
                                     std::size_t laneGroup) {
  SkinNormalizedNoteLine normalized;
  normalized.laneGroup = laneGroup;
  normalized.kind = kind;
  if (slot != nullptr) {
    normalized.image = slot->image;
    normalized.destination = slot->destination;
  }
  return normalized;
}

SkinNoteLineNormalizationError validatePrefix(
    const SkinAuthoredNoteLineSlots &slots, std::size_t count,
    std::size_t &frameCount) {
  for (std::size_t index = 0; index < count; ++index) {
    if (!slots[index]) {
      continue;
    }
    const auto error = validateSlot(*slots[index], frameCount);
    if (error != SkinNoteLineNormalizationError::None) {
      return error;
    }
  }
  return SkinNoteLineNormalizationError::None;
}

void appendPrefix(SkinNormalizedNoteLines &normalized,
                  const SkinAuthoredNoteLineSlots &slots,
                  SkinNoteLineKind kind, std::size_t count) {
  for (std::size_t index = 0; index < count; ++index) {
    const auto *slot = index < slots.size() && slots[index]
                           ? &*slots[index]
                           : nullptr;
    normalized.lines.push_back(normalizeSlot(slot, kind, index));
  }
}

} // namespace

SkinNoteLineNormalizationResult
normalizeSkinNoteLines(const SkinNoteLineNormalizationInput &input) {
  const std::size_t groupCount = input.group.size();
  if (groupCount > SkinNoteLineNormalizationPolicy::maxGroups) {
    return failure(SkinNoteLineNormalizationError::GroupLimitExceeded);
  }
  if (input.bpm.size() > SkinNoteLineNormalizationPolicy::maxAuxiliarySlots ||
      input.stop.size() > SkinNoteLineNormalizationPolicy::maxAuxiliarySlots ||
      input.time.size() > SkinNoteLineNormalizationPolicy::maxAuxiliarySlots) {
    return failure(SkinNoteLineNormalizationError::AuxiliaryLimitExceeded);
  }

  const std::size_t bpmCount = std::min(groupCount, input.bpm.size());
  const std::size_t stopCount = std::min(groupCount, input.stop.size());
  const std::size_t timeCount = std::min(groupCount, input.time.size());
  const std::size_t outputCount = groupCount * 4;
  if (outputCount > SkinNoteLineNormalizationPolicy::maxOutputLines) {
    return failure(SkinNoteLineNormalizationError::OutputLimitExceeded);
  }

  std::size_t frameCount = 0;
  const auto groupError = validatePrefix(input.group, groupCount, frameCount);
  if (groupError != SkinNoteLineNormalizationError::None) {
    return failure(groupError);
  }
  const auto bpmError = validatePrefix(input.bpm, bpmCount, frameCount);
  if (bpmError != SkinNoteLineNormalizationError::None) {
    return failure(bpmError);
  }
  const auto stopError = validatePrefix(input.stop, stopCount, frameCount);
  if (stopError != SkinNoteLineNormalizationError::None) {
    return failure(stopError);
  }
  const auto timeError = validatePrefix(input.time, timeCount, frameCount);
  if (timeError != SkinNoteLineNormalizationError::None) {
    return failure(timeError);
  }

  SkinNormalizedNoteLines normalized;
  normalized.groups.reserve(groupCount);
  normalized.lines.reserve(outputCount);
  for (std::size_t index = 0; index < groupCount; ++index) {
    const auto *slot = input.group[index] ? &*input.group[index] : nullptr;
    if (slot == nullptr || !slot->destination ||
        slot->destination->frames.empty()) {
      return failure(SkinNoteLineNormalizationError::MissingGroupLaneRect);
    }
    const auto &frame = slot->destination->frames.front();
    normalized.groups.push_back(
        {.laneGroup = index,
         .laneRect = {.x = frame.x,
                      .y = frame.y,
                      .width = frame.width,
                      .height = frame.height}});
    normalized.lines.push_back(normalizeSlot(slot, SkinNoteLineKind::Group, index));
  }
  appendPrefix(normalized, input.bpm, SkinNoteLineKind::Bpm, groupCount);
  appendPrefix(normalized, input.stop, SkinNoteLineKind::Stop, groupCount);
  appendPrefix(normalized, input.time, SkinNoteLineKind::Time, groupCount);

  return {.lines = std::move(normalized),
          .error = SkinNoteLineNormalizationError::None};
}

} // namespace skin

#endif
