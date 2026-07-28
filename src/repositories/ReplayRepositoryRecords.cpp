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

std::vector<ReplaySummary>
ReplayRepository::ListReplays(const bms_parser::ChartMeta &, int,
                              std::string_view, std::string_view) {
  return {};
}

std::vector<ReplaySummary>
ReplayRepository::ListCourseReplays(const CourseReplayLookup &, int) {
  return {};
}

std::optional<ReplayData>
ReplayRepository::LoadReplay(int, const bms_parser::ChartMeta &) {
  return std::nullopt;
}

std::optional<ReplayResultRecord>
ReplayRepository::LoadReplayResult(int, const bms_parser::ChartMeta &) {
  return std::nullopt;
}

std::optional<CourseReplayData> ReplayRepository::LoadCourseReplay(int) {
  return std::nullopt;
}

bool ReplayRepository::RecoverCourseRecords(
    std::span<const course_identity::Definition>,
    std::span<const CourseScoreEvidence>, std::string &errorMessage) {
  errorMessage.clear();
  return true;
}

std::optional<ReplayData>
ReplayRepository::LoadLatestReplay(const bms_parser::ChartMeta &) {
  return std::nullopt;
}
