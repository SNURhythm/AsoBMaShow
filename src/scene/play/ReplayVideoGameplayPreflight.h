#pragma once

#include "ReplayPlayfieldPresentation.h"

#include "../../ReplayVideoExporter.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace replay_video_export {

[[nodiscard]] std::string
skinExportFailureMessage(const PresentationFailure &failure);

// The normal exporter supplies its live BGA submitter and skin acquisition
// services here. Keeping those dependencies explicit lets tests exercise the
// same selected-skin factory failure path without constructing audio/video
// output systems.
[[nodiscard]] std::optional<ReplayVideoExportResult>
preflightReplayGameplayPresentation(
    bms_parser::Chart &, const ReplayData &, const AppSettings &,
    const PlayfieldPresentationConfig &, audio::PlaybackRate,
    skin::UiLogicalRect, IGameplayBgaSubmitter &, GameplaySkinSessionServices,
    std::unique_ptr<ReplayPlayfieldPresentation> &);

// A minimal course-stage boundary shared by the exporter and focused tests.
// Its input owns no chart or replay data; the caller retains those lifetimes
// while this loop constructs one presentation per encoded stage.
struct CourseReplayGameplayPreflightStage {
  bms_parser::Chart &chart;
  const ReplayData &replay;
  PlayfieldPresentationConfig configuration;
  audio::PlaybackRate playback;
  skin::UiLogicalRect safeUiBounds;
  GameplaySkinSessionServices skinServices;
  std::unique_ptr<ReplayPlayfieldPresentation> &presentation;
};

[[nodiscard]] std::optional<ReplayVideoExportResult>
preflightCourseReplayGameplayPresentations(
    std::vector<CourseReplayGameplayPreflightStage> &, IGameplayBgaSubmitter &,
    const AppSettings &);

} // namespace replay_video_export
