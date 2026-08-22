#pragma once

#include "BeatorajaSkinModel.h"
#include "PlaySkinViewport.h"
#include "SkinDestinationEvaluator.h"
#include "SkinDrawCommand.h"
#include "SkinGeneratedTextureRaster.h"
#include "../../scene/play/SkinGameplayGraphState.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace skin {

struct SkinHitErrorVisualizerPresentationState {
  std::optional<std::size_t> recentJudgeTimingIndex;
  std::int64_t emaMillis = 0;
};

[[nodiscard]] bool advanceSkinHitErrorVisualizerEma(
    const SkinHitErrorVisualizerObject &visualizer,
    SkinGameplayGraphStateView state,
    SkinHitErrorVisualizerPresentationState &presentation) noexcept;

struct SkinHitErrorVisualizerRenderRequest {
  SkinObjectId sourceObject = 0;
  std::uint32_t authoredOrdinal = 0;
  const SkinHitErrorVisualizerObject &visualizer;
  SkinGameplayGraphStateView state;
  std::int64_t emaMillis = 0;
  const AuthoredDestinationGeometry &geometry;
  const PlaySkinViewport &viewport;
  std::size_t maximumCommands = 0;
  std::size_t maximumPrimitiveVertices = 0;
  SkinGeneratedTextureCache *cache = nullptr;
};

using SkinHitErrorVisualizerRenderResult = SkinGeneratedTextureRasterResult;

[[nodiscard]] SkinHitErrorVisualizerRenderResult
renderSkinHitErrorVisualizer(
    const SkinHitErrorVisualizerRenderRequest &request);

} // namespace skin
