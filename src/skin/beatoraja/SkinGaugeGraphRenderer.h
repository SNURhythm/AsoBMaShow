#pragma once

#include "BeatorajaSkinModel.h"
#include "PlaySkinViewport.h"
#include "SkinDestinationEvaluator.h"
#include "SkinGeneratedTextureRaster.h"
#include "../../scene/play/SkinGameplayGraphState.h"

#include <cstddef>
#include <cstdint>

namespace skin {

struct SkinGaugeGraphRenderRequest {
  SkinObjectId sourceObject = 0;
  std::uint32_t authoredOrdinal = 0;
  const SkinGaugeGraphObject &graph;
  SkinGameplayGraphStateView state;
  const AuthoredDestinationGeometry &geometry;
  const PlaySkinViewport &viewport;
  std::int64_t elapsedMillis = 0;
  std::size_t maximumCommands = 0;
  std::size_t maximumPrimitiveVertices = 0;
};

using SkinGaugeGraphRenderResult = SkinGeneratedTextureRasterResult;

[[nodiscard]] SkinGaugeGraphRenderResult
renderSkinGaugeGraph(const SkinGaugeGraphRenderRequest &request);

} // namespace skin
