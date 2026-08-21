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

struct SkinTimingVisualizerRenderRequest {
  SkinObjectId sourceObject = 0;
  std::uint32_t authoredOrdinal = 0;
  const SkinTimingVisualizerObject &visualizer;
  SkinGameplayGraphStateView state;
  const AuthoredDestinationGeometry &geometry;
  const PlaySkinViewport &viewport;
  std::size_t maximumCommands = 0;
  std::size_t maximumPrimitiveVertices = 0;
};

using SkinTimingVisualizerRenderResult = SkinGeneratedTextureRasterResult;

[[nodiscard]] SkinTimingVisualizerRenderResult
renderSkinTimingVisualizer(const SkinTimingVisualizerRenderRequest &request);

} // namespace skin
