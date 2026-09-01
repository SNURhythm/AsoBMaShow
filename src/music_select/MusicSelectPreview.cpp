#include "MusicSelectPreview.h"

#include <utility>

MusicSelectPreviewMoveResult MusicSelectPreviewController::selectedBarMoved(
    std::optional<MusicSelectPreviewSelection> selection,
    std::int64_t nowMicros) {
  const bool stopAudio =
      active_ && (!selection || active_->folder != selection->folder);
  if (stopAudio) active_.reset();
  pending_ = std::move(selection);
  dueMicros_ = pending_ ? nowMicros + kPreviewDelayMicros : -1;
  return {.stopAudio = stopAudio};
}

std::optional<MusicSelectPreviewSwitch>
MusicSelectPreviewController::update(std::int64_t nowMicros, bool launching) {
  if (!pending_ || dueMicros_ < 0 || nowMicros <= dueMicros_ || launching) {
    return std::nullopt;
  }
  dueMicros_ = -1;
  if (active_ && active_->id == pending_->id) {
    pending_.reset();
    return std::nullopt;
  }
  active_ = std::move(pending_);
  pending_.reset();
  return MusicSelectPreviewSwitch{
      .path = active_->previewPath.empty()
                  ? std::nullopt
                  : std::optional(active_->previewPath)};
}

void MusicSelectPreviewController::reset() {
  active_.reset();
  pending_.reset();
  dueMicros_ = -1;
}
