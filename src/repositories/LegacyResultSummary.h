#pragma once

#include "../ScoreProvenance.h"

#include <cstddef>
#include <optional>
#include <string>

inline constexpr std::size_t kMaximumLegacyResultSummaryRows = 4096;

struct LegacyChartResultSummary {
  int legacyReplayId = 0;
  std::optional<std::string> chartPath;
  std::optional<std::string> chartMd5;
  std::optional<std::string> chartSha256;
  std::optional<std::string> chartTitle;
  std::optional<std::string> chartArtist;
  std::optional<int> longNoteMode;
  std::optional<int> finalScore;
  std::optional<int> maxCombo;
  std::optional<double> finalGauge;
  std::optional<int> clearType;
  std::optional<std::string> createdAt;
  std::optional<int> rulesetVersion;
  std::optional<ScoreEligibility> eligibility;
  std::optional<std::string> provenanceJson;
  bool partial = true;

  bool operator==(const LegacyChartResultSummary &) const = default;
};

struct LegacyCourseResultSummary {
  int legacyCourseReplayId = 0;
  std::optional<int> legacyCourseId;
  std::optional<std::string> courseKey;
  std::optional<std::string> courseName;
  std::optional<std::string> courseGroupName;
  std::optional<std::string> constraintJson;
  std::optional<int> finalScore;
  std::optional<int> maxCombo;
  std::optional<double> finalGauge;
  std::optional<int> clearType;
  std::optional<int> completedCharts;
  std::optional<int> totalCharts;
  std::optional<std::string> createdAt;
  std::optional<int> rulesetVersion;
  std::optional<ScoreEligibility> eligibility;
  std::optional<std::string> provenanceJson;
  bool partial = true;

  bool operator==(const LegacyCourseResultSummary &) const = default;
};
