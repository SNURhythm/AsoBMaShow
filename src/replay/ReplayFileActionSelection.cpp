#include "ReplayFileActionSelection.h"

namespace replay {

ReplayFileActionSelection replayFileActionSelection(
    const ResultRecordSummary &summary, bool interactive) noexcept {
  ReplayFileActionSelection selection;
  if (summary.modern.has_value()) {
    selection.request = ReplayFileActionRequest{
        .owner = ModernReplayOwnerKind::ChartResult,
        .attemptId = summary.modern->result.attemptId};
  } else if (summary.modernCourse.has_value()) {
    selection.request = ReplayFileActionRequest{
        .owner = ModernReplayOwnerKind::CourseResult,
        .attemptId = summary.modernCourse->result.attemptId};
  } else {
    return selection;
  }
  if (selection.request->attemptId.empty()) {
    selection.request.reset();
    return selection;
  }
  selection.shareVisible = summary.capabilities.shareOrCopy;
  selection.deleteVisible = summary.capabilities.deleteReplayFile;
  selection.enabled = interactive &&
                      (selection.shareVisible || selection.deleteVisible);
  return selection;
}

} // namespace replay
