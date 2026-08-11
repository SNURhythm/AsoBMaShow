#pragma once

#include <cstdint>

// The replay-document and live-play representations share this one semantic
// discriminator. Value is LaneRenderer.setLanecover; Enabled is
// LaneRenderer.setEnableLanecover, which intentionally must not reset fixed
// Hi-Speed.
enum class ReplayLaneCoverChangeKind : std::uint8_t {
  Value = 0,
  Enabled = 1,
};
