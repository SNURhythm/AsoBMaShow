#pragma once

#include "ReplayLimits.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace replay {

inline constexpr int kBeatorajaFirstConstraintId = 1;
inline constexpr int kBeatorajaLastConstraintId = 14;
inline constexpr int kBeatorajaLastExcludedCourseMarkerId = 3;

struct CoursePathInput {
  std::vector<std::string> stageSha256;
  int longNoteMode = 0;
  bool hasUndefinedLongNotes = false;
  std::vector<int> beatorajaConstraintIds;

  bool operator==(const CoursePathInput &) const = default;
};

struct ReplayPathIdentity {
  std::string stem;
  std::int64_t historyIndex = 0;
  std::filesystem::path relativePath;

  bool operator==(const ReplayPathIdentity &) const = default;
};

[[nodiscard]] std::optional<std::string>
chartStem(std::string_view lowerSha256, int longNoteMode,
          bool hasUndefinedLongNotes, std::string &diagnostic,
          const ReplayLimits &limits = kReplayLimits);

[[nodiscard]] bool chartStemMatches(std::string_view stem,
                                    std::string_view lowerSha256,
                                    int longNoteMode,
                                    std::optional<bool> hasUndefinedLongNotes,
                                    std::string &diagnostic,
                                    const ReplayLimits &limits = kReplayLimits);

[[nodiscard]] std::optional<std::string>
courseStem(const CoursePathInput &input, std::string &diagnostic,
           const ReplayLimits &limits = kReplayLimits);

[[nodiscard]] bool
courseStemMatches(std::string_view stem, CoursePathInput input,
                  std::optional<bool> hasUndefinedLongNotes,
                  std::string &diagnostic,
                  const ReplayLimits &limits = kReplayLimits);

[[nodiscard]] std::optional<ReplayPathIdentity>
pathForStem(std::string_view stem, std::int64_t historyIndex,
            std::string &diagnostic,
            const ReplayLimits &limits = kReplayLimits);

[[nodiscard]] bool
isCanonicalReplayRelativePath(const std::filesystem::path &relativePath,
                              std::string &diagnostic,
                              const ReplayLimits &limits = kReplayLimits);

} // namespace replay
