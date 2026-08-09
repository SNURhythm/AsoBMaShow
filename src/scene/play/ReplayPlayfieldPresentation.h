#pragma once

#include "BMSRenderer.h"
#include "GameplaySkinSessionFactory.h"
#include "PlayfieldPresentationCoordinator.h"

#include "../../AppSettings.h"
#include "../../ReplayData.h"
#include "../../audio/PlaybackRate.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

struct ReplayPlayfieldPresentationCreateInfo {
  bms_parser::Chart &chart;
  std::map<Judgement, std::pair<long long, long long>> timingWindows;
  const PlayfieldPresentationConfig &configuration;
  const AppSettings &settings;
  audio::PlaybackRate playback;
  IGameplayBgaSubmitter &bga;
  GameplaySkinSessionServices skinServices;
  GameplaySkinSessionInput skinInput;
  // Replay input is copied into the visual store before the initial state is
  // captured, so both selected skins and built-in projection observe one
  // authoritative touch timeline.
  std::vector<ReplayTouchSample> replayTouchSamples;
  std::function<void(const PresentationFailure &)> recordFailure;
};

class ReplayPlayfieldPresentation;

struct ReplayPlayfieldPresentationCreateResult {
  std::unique_ptr<ReplayPlayfieldPresentation> presentation;
  std::optional<PresentationFailure> failure;
};

// Chart-lifetime replay presentation boundary.  The exporter supplies replay
// authority and clocks; this adapter owns the sole mutable visual state and
// submits its one captured state/projection pair through the coordinator.
class ReplayPlayfieldPresentation final {
public:
  static ReplayPlayfieldPresentationCreateResult
  create(ReplayPlayfieldPresentationCreateInfo);

  // Mirrors the export reducer's HUD-applied result. Callers use it for the
  // same replay-only counter/pacemaker side effects as normal and course
  // export loops; classic LN heads deliberately return false.
  [[nodiscard]] bool applyReplayEvent(const ReplayEvent &,
                                      const PlayfieldJudgeEventClock &,
                                      bool recordTimingSample);
  void applyAuthorityUpdate(const PlayfieldAuthorityUpdate &);
  [[nodiscard]] PresentationFrameResult
  renderFrame(RenderContext &, PlayfieldFrameClock,
              const PlayfieldProjectionRequest &);
  [[nodiscard]] BMSRenderer &builtInRenderer() noexcept;

#if defined(ASOBMASHOW_REPLAY_PLAYFIELD_PRESENTATION_TESTING)
  [[nodiscard]] PlayfieldVisualState
  captureVisualStateForTesting(PlayfieldFrameClock) const;
#endif

private:
  ReplayPlayfieldPresentation(std::unique_ptr<PlayfieldChartVisualModel>,
                              std::unique_ptr<PlayfieldVisualStateStore>,
                              std::unique_ptr<PlayfieldProjection>,
                              std::unique_ptr<PlayfieldPresentationCoordinator>,
                              BMSRenderer *, PlayfieldAuthorityUpdate);

  [[nodiscard]] const ChartVisualNote *replayNote(const ReplayEvent &) const;
  [[nodiscard]] NotePresentationState *noteState(ChartVisualId) noexcept;
  void publishNoteState(ChartVisualId);
  void setReplayGauge(const ReplayEvent &);
  void markReplayMissedNote(const ChartVisualNote &, long long);
  void updateLongVisualState(const ChartVisualNote &);

  std::unique_ptr<PlayfieldChartVisualModel> chartModel_;
  std::unique_ptr<PlayfieldVisualStateStore> state_;
  std::unique_ptr<PlayfieldProjection> projection_;
  std::unique_ptr<PlayfieldPresentationCoordinator> coordinator_;
  std::unique_ptr<PlayfieldPresentationEventFanout> events_;
  BMSRenderer *builtIn_ = nullptr;
  PlayfieldAuthorityUpdate authority_;
  std::unordered_map<ChartVisualId, NotePresentationState> noteStates_;
};
