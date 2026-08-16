#pragma once

#include "BeatorajaSkinModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace skin {

// A source-neutral representation of one JsonSkin.Note line array element
// after a loader has separately attempted its Image-only lookup and decoded
// its Destination. Either nested value can be absent; auxiliary arrays retain
// that sparse shape at their authored group index.
struct SkinAuthoredNoteLineSlot {
  std::optional<SkinSpriteFrames> image;
  std::optional<SkinDestinationBody> destination;
};

using SkinAuthoredNoteLineSlots =
    std::vector<std::optional<SkinAuthoredNoteLineSlot>>;

struct SkinNoteLineNormalizationInput {
  SkinAuthoredNoteLineSlots group;
  SkinAuthoredNoteLineSlots bpm;
  SkinAuthoredNoteLineSlots stop;
  SkinAuthoredNoteLineSlots time;
};

// A source-neutral intermediate that preserves sparse lines before they are
// copied into SkinNoteLinePresentation's optional image/destination fields.
struct SkinNormalizedNoteLine {
  std::size_t laneGroup = 0;
  SkinNoteLineKind kind = SkinNoteLineKind::Group;
  std::optional<SkinSpriteFrames> image;
  std::optional<SkinDestinationBody> destination;
};

struct SkinNormalizedNoteLaneGroup {
  std::size_t laneGroup = 0;
  SkinAuthoredRect laneRect;
};

struct SkinNormalizedNoteLines {
  std::vector<SkinNormalizedNoteLaneGroup> groups;
  std::vector<SkinNormalizedNoteLine> lines;
};

struct SkinNoteLineNormalizationPolicy {
  static constexpr std::size_t maxGroups = 256;
  static constexpr std::size_t maxAuxiliarySlots = 256;
  static constexpr std::size_t maxFramesPerSprite = 4'096;
  static constexpr std::size_t maxFramesPerDestination = 4'096;
  static constexpr std::size_t maxDestinationConditions = 256;
  static constexpr std::size_t maxDestinationOffsets = 256;
  static constexpr std::size_t maxMaterializedFrames = 200'000;
  static constexpr std::size_t maxOutputLines = maxGroups * 4;
};

enum class SkinNoteLineNormalizationError : std::uint8_t {
  None,
  GroupLimitExceeded,
  AuxiliaryLimitExceeded,
  OutputLimitExceeded,
  MissingGroupLaneRect,
  InvalidSprite,
  FrameLimitExceeded,
  UnsafeCardinality,
  NonFiniteGeometry,
};

struct SkinNoteLineNormalizationResult {
  std::optional<SkinNormalizedNoteLines> lines;
  SkinNoteLineNormalizationError error = SkinNoteLineNormalizationError::None;
};

// Mirrors JsonPlaySkinObjectLoader's Note group/bpm/stop/time handling at
// Beatoraja c2ed5db1. Group length establishes the lane-group index space and
// group[i].dst[0] establishes each lane rectangle. BPM, stop, and time each
// produce one indexed result per group: their authored prefix is
// min(group.size(), authoredArray.size()), while the remaining slots are null
// Image/Destination holes, just like the loader's group-sized Java arrays.
[[nodiscard]] SkinNoteLineNormalizationResult
normalizeSkinNoteLines(const SkinNoteLineNormalizationInput &input);

} // namespace skin
