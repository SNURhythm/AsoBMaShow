#pragma once

#include "PlayfieldPresentationEvents.h"
#include "PlayfieldVisualState.h"
#include "RealtimeTouchInputRouter.h"

class PlayfieldPresentation : public IPlayfieldPresentationEvents {
public:
  ~PlayfieldPresentation() override = default;

  virtual void configure(const PlayfieldPresentationConfig &configuration) = 0;
  [[nodiscard]] virtual gameplay::RealtimeTouchLayout touchLayout() const = 0;
  // Cheap stable identity for the currently published touch topology. Unlike
  // touchLayout(), this must not copy lane geometry and must not advance for a
  // normal render frame.
  [[nodiscard]] virtual std::uint64_t
  touchLayoutRevision() const noexcept = 0;
  // Cheap identity for presentation hit geometry. This may advance while the
  // lane topology remains stable; callers republish hit regions without
  // cancelling gameplay-lane ownership.
  [[nodiscard]] virtual std::uint64_t
  touchHitRegionsRevision() const noexcept = 0;
  [[nodiscard]] virtual std::vector<PresentationUiHitRegion>
  touchHitRegions() const = 0;
  [[nodiscard]] virtual PresentationUiHit
  hitTestUiControl(UiLogicalPoint point) const = 0;
  virtual PresentationTouchResult
  beginPresentationTouch(const PresentationTouchEvent &event) = 0;
  virtual PresentationTouchResult
  updatePresentationTouch(const PresentationTouchEvent &event) = 0;
  virtual PresentationTouchResult
  endPresentationTouch(const PresentationTouchEvent &event,
                       bool cancelled) = 0;
  virtual void cancelPresentationTouches(long long eventMicros) = 0;
  virtual void reset() = 0;
  virtual void refreshGeometry() = 0;
};
