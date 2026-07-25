#include "PracticeResultModel.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace practice {
namespace {

std::string groupLabel(std::size_t index, const AnalysisGroup &group,
                       std::span<const JudgedPlaybackData> attempts) {
  const char *mode =
      group.conditions.playback.mode == audio::PlaybackMode::TimeStretch
          ? "Time Stretch"
          : "Pitch Shift";
  std::size_t autoCount = 0;
  for (const std::size_t attemptIndex : group.attemptIndices) {
    if (attemptIndex < attempts.size() && attempts[attemptIndex].autoPlay) {
      ++autoCount;
    }
  }

  std::ostringstream label;
  label << "Group " << index + 1 << " · " << group.conditions.playback.percent
        << "% " << mode << " · Judge "
        << group.conditions.judgeWindowScalePercent << "% · "
        << group.attemptIndices.size() << " loop"
        << (group.attemptIndices.size() == 1 ? "" : "s");
  if (autoCount == group.attemptIndices.size() && autoCount > 0) {
    label << " · Auto";
  } else if (autoCount > 0) {
    label << " · includes Auto";
  }
  if (group.conditions.windowResolution == TimingWindowResolution::Unresolved) {
    label << " · windows unresolved";
  } else if (group.conditions.windowResolution ==
             TimingWindowResolution::LegacyAbsent) {
    label << " · legacy windows";
  }
  return label.str();
}

} // namespace

ResultModel::ResultModel(const bms_parser::Chart &chart,
                         std::span<const JudgedPlaybackData> completedAttempts,
                         std::size_t abandonedAttempts)
    : abandonedCount(abandonedAttempts) {
  attemptAnalyses.reserve(completedAttempts.size());
  autoAttempts.reserve(completedAttempts.size());
  for (const auto &attempt : completedAttempts) {
    attemptAnalyses.push_back(analyze(chart, attempt));
    autoAttempts.push_back(attempt.autoPlay);
  }

  analysisGroups = analyzeCompatibleAttempts(chart, completedAttempts);
  resultGroups.reserve(analysisGroups.size());
  for (std::size_t groupIndex = 0; groupIndex < analysisGroups.size();
       ++groupIndex) {
    const auto &group = analysisGroups[groupIndex];
    bool containsAuto = false;
    bool containsPlayer = false;
    for (const std::size_t attemptIndex : group.attemptIndices) {
      if (attemptIndex < autoAttempts.size() && autoAttempts[attemptIndex]) {
        containsAuto = true;
      } else {
        containsPlayer = true;
      }
    }
    resultGroups.push_back({
        .label = groupLabel(groupIndex, group, completedAttempts),
        .attemptIndices = group.attemptIndices,
        .conditions = group.conditions,
        .containsAuto = containsAuto,
        .containsPlayer = containsPlayer,
    });
  }
}

void ResultModel::selectAttempt(std::optional<std::size_t> attemptIndex) {
  selection.reset();
  if (!attemptIndex.has_value() || *attemptIndex >= attemptAnalyses.size()) {
    selectedAttemptIndex.reset();
    return;
  }
  selectedAttemptIndex = attemptIndex;
}

void ResultModel::selectAggregateGroup(std::size_t groupIndex) {
  selection.reset();
  selectedAttemptIndex.reset();
  if (groupIndex < analysisGroups.size()) {
    selectedGroupIndex = groupIndex;
  }
}

void ResultModel::selectSection(std::size_t firstSection,
                                std::size_t lastSection) {
  const auto &sections = displayedAnalysis().sections;
  if (firstSection >= sections.size() || lastSection >= sections.size()) {
    selection.reset();
    return;
  }
  const auto [first, last] = std::minmax(firstSection, lastSection);
  selection = RangeSelection{
      .startMicros = sections[first].startMicros,
      .endMicros = sections[last].endMicros,
      .active = Marker::Start,
  };
}

const Analysis &ResultModel::displayedAnalysis() const {
  static const Analysis empty;
  if (selectedAttemptIndex.has_value() &&
      *selectedAttemptIndex < attemptAnalyses.size()) {
    return attemptAnalyses[*selectedAttemptIndex];
  }
  if (selectedGroupIndex < analysisGroups.size()) {
    return analysisGroups[selectedGroupIndex].aggregate;
  }
  return empty;
}

std::optional<RangeSelection> ResultModel::selectedRange() const {
  return selection;
}

std::optional<std::size_t> ResultModel::selectedAttempt() const {
  return selectedAttemptIndex;
}

std::size_t ResultModel::selectedAggregateGroup() const {
  return selectedGroupIndex;
}

std::size_t ResultModel::completedAttempts() const {
  return attemptAnalyses.size();
}

std::size_t ResultModel::abandonedAttempts() const { return abandonedCount; }

const std::vector<ResultCompatibilityGroup> &
ResultModel::compatibilityGroups() const {
  return resultGroups;
}

std::string ResultModel::attemptLabel(std::size_t attemptIndex) const {
  if (attemptIndex >= attemptAnalyses.size()) {
    return {};
  }
  std::ostringstream label;
  label << "Loop " << attemptIndex + 1;
  const auto group = std::find_if(
      resultGroups.begin(), resultGroups.end(), [&](const auto &candidate) {
        return std::find(candidate.attemptIndices.begin(),
                         candidate.attemptIndices.end(),
                         attemptIndex) != candidate.attemptIndices.end();
      });
  if (group != resultGroups.end()) {
    label << " · Group "
          << static_cast<std::size_t>(
                 std::distance(resultGroups.begin(), group)) +
                 1;
  }
  if (autoAttempts[attemptIndex]) {
    label << " · Auto";
  }
  return label.str();
}

bool ResultModel::displayedIsAuto() const {
  return selectedAttemptIndex.has_value() &&
         *selectedAttemptIndex < autoAttempts.size() &&
         autoAttempts[*selectedAttemptIndex];
}

bool ResultModel::displayedContainsAuto() const {
  if (displayedIsAuto()) {
    return true;
  }
  return !selectedAttemptIndex.has_value() &&
         selectedGroupIndex < resultGroups.size() &&
         resultGroups[selectedGroupIndex].containsAuto;
}

} // namespace practice
