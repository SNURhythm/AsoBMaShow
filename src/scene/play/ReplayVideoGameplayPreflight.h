#pragma once

#include "ReplayPlayfieldPresentation.h"

#include "../../ReplayVideoExporter.h"

#include <memory>
#include <optional>
#include <string>

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

} // namespace replay_video_export
