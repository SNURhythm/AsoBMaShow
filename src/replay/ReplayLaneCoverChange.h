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

// One recorded mutation, after the replay cursor has applied its values. A
// single export frame can contain more than one of these; their order matters
// because only Value invokes LaneRenderer.setLanecover/resetHispeed.
struct ReplayLaneCoverTransition {
  int percent = 0;
  bool enabled = false;
  ReplayLaneCoverChangeKind changeKind = ReplayLaneCoverChangeKind::Value;
  bool resetVisibleTimeReference = false;
};
