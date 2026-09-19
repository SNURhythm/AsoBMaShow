#include "ResultRecordsLoader.h"

#include "../ir/IrProfileSettings.h"
#include "../repositories/ChartRepository.h"
#include "../replay/ReplayFileActionService.h"

#include <span>
#include <unordered_map>
#include <utility>

ResultRecordsLoadOutcome
loadResultRecords(ReplayRepository &repository, const ChartMetaRecord &record,
                  const ResultRecordsLoadOptions &options) {
  ResultRecordsLoadOutcome outcome;
  using Source = ResultRecordsDiagnostic::Source;
  const auto report = [&](Source source, std::string_view diagnostic) {
    outcome.complete = false;
    outcome.diagnostics.push_back(
        {.source = source, .message = ir::sanitizeDiagnostic(diagnostic)});
  };
  const bool courseRecords = record.courseStart && options.course &&
      (!options.course->courseKey.empty() || options.course->legacyCourseId > 0);
  std::vector<ReplaySummary> synthetic;
  std::vector<ResultRecordSummary> projected;
  if (courseRecords) {
    const auto legacy = repository.ListLegacyCourseSummaries(
        *options.course, kMaximumLegacyResultSummaryRows);
    projected.reserve(legacy.size());
    for (const auto &summary : legacy) {
      projected.push_back(makeLegacyCourseResultRecord(summary));
    }
  } else {
    if (options.autoPlay) {
      synthetic.push_back(*options.autoPlay);
    }
    const auto legacy = repository.ListLegacyChartSummaries(
        record.meta, kMaximumLegacyResultSummaryRows);
    projected.reserve(legacy.size());
    for (const auto &summary : legacy) {
      projected.push_back(makeLegacyChartResultRecord(summary));
    }
  }

  std::unordered_map<std::string, ir::IrUploadRecord> irRecordsByAttempt;
  if (!courseRecords && options.irServerOrigin && !record.meta.SHA256.empty()) {
    auto loaded = repository.ListIrUploadRecordsForChart(
        ir::kTachiProviderId, *options.irServerOrigin, record.meta.SHA256,
        kMaximumModernChartHistoryRows);
    if (loaded.status != ir::IrUploadRecordReadStatus::Loaded) {
      report(Source::ModernIr,
             loaded.diagnostic.empty()
                 ? "Modern IR Records unavailable: state could not be read"
                 : std::string("Modern IR Records unavailable: ") +
                       loaded.diagnostic);
    } else {
      irRecordsByAttempt.reserve(loaded.records.size());
      for (auto &stored : loaded.records) {
        irRecordsByAttempt.emplace(stored.attemptId, std::move(stored));
      }
      if (!loaded.diagnostic.empty()) {
        report(Source::ModernIr, loaded.diagnostic);
      }
    }
  } else if (!courseRecords && !options.irServerOrigin) {
    report(Source::ModernIr,
           "Modern IR Records unavailable: provider origin is invalid");
  }

  if (courseRecords && !options.course->courseKey.empty()) {
    const auto history = repository.ListModernCourseResults(
        options.course->courseKey, kMaximumModernCourseHistoryRows);
    if (history.status == ModernCourseHistoryReadStatus::Loaded) {
      projected.reserve(projected.size() + history.records.size());
      replay::ReplayFileActionService actions(repository);
      for (const auto &modern : history.records) {
        const auto inspected = actions.probe(modern.replayFile);
        projected.push_back(makeModernCourseResultRecord(
            modern, replay::replayStateForFileAction(inspected.state)));
      }
    } else {
      report(Source::ModernHistory,
             history.diagnostic.empty()
                 ? "Modern Course Records unavailable: history could not be read"
                 : std::string("Modern Course Records unavailable: ") +
                       history.diagnostic);
    }
  } else if (!courseRecords && !record.meta.SHA256.empty()) {
    const auto history = repository.ListModernChartResults(
        record.meta.SHA256, kMaximumModernChartHistoryRows);
    if (history.status == ModernChartHistoryReadStatus::Loaded) {
      projected.reserve(projected.size() + history.records.size());
      replay::ReplayFileActionService actions(repository);
      for (const auto &modern : history.records) {
        const auto inspected = actions.probe(modern.replayFile);
        ir::IrRecordState irState = ir::IrRecordState::Hidden;
        std::optional<IrRemoteRecordId> linkedRemote;
        const auto stored = irRecordsByAttempt.find(modern.result.attemptId);
        if (stored != irRecordsByAttempt.end()) {
          const auto activity = options.attemptActivity
              ? options.attemptActivity(modern.result.attemptId)
              : ir::IrRecordActivity::None;
          irState = stored->second.resolvedState(activity);
          if (stored->second.receiptRemoteScoreId && options.irServerOrigin) {
            linkedRemote = IrRemoteRecordId{
                .providerId = std::string(ir::kTachiProviderId),
                .serverOrigin = *options.irServerOrigin,
                .remoteScoreId = *stored->second.receiptRemoteScoreId,
            };
          }
        }
        projected.push_back(makeModernChartResultRecord(
            modern, replay::replayStateForFileAction(inspected.state), irState,
            std::move(linkedRemote)));
      }
    } else {
      report(Source::ModernHistory,
             history.diagnostic.empty()
                 ? "Modern Records unavailable: history could not be read"
                 : std::string("Modern Records unavailable: ") +
                       history.diagnostic);
    }
  }

  std::vector<ir::IrRemoteScore> remoteScores;
  std::string mergeOrigin;
  if (!courseRecords && options.irEnabled) {
    if (options.irServerOrigin) {
      mergeOrigin = *options.irServerOrigin;
      auto loaded = repository.ListIrRemoteScoresForChart(
          ir::kTachiProviderId, mergeOrigin, record.meta.MD5, record.meta.SHA256);
      if (loaded.status == ir::IrRemoteScoreReadOutcome::Status::Loaded) {
        remoteScores = std::move(loaded.scores);
      } else {
        report(Source::RemoteHistory,
               std::string("IR Records unavailable: ") +
                   (loaded.diagnostic.empty()
                        ? "remote score history could not be read"
                        : loaded.diagnostic));
      }
    } else {
      report(Source::RemoteHistory,
             "IR Records unavailable: provider origin is invalid");
    }
  }

  try {
    outcome.records = mergeResultRecords(synthetic, projected, remoteScores,
                                          ir::kTachiProviderId, mergeOrigin);
  } catch (...) {
    outcome.records = mergeResultRecords(
        synthetic, projected, std::span<const ir::IrRemoteScore>{},
        ir::kTachiProviderId, std::string_view{});
    report(Source::RemoteProjection,
           "IR Records unavailable: remote score projection is invalid");
  }
  return outcome;
}
