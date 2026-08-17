#pragma once

#include "../../ReplayGhostUtils.h"
#include "SkinDrawCommand.h"

#include <array>
#include <cstdint>
#include <optional>
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

// Application HUD shown above a selected skin is anchored to the evaluated
// SkinNote source. The lane span is expressed in UI logical coordinates after
// the skin's active viewport transform.
struct SelectedSkinHudGeometry {
  std::uint64_t frameSerial = 0;
  UiLogicalRect playArea;
  double judgementLineY = 0.0;
  std::size_t laneCount = 0;
};

[[nodiscard]] std::optional<SelectedSkinHudGeometry>
selectedSkinHudGeometry(const SyntheticReplayGhostGeometry &);

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

// The start-lane cue is application-owned, but a selected skin must remain
// the only presentation owner.  Its triangles therefore use the selected
// SkinNote lane rectangles and the same post-skin overlay submission path as
// replay ghosts.
struct SyntheticStartLaneIndicatorLaneGeometry {
  int lane = -1;
  AuthoredRect laneRegion;
  std::array<float, 4> rgba{1.0F, 1.0F, 1.0F, 1.0F};
};

struct SyntheticStartLaneIndicatorFrameInput {
  std::uint64_t frameSerial = 0;
  std::span<const int> lanes;
  // Same captured visible-height fraction that produces the built-in
  // noteVisibleUpperBound. It places the cue directly below lane cover.
  double visibleLaneHeightRatio = 1.0;
};

[[nodiscard]] SkinCommandBuffer buildSyntheticStartLaneIndicatorOverlay(
    const PlaySkinViewport &,
    std::span<const SyntheticStartLaneIndicatorLaneGeometry>,
    const SyntheticStartLaneIndicatorFrameInput &);

} // namespace skin
