#pragma once

#include "BeatorajaSkinModel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace skin {

// This owns only the resolved, source-neutral note texture arrays. A decoder
// can build it from Lua tables without giving this helper access to Lua,
// files, textures, or a mutable SkinNoteObject.
using SkinAuthoredNoteVisualSlot = std::optional<SkinSpriteFrames>;
using SkinAuthoredNoteVisualSlots = std::vector<SkinAuthoredNoteVisualSlot>;

struct SkinNoteNormalizationInput {
  SkinAuthoredNoteVisualSlots note;
  SkinAuthoredNoteVisualSlots mine;
  SkinAuthoredNoteVisualSlots lnEnd;
  SkinAuthoredNoteVisualSlots lnStart;
  SkinAuthoredNoteVisualSlots lnBody;
  SkinAuthoredNoteVisualSlots lnActive;
  // JsonPlaySkinObjectLoader selects the modern LN layout only when this
  // field is present and non-empty.
  std::optional<SkinAuthoredNoteVisualSlots> lnBodyActive;
  SkinAuthoredNoteVisualSlots hcnEnd;
  SkinAuthoredNoteVisualSlots hcnStart;
  SkinAuthoredNoteVisualSlots hcnBody;
  SkinAuthoredNoteVisualSlots hcnActive;
  SkinAuthoredNoteVisualSlots hcnDamage;
  SkinAuthoredNoteVisualSlots hcnReactive;
  // JsonPlaySkinObjectLoader selects the modern HCN layout only when this
  // field is present and non-empty.
  std::optional<SkinAuthoredNoteVisualSlots> hcnBodyActive;
  SkinAuthoredNoteVisualSlots hcnBodyReactive;
  SkinAuthoredNoteVisualSlots hcnBodyMiss;
};

enum class SkinNoteFallbackColor : std::uint8_t {
  White,
  Yellow,
  Red,
  Orange,
  Cyan,
};

enum class SkinNoteFallbackShape : std::uint8_t { Solid, DoubleOutline };

struct SkinSynthesizedNoteFallback {
  SkinNoteFallbackColor color = SkinNoteFallbackColor::Yellow;
  SkinNoteFallbackShape shape = SkinNoteFallbackShape::Solid;
  std::optional<int> authoredNoteSlot;
};

using SkinNormalizedNoteVisual =
    std::variant<SkinSpriteFrames, SkinSynthesizedNoteFallback>;

struct SkinNormalizedNoteLane {
  std::size_t authoredLane = 0;
  std::array<SkinNormalizedNoteVisual, 14> visuals;
};

struct SkinNormalizedNote {
  std::vector<SkinNormalizedNoteLane> lanes;
  SkinHcnBodySlotLayout hcnBodySlotLayout = SkinHcnBodySlotLayout::Legacy;
};

struct SkinNoteNormalizationPolicy {
  static constexpr std::size_t visualKindCount = 14;
  static constexpr std::size_t maxLanes = 256;
  static constexpr std::size_t maxFramesPerVisual = 4'096;
  static constexpr std::size_t maxNormalizedFrames = 200'000;
};

enum class SkinNoteNormalizationError : std::uint8_t {
  None,
  LaneLimitExceeded,
  UnsafeCardinality,
  InvalidAuthoredVisual,
  FrameLimitExceeded,
};

struct SkinNoteNormalizationResult {
  std::optional<SkinNormalizedNote> note;
  SkinNoteNormalizationError error = SkinNoteNormalizationError::None;
};

// Mirrors JsonPlaySkinObjectLoader's long-note array ordering at Beatoraja
// c2ed5db1 while rejecting array shapes that the Java implementation would
// index out of bounds. Null entries receive SkinNote's pinned fallback colors.
[[nodiscard]] SkinNoteNormalizationResult
normalizeSkinNote(const SkinNoteNormalizationInput &input);

} // namespace skin
