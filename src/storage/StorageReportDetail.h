#pragma once
#include "StorageTypes.h"
#include <algorithm>
#include <utility>

namespace storage_management {
inline void RetainStorageReportDetail(StorageReport &report, FileResult result) {
  const auto priority = [](const FileResult &file) {
    if (file.reason.code == StorageReason::RecoveryRequired) return 4;
    if (file.outcome == FileOutcome::Failed) return 3;
    if (file.reason.code == StorageReason::CleanupFailed) return 2;
    return file.reason.code != StorageReason::None ? 1 : 0;
  };
  // Counts remain independent of this bounded sample. A long review's routine
  // skips must never crowd out the later failure that prevented Apply.
  const auto rank = priority(result);
  if (report.files.size() == 100 && rank <= priority(report.files.back())) return;
  const auto position = std::find_if(report.files.begin(), report.files.end(),
      [&](const auto &file) { return priority(file) < rank; });
  report.files.insert(position, std::move(result));
  if (report.files.size() > 100) report.files.pop_back();
}
}
