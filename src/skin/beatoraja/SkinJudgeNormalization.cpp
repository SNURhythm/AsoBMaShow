#include "SkinJudgeNormalization.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
#include <cmath>
#include <utility>

namespace skin {
namespace {

SkinJudgeNormalizationResult
failure(SkinJudgeNormalizationError error) {
  return {.judge = std::nullopt, .error = error};
}

bool finiteDestination(const SkinDestinationBody &destination) noexcept {
  if (destination.frames.size() >
      SkinJudgeNormalizationPolicy::maxDestinationFramesPerChild) {
    return false;
  }
  if (destination.mouseRect &&
      (!std::isfinite(destination.mouseRect->x) ||
       !std::isfinite(destination.mouseRect->y) ||
       !std::isfinite(destination.mouseRect->width) ||
       !std::isfinite(destination.mouseRect->height))) {
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

SkinJudgeNormalizationError
validateImages(const SkinAuthoredJudgeImages &children) {
  for (const auto &child : children) {
    if (!child) {
      continue;
    }
    if (child->destination.frames.size() >
        SkinJudgeNormalizationPolicy::maxDestinationFramesPerChild) {
      return SkinJudgeNormalizationError::FrameLimitExceeded;
    }
    if (!finiteDestination(child->destination)) {
      return SkinJudgeNormalizationError::NonFiniteGeometry;
    }
  }
  return SkinJudgeNormalizationError::None;
}

SkinJudgeNormalizationError
validateNumbers(const SkinAuthoredJudgeNumbers &children,
                std::size_t requiredCount) {
  for (std::size_t grade = 0; grade < requiredCount; ++grade) {
    const auto &child = children[grade];
    if (!child) {
      continue;
    }
    const auto &destination = child->presentation.destination;
    if (destination.frames.size() >
        SkinJudgeNormalizationPolicy::maxDestinationFramesPerChild) {
      return SkinJudgeNormalizationError::FrameLimitExceeded;
    }
    if (!finiteDestination(destination)) {
      return SkinJudgeNormalizationError::NonFiniteGeometry;
    }
  }
  return SkinJudgeNormalizationError::None;
}

} // namespace

SkinJudgeNormalizationResult
normalizeSkinJudge(const SkinJudgeNormalizationInput &input) {
  if (input.images.size() > SkinJudgeNormalizationPolicy::maxAuthoredGrades ||
      input.numbers.size() > SkinJudgeNormalizationPolicy::maxAuthoredGrades) {
    return failure(SkinJudgeNormalizationError::GradeLimitExceeded);
  }
  // JsonPlaySkinObjectLoader allocates by images.length and then evaluates
  // judge.numbers[i] for every image grade. Extra number entries are harmless,
  // but a shorter array is unsafe.
  if (input.numbers.size() < input.images.size()) {
    return failure(SkinJudgeNormalizationError::UnsafeCardinality);
  }
  if (const auto imageValidation = validateImages(input.images);
      imageValidation != SkinJudgeNormalizationError::None) {
    return failure(imageValidation);
  }
  if (const auto numberValidation =
          validateNumbers(input.numbers, input.images.size());
      numberValidation != SkinJudgeNormalizationError::None) {
    return failure(numberValidation);
  }

  SkinNormalizedJudge normalized;
  normalized.player = input.player;
  normalized.shiftImageByHalfDetailWidth = input.shiftImageByHalfDetailWidth;
  const std::size_t retained = std::min(
      input.images.size(), SkinJudgeNormalizationPolicy::runtimeGradeSlots);
  for (std::size_t grade = 0; grade < retained; ++grade) {
    normalized.grades[grade].image = input.images[grade];
    normalized.grades[grade].detailNumber = input.numbers[grade];
  }

  return {.judge = std::move(normalized),
          .ignoredAuthoredGrades = input.images.size() - retained,
          .error = SkinJudgeNormalizationError::None};
}

} // namespace skin

#endif
