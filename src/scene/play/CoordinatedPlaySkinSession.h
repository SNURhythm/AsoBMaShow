#pragma once

#include "PlayfieldPresentation.h"
#include "../../skin/SkinProfileSettings.h"
#include "../../skin/beatoraja/PlaySkinSessionIdentity.h"
#include "../../skin/beatoraja/PlaySkinViewport.h"
#include "../../skin/beatoraja/SyntheticReplayGhostOverlay.h"

#include <cstdint>
#include <span>
#include <vector>

// Renderer-independent chart-lifetime boundary used by the coordinator.
// The concrete Lua session implements it when that feature is enabled; the
// built-in-only build still compiles the replay coordinator without linking
// the Lua/resource graph.
class CoordinatedPlaySkinSession {
public:
  virtual ~CoordinatedPlaySkinSession() = default;

  [[nodiscard]] virtual const skin::PlaySkinSessionIdentity &
  identity() const noexcept = 0;
  [[nodiscard]] virtual std::optional<skin::SkinGameplayTiming>
  selectedSkinGameplayTiming() const {
    return std::nullopt;
  }
  [[nodiscard]] virtual PresentationFrameOutcome prepareFrame(
      const PlayfieldVisualState &, const PlayfieldProjectionResult &) = 0;
  [[nodiscard]] virtual PresentationFrameResult
  render(RenderContext &, const PreparedGameplayBgaFrame &,
         IGameplayBgaSubmitter &) = 0;
  // Optional application-owned replay decoration. A concrete skin session
  // emits it only after its authored frame is successfully submitted.
  virtual void submitSyntheticReplayGhosts(
      RenderContext &, const skin::SyntheticReplayGhostFrameInput &) {}
  // Start-lane indicators are application-owned preparation feedback. They
  // are submitted through the selected skin only after its authored frame,
  // never by re-enabling the built-in playfield.
  virtual void submitSyntheticStartLaneIndicators(RenderContext &,
                                                  std::uint64_t,
                                                  std::span<const int>) {}
  virtual void setViewport(skin::ViewportSettings) = 0;
  virtual void updateViewportGeometry(skin::UiLogicalRect) = 0;
  [[nodiscard]] virtual gameplay::RealtimeTouchLayout touchLayout() const = 0;
  [[nodiscard]] virtual std::uint64_t
  touchLayoutRevision() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t
  touchHitRegionsRevision() const noexcept = 0;
  [[nodiscard]] virtual std::vector<PresentationUiHitRegion>
  touchHitRegions() const = 0;
  [[nodiscard]] virtual PresentationUiHit
  hitTestUiControl(UiLogicalPoint) const = 0;
  virtual PresentationTouchResult
  beginPresentationTouch(const PresentationTouchEvent &) = 0;
  virtual PresentationTouchResult
  updatePresentationTouch(const PresentationTouchEvent &) = 0;
  virtual PresentationTouchResult
  endPresentationTouch(const PresentationTouchEvent &, bool cancelled) = 0;
  virtual void cancelPresentationTouches(long long eventMicros) = 0;
  virtual void onLanePressed(int, JudgeResult, long long) = 0;
  virtual void onLaneReleased(int, long long) = 0;
  virtual void onJudge(JudgeResult, int, int, PlayfieldJudgeEventClock,
                       bool) = 0;
};
