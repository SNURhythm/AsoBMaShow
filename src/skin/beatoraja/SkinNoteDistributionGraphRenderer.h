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

struct SkinNoteDistributionGraphRenderRequest {
  SkinObjectId sourceObject = 0;
  std::uint32_t authoredOrdinal = 0;
  const SkinNoteDistributionGraphObject &graph;
  SkinGameplayGraphStateView state;
  const AuthoredDestinationGeometry &geometry;
  const PlaySkinViewport &viewport;
  bool pmsMode = false;
  std::int64_t elapsedMillis = 0;
  std::optional<std::int64_t> startMillis;
  std::optional<std::int64_t> endMillis;
  std::optional<std::int64_t> currentMillis;
  std::size_t maximumCommands = 0;
  std::size_t maximumPrimitiveVertices = 0;
  SkinGeneratedTextureCache *cache = nullptr;
};

using SkinNoteDistributionGraphRenderResult = SkinGeneratedTextureRasterResult;

[[nodiscard]] SkinNoteDistributionGraphRenderResult
renderSkinNoteDistributionGraph(
    const SkinNoteDistributionGraphRenderRequest &request);

} // namespace skin
