#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct CoursePlaySession;

namespace course_identity {

[[nodiscard]] bool isCanonicalKey(std::string_view key) noexcept;

struct ChartIdentity {
  std::string sha256;
  std::string md5;
};

struct Definition {
  int courseId = 0;
  std::string courseKey;
  std::string name;
  std::string groupName;
  std::string constraintJson;
  std::vector<ChartIdentity> charts;
};

struct ParsedLegacyKey {
  std::string courseName;
  std::string constraintJson;
  std::vector<ChartIdentity> charts;
  std::string courseKey;
};

[[nodiscard]] std::string
canonicalConstraintPayload(std::string_view constraintJson);
[[nodiscard]] std::string makeCourseKey(std::span<const ChartIdentity> charts,
                                        std::string_view constraintJson);
[[nodiscard]] std::string makeCourseKey(const CoursePlaySession &session);
[[nodiscard]] std::optional<ParsedLegacyKey>
parseLegacyScoreKey(std::string_view legacyKey);

[[nodiscard]] bool sameChart(const ChartIdentity &left,
                             const ChartIdentity &right);
[[nodiscard]] bool sameDefinition(const Definition &left,
                                  const Definition &right);
[[nodiscard]] bool
prefixMatches(std::span<const ChartIdentity> prefix,
              std::span<const ChartIdentity> completeSequence);

} // namespace course_identity
