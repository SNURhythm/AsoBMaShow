#include "CourseIdentity.h"

#include "BmsMetadataText.h"
#include "CourseConstraintUtils.h"
#include "FileChecksum.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <sstream>
#include <utility>

namespace course_identity {
namespace {

using asobmshow::bms_metadata::normalizedHash;

struct NormalizedChartIdentity {
  std::string sha256;
  std::string md5;
};

bool isHexDigest(std::string_view value, std::size_t expectedLength) {
  return value.size() == expectedLength &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isxdigit(character) != 0;
         });
}

std::optional<NormalizedChartIdentity>
normalizeChartIdentity(const ChartIdentity &chart) {
  NormalizedChartIdentity normalized{
      .sha256 = normalizedHash(chart.sha256),
      .md5 = normalizedHash(chart.md5),
  };
  if ((!chart.sha256.empty() && !isHexDigest(normalized.sha256, 64)) ||
      (!chart.md5.empty() && !isHexDigest(normalized.md5, 32)) ||
      (normalized.sha256.empty() && normalized.md5.empty())) {
    return std::nullopt;
  }
  return normalized;
}

bool validConstraintJson(std::string_view constraintJson) {
  if (constraintJson.empty()) {
    return true;
  }
  return !nlohmann::json::parse(constraintJson, nullptr, false).is_discarded();
}

std::string serializeVersionedPayload(std::span<const ChartIdentity> charts,
                                      std::string_view canonicalConstraints) {
  if (charts.empty()) {
    return {};
  }

  std::string payload = "course-definition:v1\nconstraints:";
  payload += std::to_string(canonicalConstraints.size());
  payload.push_back(':');
  payload.append(canonicalConstraints);
  payload += "\ncharts:" + std::to_string(charts.size()) + "\n";
  for (const auto &chart : charts) {
    const auto normalized = normalizeChartIdentity(chart);
    if (!normalized) {
      return {};
    }
    if (!normalized->sha256.empty()) {
      payload += "sha256:" + normalized->sha256 + "\n";
    } else {
      payload += "md5:" + normalized->md5 + "\n";
    }
  }
  return payload;
}

bool startsWith(std::string_view value, std::string_view prefix) {
  return value.starts_with(prefix);
}

std::string canonicalConstraintName(std::string name) {
  name = normalizeCourseConstraintName(std::move(name));
  std::string canonical;
  canonical.reserve(name.size());
  for (const unsigned char character : name) {
    if (std::isspace(character) != 0) {
      if (!canonical.empty() && canonical.back() != '_') {
        canonical.push_back('_');
      }
    } else {
      canonical.push_back(static_cast<char>(character));
    }
  }
  if (!canonical.empty() && canonical.back() == '_') {
    canonical.pop_back();
  }
  return canonical;
}

} // namespace

std::string canonicalConstraintPayload(std::string_view constraintJson) {
  if (constraintJson.empty()) {
    return "[]";
  }
  const nlohmann::json parsed =
      nlohmann::json::parse(constraintJson, nullptr, false);
  if (parsed.is_discarded()) {
    return {};
  }

  std::vector<std::string> names;
  collectCourseConstraintNames(parsed, names);
  std::array<std::optional<std::string>, kCourseConstraintTypeCount> byType;
  for (auto &name : names) {
    name = canonicalConstraintName(std::move(name));
    const CourseConstraintType type = courseConstraintType(name);
    if (type == CourseConstraintType::Unknown ||
        type == CourseConstraintType::Grade) {
      continue;
    }
    auto &selected = byType[static_cast<std::size_t>(type)];
    if (!selected) {
      selected = name;
    }
  }

  nlohmann::json canonical = nlohmann::json::array();
  constexpr std::array kIdentityTypes = {
      CourseConstraintType::NoSpeed,
      CourseConstraintType::Judgement,
      CourseConstraintType::Gauge,
      CourseConstraintType::LongNote,
  };
  for (const CourseConstraintType type : kIdentityTypes) {
    const auto &selected = byType[static_cast<std::size_t>(type)];
    if (selected) {
      canonical.push_back(*selected);
    }
  }
  return canonical.dump();
}

std::string makeCourseKey(std::span<const ChartIdentity> charts,
                          std::string_view constraintJson) {
  const std::string canonicalConstraints =
      canonicalConstraintPayload(constraintJson);
  if (canonicalConstraints.empty()) {
    return {};
  }
  const std::string payload =
      serializeVersionedPayload(charts, canonicalConstraints);
  return payload.empty() ? std::string()
                         : "course:v1:" + file_checksum::sha256(payload);
}

std::string makeCourseKey(const CoursePlaySession &session) {
  std::vector<ChartIdentity> charts;
  charts.reserve(session.entries.size());
  for (const auto &entry : session.entries) {
    charts.push_back({.sha256 = entry.meta.SHA256, .md5 = entry.meta.MD5});
  }
  return makeCourseKey(charts, session.constraintJson);
}

std::optional<ParsedLegacyKey> parseLegacyScoreKey(std::string_view legacyKey) {
  std::istringstream input{std::string(legacyKey)};
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
  }
  while (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }
  constexpr std::string_view kCoursePrefix = "course:";
  constexpr std::string_view kConstraintPrefix = "constraint:";
  if (lines.size() < 3 || !startsWith(lines[0], kCoursePrefix) ||
      !startsWith(lines[1], kConstraintPrefix)) {
    return std::nullopt;
  }

  ParsedLegacyKey parsed;
  parsed.courseName = lines[0].substr(kCoursePrefix.size());
  parsed.constraintJson = lines[1].substr(kConstraintPrefix.size());
  if (!validConstraintJson(parsed.constraintJson)) {
    return std::nullopt;
  }

  constexpr std::string_view kSha256Prefix = "sha256:";
  constexpr std::string_view kMd5Prefix = "md5:";
  parsed.charts.reserve(lines.size() - 2);
  for (std::size_t index = 2; index < lines.size(); ++index) {
    const std::string_view chartLine = lines[index];
    if (startsWith(chartLine, kSha256Prefix)) {
      parsed.charts.push_back(
          {.sha256 = std::string(chartLine.substr(kSha256Prefix.size()))});
    } else if (startsWith(chartLine, kMd5Prefix)) {
      parsed.charts.push_back(
          {.md5 = std::string(chartLine.substr(kMd5Prefix.size()))});
    } else {
      return std::nullopt;
    }
    if (!normalizeChartIdentity(parsed.charts.back())) {
      return std::nullopt;
    }
  }

  parsed.courseKey = makeCourseKey(parsed.charts, parsed.constraintJson);
  if (parsed.courseKey.empty()) {
    return std::nullopt;
  }
  return parsed;
}

bool sameChart(const ChartIdentity &left, const ChartIdentity &right) {
  const auto normalizedLeft = normalizeChartIdentity(left);
  const auto normalizedRight = normalizeChartIdentity(right);
  if (!normalizedLeft || !normalizedRight) {
    return false;
  }
  if (!normalizedLeft->sha256.empty() && !normalizedRight->sha256.empty()) {
    return normalizedLeft->sha256 == normalizedRight->sha256;
  }
  return !normalizedLeft->md5.empty() && !normalizedRight->md5.empty() &&
         normalizedLeft->md5 == normalizedRight->md5;
}

bool sameDefinition(const Definition &left, const Definition &right) {
  const std::string leftConstraints =
      canonicalConstraintPayload(left.constraintJson);
  const std::string rightConstraints =
      canonicalConstraintPayload(right.constraintJson);
  if (left.charts.empty() || leftConstraints.empty() ||
      rightConstraints.empty() || leftConstraints != rightConstraints ||
      left.charts.size() != right.charts.size()) {
    return false;
  }
  return std::ranges::equal(left.charts, right.charts, sameChart);
}

bool prefixMatches(std::span<const ChartIdentity> prefix,
                   std::span<const ChartIdentity> completeSequence) {
  if (prefix.empty() || prefix.size() > completeSequence.size()) {
    return false;
  }
  return std::ranges::equal(prefix, completeSequence.first(prefix.size()),
                            sameChart);
}

} // namespace course_identity
