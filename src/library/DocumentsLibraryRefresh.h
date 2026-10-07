#pragma once

#include "ChartLibraryTaskService.h"

namespace chart_library_tasks {

// Files edits share the library worker and its gameplay pause/lifecycle handling.
inline std::uint64_t enqueueDocumentsLibraryRefresh(
    ChartLibraryTaskService &tasks, const std::filesystem::path &bmsDirectory) {
  if (tasks.snapshot().activeCount != 0) return 0;
  TaskRequest request;
  request.kind = TaskKind::RefreshPath;
  request.title = i18n::message("library.tasks.refreshing_library");
  request.refreshPath = bmsDirectory;
  // An explicit Files deletion/rename still needs a healthy empty scan to
  // reconcile old chart entries; a missing root is a scanner I/O failure.
  std::filesystem::create_directories(bmsDirectory);
  return tasks.enqueue(std::move(request));
}

} // namespace chart_library_tasks
