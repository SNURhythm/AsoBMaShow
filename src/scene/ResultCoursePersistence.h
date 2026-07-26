#pragma once

#include "../CoursePlaySession.h"
#include "../ResultPresentationUtils.h"
#include "../ResultPersistenceCoordinator.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace result_scene_detail {

[[nodiscard]] inline bool
courseReplayActionAvailable(const CoursePlaySession &session) noexcept {
  return session.hasCourseReplayStage(0);
}

[[nodiscard]] inline std::vector<analysis::JudgedCourseEntryFacts>
effectiveCourseEntryFacts(const CoursePlaySession &session) {
  std::vector<analysis::JudgedCourseEntryFacts> result;
  result.reserve(session.entries.size());
  for (const auto &entry : session.entries) {
    result.push_back(
        {.totalNotes = std::max(0, entry.meta.TotalNotes),
         .playLengthMicros = std::max<std::int64_t>(0, entry.meta.PlayLength)});
  }
  const std::size_t playedCount =
      std::min(result.size(), session.completedResults.size());
  for (std::size_t index = 0; index < playedCount; ++index) {
    const auto &meta = session.completedResults[index].meta;
    result[index] = {
        .totalNotes = std::max(0, meta.TotalNotes),
        .playLengthMicros = std::max<std::int64_t>(0, meta.PlayLength),
    };
  }
  return result;
}

[[nodiscard]] inline std::vector<result_persistence::PersistedCourseEntryFacts>
courseEntryFactsForPersistence(const CoursePlaySession &session) {
  const auto effective = effectiveCourseEntryFacts(session);
  std::vector<result_persistence::PersistedCourseEntryFacts> result;
  result.reserve(effective.size());
  for (const auto &facts : effective) {
    result.push_back({.totalNotes = facts.totalNotes,
                      .playLengthMicros = facts.playLengthMicros});
  }
  return result;
}

[[nodiscard]] inline bms_parser::ChartMeta
courseResultMetaForSession(const CoursePlaySession &session) {
  return result_presentation::courseResultMetaFromEntryFacts(
      session.courseName, session.courseGroupName,
      effectiveCourseEntryFacts(session));
}

[[nodiscard]] inline std::vector<CoursePlayEntry>
legacyReplayEntriesForSession(const CoursePlaySession &source,
                              const JudgedCoursePlaybackData &replay) {
  std::vector<CoursePlayEntry> result = source.entries;
  const std::size_t expected = std::max(
      result.size(), static_cast<std::size_t>(std::max(0, replay.totalCharts)));
  result.resize(std::max(expected, replay.stages.size()));
  for (std::size_t index = 0; index < replay.stages.size(); ++index) {
    result[index].meta = replay.stages[index].replay.chartMeta;
  }
  return result;
}

[[nodiscard]] inline bool applyCoursePersistenceReceipt(
    const std::shared_ptr<const result_persistence::CompletedCourseAttempt>
        &attempt,
    const result_persistence::SaveOutcome &outcome,
    CoursePlaySession &session) noexcept {
  if (attempt == nullptr || !outcome.saved() || !outcome.receipt.has_value() ||
      !attempt->result.attemptId.has_value() ||
      outcome.receipt->attemptId != *attempt->result.attemptId ||
      outcome.receipt->resultId <= 0 || outcome.receipt->createdAt.empty()) {
    return false;
  }
  session.savedCourseReplayId = outcome.receipt->resultId;
  session.courseReplaySaved = true;
  session.courseReplayPlaybackData =
      std::make_shared<replay::CourseReplayPlaybackData>(attempt->replay);
  return true;
}

} // namespace result_scene_detail
