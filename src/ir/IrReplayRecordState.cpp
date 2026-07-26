#include "IrReplayRecordState.h"

#include "tachi/TachiBatchManual.h"

namespace ir {

void resolveReplayIrRecordState(ReplaySummary &summary,
                                IrRecordActivity activity) noexcept {
  summary.irSubmissionEligible =
      summary.hasIrSubmissionSnapshot && summary.attemptId.has_value() &&
      summary.chartMeta.has_value() &&
      summary.provenance != nullptr &&
      tachi::isReplayEligibleForBokutachi(
          *summary.attemptId, summary.hasCanonicalAttemptFingerprint,
          *summary.chartMeta, *summary.provenance);
  summary.irRecordState = resolveIrRecordState({
      .eligible = summary.irSubmissionEligible,
      .hasReceipt = summary.hasIrReceipt,
      .outboxState = summary.requestedIrOutboxState,
      .activity = activity,
  });
}

} // namespace ir
