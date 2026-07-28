#include "ReplayRepository.h"

#include <utility>

namespace {

constexpr const char *kLegacyReplayCutoverDiagnostic =
    "legacy replay detail is unavailable after the summary-only cutover";

} // namespace

ir::IrReconciliationReadOutcome
ReplayRepository::LoadIrReconciliationCandidates(
    std::string_view, std::string_view) {
  return {.status = ir::IrReconciliationReadOutcome::Status::Loaded,
          .candidates = {},
          .diagnostic = kLegacyReplayCutoverDiagnostic};
}

IrUploadReplayReadOutcome ReplayRepository::ListIrUploadCandidateReplays(
    std::string_view, std::string_view) {
  return {.status = IrUploadReplayReadStatus::Loaded,
          .replays = {},
          .omittedRows = 0,
          .diagnostic = kLegacyReplayCutoverDiagnostic};
}
