#pragma once

#include "PracticeAnalytics.h"
#include "PracticeConfiguration.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace practice {

struct ResultCompatibilityGroup {
  std::string label;
  std::vector<std::size_t> attemptIndices;
  TimingConditions conditions;
  bool containsAuto = false;
  bool containsPlayer = false;
};

class ResultModel {
public:
  ResultModel(const bms_parser::Chart &chart,
              std::span<const ReplayData> completedAttempts,
              std::size_t abandonedAttempts);

  void selectAttempt(std::optional<std::size_t> attemptIndex);
  void selectAggregateGroup(std::size_t groupIndex);
  void selectSection(std::size_t firstSection, std::size_t lastSection);

  [[nodiscard]] const Analysis &displayedAnalysis() const;
  [[nodiscard]] std::optional<RangeSelection> selectedRange() const;
  [[nodiscard]] std::optional<std::size_t> selectedAttempt() const;
  [[nodiscard]] std::size_t selectedAggregateGroup() const;
  [[nodiscard]] std::size_t completedAttempts() const;
  [[nodiscard]] std::size_t abandonedAttempts() const;
  [[nodiscard]] const std::vector<ResultCompatibilityGroup> &
  compatibilityGroups() const;
  [[nodiscard]] std::string attemptLabel(std::size_t attemptIndex) const;
  [[nodiscard]] bool displayedIsAuto() const;
  [[nodiscard]] bool displayedContainsAuto() const;

private:
  std::vector<Analysis> attemptAnalyses;
  std::vector<AnalysisGroup> analysisGroups;
  std::vector<ResultCompatibilityGroup> resultGroups;
  std::vector<bool> autoAttempts;
  std::size_t abandonedCount = 0;
  std::optional<std::size_t> selectedAttemptIndex;
  std::size_t selectedGroupIndex = 0;
  std::optional<RangeSelection> selection;
};

} // namespace practice
