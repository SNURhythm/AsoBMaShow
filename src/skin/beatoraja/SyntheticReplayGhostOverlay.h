#pragma once

#include "../../ReplayGhostUtils.h"
#include "SkinDrawCommand.h"

#include <cstdint>
#include <span>
#include <vector>

namespace skin {

// This is an application overlay, not a Beatoraja skin object. It is emitted
// only for replay playback when the existing replay-ghost option is enabled.
// Lane rectangles come from the selected SkinNote object that lowered the
// current frame; the scroll scale deliberately remains shared, mirroring
// Beatoraja LaneRenderer's lanes[0].region conversion.
struct SyntheticReplayGhostLaneGeometry {
  int lane = -1;
  AuthoredRect normalNote;
  AuthoredRect clip;
};

struct SyntheticReplayGhostGeometry {
  std::uint64_t frameSerial = 0;
  PlaySkinViewport viewport;
  // Lower authored edge of the primary lane region. The live lane-cover
  // traversal turns its visible height into this coordinate space.
  double sharedLaneOriginY = 0.0;
  double sharedLaneHeight = 0.0;
  std::vector<SyntheticReplayGhostLaneGeometry> lanes;
};

struct SyntheticReplayGhostFrameInput {
  std::uint64_t frameSerial = 0;
  long long visualTimeMicros = 0;
  double currentScrollPosition = 0.0;
  // Captured LaneRenderer::getHispeed()-equivalent value from the same
  // projection snapshot that supplied currentScrollPosition.
  double hispeed = 0.0;
  // Portion of the primary lane height remaining below the live lane cover.
  // This is derived from the captured BMSRenderer traversal rather than from
  // a second interpretation of cover configuration.
  double visibleLaneHeightRatio = 1.0;
  bool enabled = false;
  std::span<const ReplayGhostEvent> events;
};

[[nodiscard]] SkinCommandBuffer buildSyntheticReplayGhostOverlay(
    const SyntheticReplayGhostGeometry &,
    const SyntheticReplayGhostFrameInput &);

} // namespace skin
