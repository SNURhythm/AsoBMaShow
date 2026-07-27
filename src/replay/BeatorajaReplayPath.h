#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace replay {

inline constexpr std::size_t kMaximumCourseReplayStages = 256;
inline constexpr std::size_t kMaximumReplayFilenameBytes = 255;
inline constexpr int kBeatorajaFirstConstraintId = 1;
inline constexpr int kBeatorajaLastConstraintId = 14;
inline constexpr int kBeatorajaLastExcludedCourseMarkerId = 3;

struct CoursePathInput {
  std::vector<std::string> stageSha256;
  int longNoteMode = 0;
  bool hasUndefinedLongNotes = false;
  std::vector<int> beatorajaConstraintIds;
};

struct ReplayPathIdentity {
  std::string stem;
  std::int64_t historyIndex = 0;
  std::filesystem::path relativePath;

  bool operator==(const ReplayPathIdentity &) const = default;
};

[[nodiscard]] std::optional<std::string> chartStem(std::string_view lowerSha256,
                                                   int longNoteMode,
                                                   bool hasUndefinedLongNotes,
                                                   std::string &diagnostic);

[[nodiscard]] bool chartStemMatches(
    std::string_view stem, std::string_view lowerSha256, int longNoteMode,
    std::optional<bool> hasUndefinedLongNotes, std::string &diagnostic);

[[nodiscard]] std::optional<std::string>
courseStem(const CoursePathInput &input, std::string &diagnostic);

[[nodiscard]] bool courseStemMatches(
    std::string_view stem, CoursePathInput input,
    std::optional<bool> hasUndefinedLongNotes, std::string &diagnostic);

[[nodiscard]] std::optional<ReplayPathIdentity>
pathForStem(std::string_view stem, std::int64_t historyIndex,
            std::string &diagnostic);

} // namespace replay
