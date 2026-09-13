#pragma once

#include "../repositories/ChartRepository.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct CoursePlaySession;
class ReplayRepository;
namespace result_persistence {
struct ModernCourseResult;
}

namespace course_records {

struct CurrentCourseSelection {
  std::vector<ChartMetaRecord> records;
  std::vector<std::filesystem::path> completedChartPaths;
  bool completeCourse = false;
};

[[nodiscard]] std::optional<CurrentCourseSelection> currentCourseSelectionFor(
    std::string_view currentCourseKey,
    const std::vector<ChartMetaRecord> &records,
    const result_persistence::ModernCourseResult &result);

struct PreparedCourseResult {
  std::shared_ptr<CoursePlaySession> session;
  // Cancellation returns no session and no diagnostic.
  std::string diagnostic;
};

// Reads the exact saved attempt and owns every chart/replay used by its result
// state. Callers retain responsibility for preview cleanup and scene changes.
[[nodiscard]] PreparedCourseResult prepareCourseResult(
    ReplayRepository &repository, std::string_view attemptId,
    const std::optional<CurrentCourseSelection> &currentSelection,
    bool retrySameAllowed, std::atomic_bool &cancelled);

} // namespace course_records
