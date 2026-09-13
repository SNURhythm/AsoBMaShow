#pragma once

#include "../ResultRecordSummary.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ChartMetaRecord;

struct ResultRecordsLoadOptions {
  std::optional<CourseReplayLookup> course;
  std::optional<ReplaySummary> autoPlay;
  std::optional<std::string> irServerOrigin;
  bool irEnabled = false;
  std::function<ir::IrRecordActivity(std::string_view)> attemptActivity;
};

struct ResultRecordsDiagnostic {
  enum class Source { ModernIr, ModernHistory, RemoteHistory, RemoteProjection };

  Source source;
  std::string message;
};

struct ResultRecordsLoadOutcome {
  std::vector<ResultRecordSummary> records;
  // Already sanitized, in publication order. A caller can retain its last
  // published message to suppress duplicates and clear it on a complete read.
  std::vector<ResultRecordsDiagnostic> diagnostics;
  bool complete = true;
};

[[nodiscard]] ResultRecordsLoadOutcome
loadResultRecords(ReplayRepository &repository, const ChartMetaRecord &record,
                  const ResultRecordsLoadOptions &options);
