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
#include <utility>

struct ReplayPlayfieldPresentationCreateInfo {
  bms_parser::Chart &chart;
  std::map<Judgement, std::pair<long long, long long>> timingWindows;
  const PlayfieldPresentationConfig &configuration;
  const AppSettings &settings;
  audio::PlaybackRate playback;
  IGameplayBgaSubmitter &bga;
  GameplaySkinSessionServices skinServices;
  GameplaySkinSessionInput skinInput;
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

  void applyReplayEvent(const ReplayEvent &, const PlayfieldJudgeEventClock &,
                        bool recordTimingSample);
  void applyAuthorityUpdate(const PlayfieldAuthorityUpdate &);
  [[nodiscard]] PresentationFrameResult
  renderFrame(RenderContext &, PlayfieldFrameClock,
              const PlayfieldProjectionRequest &);
  [[nodiscard]] BMSRenderer &builtInRenderer() noexcept;

private:
  ReplayPlayfieldPresentation(std::unique_ptr<PlayfieldChartVisualModel>,
                              std::unique_ptr<PlayfieldVisualStateStore>,
                              std::unique_ptr<PlayfieldProjection>,
                              std::unique_ptr<PlayfieldPresentationCoordinator>,
                              BMSRenderer *, PlayfieldAuthorityUpdate);

  [[nodiscard]] std::optional<ChartVisualId>
  replayNoteId(const ReplayEvent &) const;
  void applyReplayNote(const ReplayEvent &, bool judged, bool dead,
                       bool longActive);
  [[nodiscard]] PresentationFailure
  makeReplayFrameFailure(std::uint64_t serial) const;

  std::unique_ptr<PlayfieldChartVisualModel> chartModel_;
  std::unique_ptr<PlayfieldVisualStateStore> state_;
  std::unique_ptr<PlayfieldProjection> projection_;
  std::unique_ptr<PlayfieldPresentationCoordinator> coordinator_;
  std::unique_ptr<PlayfieldPresentationEventFanout> events_;
  BMSRenderer *builtIn_ = nullptr;
  PlayfieldAuthorityUpdate authority_;
};
