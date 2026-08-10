#pragma once

#include "ReplayPlayfieldPresentation.h"

#include "../../PreparationPlan.h"
#include "../../ReplayVideoExporter.h"
#include "../../video/RendererAccessCoordinator.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace replay_video_export {

[[nodiscard]] skin::UiLogicalRect
replayGameplayLogicalUiBounds(int exportWidth, int exportHeight) noexcept;

struct ReplayGameplayFrameState {
  PlayfieldFrameClock clock;
  long long sceneStartMicros = kPlayfieldTimestampOff;
  long long playStartMicros = kPlayfieldTimestampOff;
};

[[nodiscard]] ReplayGameplayFrameState replayGameplayFrameState(
    const preparation::Plan &, const bms_parser::Chart &, const ReplayData &,
    const AppSettings &, std::uint64_t serial,
    long long realTimeMicros) noexcept;

// Export replays map to BMSPlayerMode.REPLAY, which always ends after
// lastNoteTime + TIME_MARGIN. Its separate AUTOPLAY mode uses lastTime.
[[nodiscard]] long long replayGameplayStatePlayDeadlineMicros(
    const bms_parser::Chart &, const ReplayData &) noexcept;

// Extends only a selected-skin replay path through the same BMSPlayer
// end-of-notes, finishmargin, and fadeout lifecycle as interactive gameplay.
[[nodiscard]] long long replayGameplayDurationWithSelectedSkinAnimation(
    const bms_parser::Chart &, const ReplayData &, const preparation::Plan &,
    long long audioOffsetMicros, int fps, long long requestedDurationMicros,
    bool stoppedOnGaugeFailure, const ReplayPlayfieldPresentation *);

struct ReplayLaneCoverFrameState {
  int percent = 0;
  bool resetVisibleTimeReference = false;
};

class ReplayLaneCoverPlayback final {
public:
  explicit ReplayLaneCoverPlayback(int initialPercent) noexcept
      : percent_(initialPercent) {}

  [[nodiscard]] ReplayLaneCoverFrameState
  advance(std::span<const ReplayLaneCoverEvent>, long long songTimeMicros);

private:
  std::size_t cursor_ = 0;
  int percent_ = 0;
};

// Course stages own separate presentation adapters, but their already-earned
// maximum combo is one course-lifetime authority. Observing a fresh stage must
// never reduce the value established by earlier replay events.
class ReplayCourseMaximumComboPlayback final {
public:
  [[nodiscard]] int
  observe(const ReplayPlayfieldPresentation &presentation) noexcept;

private:
  int maximumCombo_ = 0;
};

[[nodiscard]] std::string
skinExportFailureMessage(const PresentationFailure &failure);

// The normal exporter supplies its live BGA submitter and skin acquisition
// services here. Keeping those dependencies explicit lets tests exercise the
// same selected-skin factory failure path without constructing audio/video
// output systems.
[[nodiscard]] std::optional<ReplayVideoExportResult>
preflightReplayGameplayPresentation(
    bms_parser::Chart &, const ReplayData &, const AppSettings &,
    const preparation::Plan &, const PlayfieldPresentationConfig &, int, int,
    IGameplayBgaSubmitter &, GameplaySkinSessionServices,
    display::RendererAccessCoordinator &,
    std::unique_ptr<ReplayPlayfieldPresentation> &);

void destroyReplayGameplayPresentation(
    display::RendererAccessCoordinator &,
    std::unique_ptr<ReplayPlayfieldPresentation> &);

// A minimal course-stage boundary shared by the exporter and focused tests.
// Its input owns no chart or replay data; the caller retains those lifetimes
// while this loop constructs one presentation per encoded stage.
struct CourseReplayGameplayPreflightStage {
  bms_parser::Chart &chart;
  const ReplayData &replay;
  const preparation::Plan &preparationPlan;
  PlayfieldPresentationConfig configuration;
  int exportWidth = 0;
  int exportHeight = 0;
  GameplaySkinSessionServices skinServices;
  std::unique_ptr<ReplayPlayfieldPresentation> &presentation;
};

[[nodiscard]] std::optional<ReplayVideoExportResult>
preflightCourseReplayGameplayPresentations(
    std::vector<CourseReplayGameplayPreflightStage> &, IGameplayBgaSubmitter &,
    const AppSettings &, display::RendererAccessCoordinator &);

} // namespace replay_video_export
