#include "ReplayFileActionSelection.h"

namespace replay {

ReplayFileActionSelection replayFileActionSelection(
    const ResultRecordSummary &summary, bool interactive) noexcept {
  ReplayFileActionSelection selection;
  const auto shareTarget = resultRecordActionTarget(
      summary, ResultRecordAction::ShareOrCopy);
  const auto deleteTarget = resultRecordActionTarget(
      summary, ResultRecordAction::DeleteReplayFile);
  const auto target = shareTarget != ResultRecordActionTarget::None
                          ? shareTarget
                          : deleteTarget;
  if (target == ResultRecordActionTarget::ModernChart &&
      summary.modern.has_value()) {
    selection.request = ReplayFileActionRequest{
        .owner = ModernReplayOwnerKind::ChartResult,
        .attemptId = summary.modern->result.attemptId};
  } else if (target == ResultRecordActionTarget::ModernCourse &&
             summary.modernCourse.has_value()) {
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
  selection.shareVisible = shareTarget != ResultRecordActionTarget::None;
  selection.deleteVisible = deleteTarget != ResultRecordActionTarget::None;
  selection.enabled = interactive &&
                      (selection.shareVisible || selection.deleteVisible);
  return selection;
}

} // namespace replay
