#include "MusicSelectReplaySlots.h"

#include "../CourseConstraintUtils.h"
#include "../replay/BeatorajaLongNoteMode.h"

#include <ranges>

std::optional<std::array<replay::ReplayPathIdentity, 4>>
musicSelectChartReplaySlotPaths(const ChartMetaRecord &record,
                                int selectedLongNoteMode) {
  std::string diagnostic;
  const auto stem = replay::chartStem(
      record.meta.SHA256, selectedLongNoteMode,
      replay::hasUndefinedLongNotesForReplay(
          record.meta.LnMode, record.meta.TotalLongNotes,
          record.meta.TotalBackSpinNotes),
      diagnostic);
  if (!stem) return std::nullopt;

  std::array<replay::ReplayPathIdentity, 4> result;
  for (std::size_t index = 0; index < result.size(); ++index) {
    auto path = replay::pathForStem(*stem, static_cast<std::int64_t>(index),
                                    diagnostic);
    if (!path) return std::nullopt;
    result[index] = std::move(*path);
  }
  return result;
}

std::array<bool, 4> musicSelectExistingChartReplaySlots(
    const ChartMetaRecord &record, int selectedLongNoteMode,
    const std::filesystem::path &profileRoot) {
  std::array<bool, 4> result{};
  const auto paths =
      musicSelectChartReplaySlotPaths(record, selectedLongNoteMode);
  if (!paths) return result;
  for (std::size_t index = 0; index < result.size(); ++index) {
    std::error_code error;
    result[index] =
        std::filesystem::exists(profileRoot / (*paths)[index].relativePath,
                                error);
  }
  return result;
}

std::optional<int> musicSelectChartReplayResultId(
    std::span<const ModernReplayFileInventoryEntry> entries,
    const replay::ReplayPathIdentity &slot) {
  const auto found = std::ranges::find_if(entries, [&](const auto &entry) {
    return entry.owner == ModernReplayOwnerKind::ChartResult &&
           entry.reference.identity == slot;
  });
  if (found == entries.end()) return std::nullopt;
  return found->reference.resultId;
}

std::optional<std::array<replay::ReplayPathIdentity, 4>>
musicSelectCourseReplaySlotPaths(const MusicSelectBar &course,
                                 int selectedLongNoteMode) {
  replay::CoursePathInput input{
      .longNoteMode = selectedLongNoteMode,
      .beatorajaConstraintIds =
          beatorajaCourseConstraintIdsFromJson(course.courseConstraintJson),
  };
  input.stageSha256.reserve(course.courseCharts.size());
  for (const auto &record : course.courseCharts) {
    input.stageSha256.push_back(record.meta.SHA256);
    input.hasUndefinedLongNotes |= replay::hasUndefinedLongNotesForReplay(
        record.meta.LnMode, record.meta.TotalLongNotes,
        record.meta.TotalBackSpinNotes);
  }

  std::string diagnostic;
  const auto stem = replay::courseStem(input, diagnostic);
  if (!stem) return std::nullopt;
  std::array<replay::ReplayPathIdentity, 4> result;
  for (std::size_t index = 0; index < result.size(); ++index) {
    auto path = replay::pathForStem(*stem, static_cast<std::int64_t>(index),
                                    diagnostic);
    if (!path) return std::nullopt;
    result[index] = std::move(*path);
  }
  return result;
}

std::array<bool, 4> musicSelectExistingCourseReplaySlots(
    const MusicSelectBar &course, int selectedLongNoteMode,
    const std::filesystem::path &profileRoot) {
  std::array<bool, 4> result{};
  const auto paths =
      musicSelectCourseReplaySlotPaths(course, selectedLongNoteMode);
  if (!paths) return result;
  for (std::size_t index = 0; index < result.size(); ++index) {
    std::error_code error;
    result[index] =
        std::filesystem::exists(profileRoot / (*paths)[index].relativePath,
                                error);
  }
  return result;
}

std::optional<int> musicSelectCourseReplayResultId(
    std::span<const ModernReplayFileInventoryEntry> entries,
    const replay::ReplayPathIdentity &slot) {
  const auto found = std::ranges::find_if(entries, [&](const auto &entry) {
    return entry.owner == ModernReplayOwnerKind::CourseResult &&
           entry.reference.identity == slot;
  });
  if (found == entries.end()) return std::nullopt;
  return found->reference.resultId;
}
