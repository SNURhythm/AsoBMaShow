#pragma once

#include "BeatorajaSkinModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace skin {

// The already-expanded source sprite, ref binding, and child destination used
// by JsonPlaySkinObjectLoader's Judge branch. This boundary deliberately does
// not depend on Lua tables or renderer resources.
struct SkinJudgeNumberNormalizationInput {
  SkinSpriteFrames source;
  // JsonSkin.Value's raw selector is `ref`, rather than `value`. The live
  // decoder resolves that selector to an IntegerValue-domain binding before
  // this helper, preserving named/function/script binding support.
  SkinIntegerPropertyId value{};
  int digitCount = 0;
  int spacing = 0;
  std::vector<SkinDigitOffset> offsets;
  SkinDestinationBody destination;
};

struct SkinJudgeNumberPresentation {
  SkinNumberObject number;
  SkinDestinationBody destination;
};

struct SkinJudgeNumberNormalizationPolicy {
  // Matches the decoder's model-wide materialized sprite-frame bound while
  // keeping this source-neutral helper independent of decoder implementation.
  static constexpr std::size_t maxMaterializedFrames = 200'000;
  static constexpr std::size_t maxDigitOffsets = 256;
  static constexpr int maxDigitCount = 256;
};

enum class SkinJudgeNumberNormalizationError : std::uint8_t {
  None,
  EmptyFrames,
  FrameLimitExceeded,
  OffsetLimitExceeded,
  MissingValueBinding,
  InvalidDigitCount,
  NonFiniteGeometry,
  InvalidIntegerGeometry,
};

struct SkinJudgeNumberNormalizationResult {
  std::optional<SkinJudgeNumberPresentation> number;
  SkinJudgeNumberNormalizationError error =
      SkinJudgeNumberNormalizationError::None;
};

// Mirrors the Judge child-number construction in JsonPlaySkinObjectLoader at
// Beatoraja c2ed5db1. Unlike the generic Number path, Judge uses only 10 or
// 11 glyph rows: divisibility by 10 wins, incomplete 11-glyph tails are
// ignored, and no 24-glyph signed partition is considered.
//
// Integration note: this intentionally handles one resolved child. A later
// decoder integration must retain the Judge's sparse first-seven grade slots
// and map JsonSkin.Judge.shift to SkinJudgeObject's
// shiftImageByHalfDetailWidth; neither requires changing this pure helper.
[[nodiscard]] SkinJudgeNumberNormalizationResult
normalizeSkinJudgeNumber(const SkinJudgeNumberNormalizationInput &input);

} // namespace skin
