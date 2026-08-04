#pragma once

#include "../../audio/PlaybackRate.h"
#include "Judgement.h"
#include "PlayfieldPresentation.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace bms_parser {
class Chart;
}

struct BuiltInRendererTraversal;
struct ReplayData;

// The built-in renderer's projection state remains available without leaking
// BMSRenderer into scene/coordinator ownership. Returning a value freezes the
// retained traversal cursor and geometry for the same immutable frame capture.
class BuiltInPlayfieldPresentation : public PlayfieldPresentation {
public:
  ~BuiltInPlayfieldPresentation() override = default;

  [[nodiscard]] virtual BuiltInRendererTraversal
  projectionTraversal() const = 0;
  [[nodiscard]] virtual long long
  projectionLatePoorTimingMicros() const noexcept = 0;
  // Narrow legacy lane-cover drag seam retained for the scene's native touch
  // adapter. Gameplay state remains authoritative outside the presentation.
  [[nodiscard]] virtual std::optional<float>
  laneCoverHandleGrabOffset(float renderX, float renderY) const = 0;
  [[nodiscard]] virtual int
  dragLaneCoverHandleTo(float renderX, float renderY,
                        float lanePointYOffset) = 0;
};

struct BuiltInPlayfieldPresentationCreateInfo {
  // Borrowed and non-null. The caller must keep this chart alive and unchanged
  // until the returned presentation has been destroyed.
  bms_parser::Chart &chart;
  std::map<Judgement, std::pair<long long, long long>> timingWindows;
  int visibleTimeGreenNumber = 0;
  bool renderHud = true;
  audio::PlaybackRate playbackRate;
  // Borrowed for construction only. The concrete presentation preprocesses
  // replay ghosts/touches before the factory returns and retains no ReplayData
  // pointer.
  const ReplayData *replayData = nullptr;
};

[[nodiscard]] std::unique_ptr<BuiltInPlayfieldPresentation>
createBuiltInPlayfieldPresentation(
    BuiltInPlayfieldPresentationCreateInfo creation);
