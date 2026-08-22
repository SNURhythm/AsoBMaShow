#include "SkinNoteNormalization.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <initializer_list>
#include <utility>

namespace skin {
namespace {

using Slots = SkinAuthoredNoteVisualSlots;

constexpr std::size_t visualIndex(SkinNoteVisualKind kind) {
  return static_cast<std::size_t>(kind);
}

static_assert(visualIndex(SkinNoteVisualKind::HcnReactive) + 1 ==
              SkinNoteNormalizationPolicy::visualKindCount);

SkinNoteNormalizationResult failure(SkinNoteNormalizationError error) {
  return {.note = std::nullopt, .error = error};
}

SkinNormalizedNoteVisual fallbackFor(SkinNoteVisualKind kind) {
  switch (kind) {
  case SkinNoteVisualKind::Normal:
    return SkinSynthesizedNoteFallback{.color = SkinNoteFallbackColor::White,
                                       .shape = SkinNoteFallbackShape::Solid};
  case SkinNoteVisualKind::Mine:
    return SkinSynthesizedNoteFallback{.color = SkinNoteFallbackColor::Red,
                                       .shape = SkinNoteFallbackShape::Solid};
  case SkinNoteVisualKind::Hidden:
    return SkinSynthesizedNoteFallback{
        .color = SkinNoteFallbackColor::Orange,
        .shape = SkinNoteFallbackShape::DoubleOutline};
  case SkinNoteVisualKind::Processed:
    return SkinSynthesizedNoteFallback{
        .color = SkinNoteFallbackColor::Cyan,
        .shape = SkinNoteFallbackShape::DoubleOutline};
  default:
    return SkinSynthesizedNoteFallback{.color = SkinNoteFallbackColor::Yellow,
                                       .shape = SkinNoteFallbackShape::Solid};
  }
}

bool validSprite(const SkinSpriteFrames &sprite) {
  return sprite.resource != 0 && !sprite.frames.empty() &&
         sprite.frames.size() <=
             SkinNoteNormalizationPolicy::maxFramesPerVisual;
}

SkinNoteNormalizationError
validateSelectedArrays(std::size_t laneCount,
                       std::initializer_list<const Slots *> selectedArrays) {
  std::size_t totalFrames = 0;
  for (const Slots *const slots : selectedArrays) {
    if (slots->size() != laneCount) {
      return SkinNoteNormalizationError::UnsafeCardinality;
    }
    for (const auto &slot : *slots) {
      if (!slot) {
        continue;
      }
      if (!validSprite(*slot)) {
        return slot->frames.size() >
                       SkinNoteNormalizationPolicy::maxFramesPerVisual
                   ? SkinNoteNormalizationError::FrameLimitExceeded
                   : SkinNoteNormalizationError::InvalidAuthoredVisual;
      }
      if (slot->frames.size() >
          SkinNoteNormalizationPolicy::maxNormalizedFrames - totalFrames) {
        return SkinNoteNormalizationError::FrameLimitExceeded;
      }
      totalFrames += slot->frames.size();
    }
  }
  return SkinNoteNormalizationError::None;
}

void setVisual(SkinNormalizedNoteLane &lane, SkinNoteVisualKind kind,
               const SkinAuthoredNoteVisualSlot &authored,
               std::optional<int> authoredSlot = std::nullopt) {
  auto visual = authored ? SkinNormalizedNoteVisual(*authored)
                         : fallbackFor(kind);
  std::visit(
      [authoredSlot](auto &value) { value.authoredNoteSlot = authoredSlot; },
      visual);
  lane.visuals[visualIndex(kind)] = std::move(visual);
}

} // namespace

SkinNoteNormalizationResult
normalizeSkinNote(const SkinNoteNormalizationInput &input) {
  const std::size_t laneCount = input.note.size();
  if (laneCount > SkinNoteNormalizationPolicy::maxLanes) {
    return failure(SkinNoteNormalizationError::LaneLimitExceeded);
  }

  // JsonPlaySkinObjectLoader uses presence-and-non-empty, not just presence,
  // for each compatibility switch. The two switches are intentionally
  // independent: a skin can use one modern family and one legacy family.
  const bool modernLn = input.lnBodyActive && !input.lnBodyActive->empty();
  const bool modernHcn = input.hcnBodyActive && !input.hcnBodyActive->empty();
  const Slots &lnActive = modernLn ? *input.lnBodyActive : input.lnBody;
  const Slots &lnInactive = modernLn ? input.lnBody : input.lnActive;
  const Slots &hcnActive = modernHcn ? *input.hcnBodyActive : input.hcnBody;
  const Slots &hcnInactive = modernHcn ? input.hcnBody : input.hcnActive;
  // JsonPlaySkinObjectLoader's raw indexes 8 and 9 are hcnbodyReactive and
  // hcnbodyMiss. The source-neutral kinds preserve their HCN semantics,
  // rather than retaining those raw positions.
  const Slots &hcnDamage = modernHcn ? input.hcnBodyMiss : input.hcnDamage;
  const Slots &hcnReactive =
      modernHcn ? input.hcnBodyReactive : input.hcnReactive;

  const auto validation = validateSelectedArrays(
      laneCount, {&input.note, &input.mine, &input.lnEnd, &input.lnStart,
                  &lnActive, &lnInactive, &input.hcnEnd, &input.hcnStart,
                  &hcnActive, &hcnInactive, &hcnDamage, &hcnReactive});
  if (validation != SkinNoteNormalizationError::None) {
    return failure(validation);
  }

  SkinNormalizedNote normalized;
  normalized.hcnBodySlotLayout =
      modernHcn ? SkinHcnBodySlotLayout::Modern : SkinHcnBodySlotLayout::Legacy;
  normalized.lanes.reserve(laneCount);
  for (std::size_t laneIndex = 0; laneIndex < laneCount; ++laneIndex) {
    SkinNormalizedNoteLane lane{.authoredLane = laneIndex};
    setVisual(lane, SkinNoteVisualKind::Normal, input.note[laneIndex]);
    setVisual(lane, SkinNoteVisualKind::Mine, input.mine[laneIndex]);
    lane.visuals[visualIndex(SkinNoteVisualKind::Hidden)] =
        fallbackFor(SkinNoteVisualKind::Hidden);
    lane.visuals[visualIndex(SkinNoteVisualKind::Processed)] =
        fallbackFor(SkinNoteVisualKind::Processed);
    setVisual(lane, SkinNoteVisualKind::LnEnd, input.lnEnd[laneIndex], 0);
    setVisual(lane, SkinNoteVisualKind::LnStart, input.lnStart[laneIndex], 1);
    setVisual(lane, SkinNoteVisualKind::LnBodyActive, lnActive[laneIndex], 2);
    setVisual(lane, SkinNoteVisualKind::LnBodyInactive, lnInactive[laneIndex], 3);
    setVisual(lane, SkinNoteVisualKind::HcnEnd, input.hcnEnd[laneIndex], 4);
    setVisual(lane, SkinNoteVisualKind::HcnStart, input.hcnStart[laneIndex], 5);
    setVisual(lane, SkinNoteVisualKind::HcnBodyActive, hcnActive[laneIndex], 6);
    setVisual(lane, SkinNoteVisualKind::HcnBodyInactive,
              hcnInactive[laneIndex], 7);
    setVisual(lane, SkinNoteVisualKind::HcnDamage, hcnDamage[laneIndex],
              modernHcn ? 9 : 8);
    setVisual(lane, SkinNoteVisualKind::HcnReactive, hcnReactive[laneIndex],
              modernHcn ? 8 : 9);
    normalized.lanes.push_back(std::move(lane));
  }

  return {.note = std::move(normalized),
          .error = SkinNoteNormalizationError::None};
}

} // namespace skin

#endif
