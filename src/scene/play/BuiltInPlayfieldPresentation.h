#pragma once

#include "../../audio/PlaybackRate.h"
#include "Judgement.h"
#include "PlayfieldPresentation.h"

#include <cstdint>
#include <map>
#include <memory>
#include <utility>

namespace bms_parser {
class Chart;
}

struct BuiltInRendererTraversal;

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
};

struct BuiltInPlayfieldPresentationCreateInfo {
  // Borrowed and non-null. The caller must keep this chart alive and unchanged
  // until the returned presentation has been destroyed.
  bms_parser::Chart &chart;
  std::map<Judgement, std::pair<long long, long long>> timingWindows;
  int visibleTimeGreenNumber = 0;
  bool renderHud = true;
  audio::PlaybackRate playbackRate;
};

[[nodiscard]] std::unique_ptr<BuiltInPlayfieldPresentation>
createBuiltInPlayfieldPresentation(
    BuiltInPlayfieldPresentationCreateInfo creation);
