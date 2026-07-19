#include "IrUploadCandidates.h"

#include "IrOutboxModels.h"
#include "../repositories/ChartStorageIdentity.h"

#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace ir {
namespace {

bool hashesMatchWhenPresent(const std::string &stored,
                            const std::string &hydrated) noexcept {
  return stored.empty() || hydrated.empty() || stored == hydrated;
}

void setOmissionDiagnostic(IrUploadCandidateProjection &result) {
  if (result.omittedRows == 0) {
    return;
  }
  result.diagnostic = sanitizeDiagnostic(
      std::to_string(result.omittedRows) +
      " replay rows were omitted because they could not be safely prepared.");
}

} // namespace

IrUploadCandidateProjection projectIrUploadCandidates(
    std::span<const ReplaySummary> replays,
    std::span<const ChartMetaRecord> charts) noexcept {
  IrUploadCandidateProjection result;
  try {
    std::unordered_map<std::string, const ChartMetaRecord *> chartsByPath;
    std::unordered_set<std::string> ambiguousPaths;
    chartsByPath.reserve(charts.size());
    ambiguousPaths.reserve(charts.size());

    for (const ChartMetaRecord &chart : charts) {
      const std::string path =
          chart_storage_identity::StoredPathText(chart.meta.BmsPath);
      if (path.empty()) {
        continue;
      }
      const auto [it, inserted] = chartsByPath.emplace(path, &chart);
      if (!inserted) {
        ambiguousPaths.insert(path);
      }
    }

    result.candidates.reserve(replays.size());
    for (const ReplaySummary &replay : replays) {
      if (replay.id <= 0 || replay.courseReplay || replay.autoPlay ||
          !replay.chartMeta.has_value()) {
        ++result.omittedRows;
        continue;
      }

      const bms_parser::ChartMeta &stored = *replay.chartMeta;
      const std::string path =
          chart_storage_identity::StoredPathText(stored.BmsPath);
      if (path.empty() || ambiguousPaths.contains(path)) {
        ++result.omittedRows;
        continue;
      }

      const auto found = chartsByPath.find(path);
      if (found == chartsByPath.end()) {
        ++result.omittedRows;
        continue;
      }

      const ChartMetaRecord &chart = *found->second;
      if (!hashesMatchWhenPresent(stored.MD5, chart.meta.MD5) ||
          !hashesMatchWhenPresent(stored.SHA256, chart.meta.SHA256) ||
          chart.meta.TotalNotes <= 0 ||
          chart.meta.TotalNotes > std::numeric_limits<int>::max() / 2) {
        ++result.omittedRows;
        continue;
      }

      ReplaySummary hydrated = replay;
      hydrated.chartMeta = chart.meta;
      hydrated.maxScore = chart.meta.TotalNotes * 2;
      resolveReplayIrRecordState(hydrated);
      if (hydrated.irRecordState != IrRecordState::Eligible &&
          hydrated.irRecordState != IrRecordState::Failed) {
        continue;
      }
      const IrRecordState state = hydrated.irRecordState;
      result.candidates.push_back({.replay = std::move(hydrated),
                                   .chart = chart,
                                   .state = state});
    }
    setOmissionDiagnostic(result);
  } catch (...) {
    result.candidates.clear();
    result.omittedRows = replays.size();
    result.diagnostic =
        sanitizeDiagnostic("IR upload candidates are unavailable.");
  }
  return result;
}

void intersectIrUploadSelection(
    std::unordered_set<int> &selectedReplayIds,
    std::span<const IrUploadCandidate> candidates) {
  detail::intersectIrUploadSelectionIndexed(selectedReplayIds, candidates);
}

} // namespace ir
