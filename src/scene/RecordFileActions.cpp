#include "RecordFileActions.h"

#include "../ir/IrOutboxModels.h"

#include <string_view>
#include <utility>

namespace {

RecordFileActions::Feedback failure(std::string_view diagnostic,
                                    std::string_view fallback,
                                    bool reloadRecords = false) {
  std::string message = ir::sanitizeDiagnostic(diagnostic);
  if (message.empty()) {
    message = fallback;
  }
  return {.message = std::move(message),
          .reloadRecords = reloadRecords,
          .failed = true};
}

} // namespace

RecordFileActions::RecordFileActions(ReplayRepository &repository,
                                     DocumentExporter exporter)
    : repository_(repository), exporter_(std::move(exporter)) {}

RecordFileActions::Feedback
RecordFileActions::share(const replay::ReplayFileActionRequest &request) {
  replay::ReplayFileActionService actions(repository_);
  auto prepared = actions.prepareShare(request);
  if (prepared.state != replay::ReplayFileActionState::Verified ||
      !prepared.share) {
    return failure(prepared.diagnostic, "Replay file is unavailable to share.",
                   true);
  }
  PlatformDocumentExportRequest exportRequest{
      .localPath = prepared.share->sourcePath,
      .mimeType = "application/gzip",
      .suggestedName = prepared.share->suggestedFilename,
      .maxBytes = replay::kReplayLimits.maxCompressedBytes,
      .sourceLifetime = std::move(prepared.share->sourceLifetime),
  };
  handoff_ = exporter_(std::move(exportRequest));
  if (!handoff_) {
    return failure({}, "Unable to open replay sharing.");
  }
  return {.message = "Choose where to share the BRD replay."};
}

RecordFileActions::Feedback
RecordFileActions::remove(const replay::ReplayFileActionRequest &request) {
  replay::ReplayFileActionService actions(repository_);
  const auto removed = actions.remove(request);
  if (removed.state == replay::ReplayFileActionState::UserDeleted) {
    return {.message = removed.cleanupPending
                ? "Replay hidden; file cleanup will retry at startup."
                : "Replay file deleted. Result history was kept.",
            .reloadRecords = true};
  }
  return failure(removed.diagnostic, "Replay file could not be deleted.");
}

std::optional<RecordFileActions::Feedback> RecordFileActions::poll() {
  if (!handoff_ || !handoff_.ready()) {
    return std::nullopt;
  }
  auto result = handoff_.takeResult();
  handoff_.close();
  if (!result) {
    return Feedback{};
  }
  if (result->ok()) {
    return Feedback{.message = "Replay BRD shared."};
  }
  if (result->cancelled()) {
    return Feedback{.message = "Replay sharing cancelled."};
  }
  return failure(result->message, "Replay sharing failed.");
}

bool RecordFileActions::active() const noexcept {
  return static_cast<bool>(handoff_);
}

void RecordFileActions::close() noexcept { handoff_.close(); }
