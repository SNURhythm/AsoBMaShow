#pragma once

#include "BeatorajaSkinModel.h"
#include "PlaySkinViewport.h"
#include "SkinDestinationEvaluator.h"
#include "SkinDrawCommand.h"
#include "../../scene/play/SkinGameplayGraphState.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace skin {

struct SkinBpmGraphRenderRequest {
  SkinObjectId sourceObject = 0;
  std::uint32_t authoredOrdinal = 0;
  const SkinBpmGraphObject &graph;
  SkinGameplayGraphStateView state;
  const AuthoredDestinationGeometry &geometry;
  const PlaySkinViewport &viewport;
  std::int64_t elapsedMillis = 0;
  std::size_t maximumCommands = 0;
  std::size_t maximumPrimitiveVertices = 0;
};

struct SkinBpmGraphRenderResult {
  std::vector<SkinDrawCommand> commands;
  std::size_t primitiveVertices = 0;
  std::optional<SkinDiagnostic> failure;
};

[[nodiscard]] SkinBpmGraphRenderResult
renderSkinBpmGraph(const SkinBpmGraphRenderRequest &request);

} // namespace skin
