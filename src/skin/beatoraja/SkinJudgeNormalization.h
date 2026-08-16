#pragma once

#include "SkinJudgeNumberNormalization.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skin {

// These inline children preserve the first matching authored IDs and array
// indexes without minting model object IDs. The pinned loader independently
// resolves Image and Value definitions for each Judge grade, so either child
// can be absent while its sibling remains usable.
struct SkinJudgeInlineImageChild {
  std::string authoredId;
  std::size_t authoredIndex = 0;
  SkinImageObject image;
  SkinDestinationBody destination;
};

struct SkinJudgeInlineNumberChild {
  std::string authoredId;
  std::size_t authoredIndex = 0;
  SkinJudgeNumberPresentation presentation;
};

using SkinAuthoredJudgeImages =
    std::vector<std::optional<SkinJudgeInlineImageChild>>;
using SkinAuthoredJudgeNumbers =
    std::vector<std::optional<SkinJudgeInlineNumberChild>>;

struct SkinJudgeNormalizationInput {
  SkinAuthoredJudgeImages images;
  SkinAuthoredJudgeNumbers numbers;
  // The pinned JudgeManager treats every out-of-range player as no current
  // judge. Retain the authored value instead of applying an invented limit.
  int player = 0;
  bool shiftImageByHalfDetailWidth = false;
};

struct SkinJudgeNormalizationPolicy {
  static constexpr std::size_t runtimeGradeSlots = 7;
  static constexpr std::size_t maxAuthoredGrades = 256;
  static constexpr std::size_t maxDestinationFramesPerChild = 4'096;
};

struct SkinJudgeRuntimeMetadata {
  // SkinJudge stores exactly seven entries. During a max-gauge grade-zero
  // event it prefers grade six when present, otherwise falls back to grade
  // zero.
  std::size_t maxGaugePreferredGrade = 6;
  std::size_t maxGaugeFallbackGrade = 0;
  // Outside the max-gauge branch SkinJudge renders a detail number only for
  // grades 0, 1, and 2. This describes renderer behavior; it creates no IDs.
  std::size_t detailNumberVisibleGradeCount = 3;
};

struct SkinNormalizedJudgeGrade {
  std::optional<SkinJudgeInlineImageChild> image;
  std::optional<SkinJudgeInlineNumberChild> detailNumber;
};

// Pending, source-neutral Judge representation. It is intentionally not a
// live SkinJudgeObject model payload: decoder integration can consume the
// preserved inline presentations later without this helper inventing IDs.
struct SkinNormalizedJudge {
  std::array<SkinNormalizedJudgeGrade,
             SkinJudgeNormalizationPolicy::runtimeGradeSlots>
      grades;
  int player = 0;
  bool shiftImageByHalfDetailWidth = false;
  SkinJudgeRuntimeMetadata runtime;
};

enum class SkinJudgeNormalizationError : std::uint8_t {
  None,
  GradeLimitExceeded,
  UnsafeCardinality,
  FrameLimitExceeded,
  NonFiniteGeometry,
};

struct SkinJudgeNormalizationResult {
  std::optional<SkinNormalizedJudge> judge;
  // Pinned SkinJudge copies only the first seven children, so this makes
  // otherwise silent extra authored image grades observable to a caller.
  std::size_t ignoredAuthoredGrades = 0;
  SkinJudgeNormalizationError error = SkinJudgeNormalizationError::None;
};

// Mirrors JsonPlaySkinObjectLoader and SkinJudge at Beatoraja c2ed5db1. The
// Java loader sizes both temporary arrays from images.length and directly
// indexes numbers[i]; shorter number arrays are rejected here rather than
// reproducing that out-of-bounds crash. Extra image grades are retained only
// as ignored-count metadata because SkinJudge's fixed runtime arrays copy the
// first seven slots, preserving holes and independently resolved children.
[[nodiscard]] SkinJudgeNormalizationResult
normalizeSkinJudge(const SkinJudgeNormalizationInput &input);

} // namespace skin
