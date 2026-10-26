#include "MusicSelectAssignmentFolders.h"

#include <set>

namespace {
constexpr std::string_view rootId = "container:assignments";
constexpr std::string_view assignmentPrefix = "container:assignment:";

MusicSelectBar folder(std::string id, std::string title) {
  return {.id={std::move(id)}, .kind=skin::MusicSelectBarKind::Container,
          .title=title,
          .presentation={.kind=skin::MusicSelectBarKind::Container,
                         .title=std::move(title), .exists=true},
          .selectable=true, .sortable=true, .childrenLoaded=false};
}
}

bool musicSelectIsAssignmentDirectory(const MusicSelectBar &bar) {
  return bar.kind == skin::MusicSelectBarKind::Container &&
      (bar.id.value == rootId || bar.id.value.starts_with(assignmentPrefix));
}
MusicSelectBar musicSelectAssignmentsRoot(std::string title) {
  return folder(std::string(rootId), std::move(title));
}
std::vector<MusicSelectBar> musicSelectAssignmentChildren(
    const streak::Day &day, const MusicSelectBar &directory,
    std::span<const MusicSelectAssignmentChart> charts,
    MusicSelectRepositoryProjectionInput input) {
  if (!musicSelectIsAssignmentDirectory(directory)) return {};
  const bool root = directory.id.value == rootId;
  std::vector<MusicSelectBar> result;
  std::vector<const streak::Progress *> assignments;
  for (const auto &progress : day.assignments) {
    const auto id = std::string(assignmentPrefix) + progress.requirement.id;
    if (root) result.push_back(folder(id, progress.requirement.name));
    if (root || directory.id.value == id) assignments.push_back(&progress);
  }
  if (day.points >= day.config.dailyGoal &&
      streak::bonusPoints(day) >= streak::bonusLimit(day)) return result;
  std::set<std::string> included;
  for (const auto &source : charts) {
    const auto &record = source.record;
    if (record.unavailable || record.solidArchive || record.meta.BmsPath.empty() ||
        record.meta.SHA256.empty()) continue;
    streak::Play play{.chart=record.meta.SHA256, .levels=source.levels,
                     .previousLamp=input.clearFor
                         ? input.clearFor(record.meta, input.selectedLongNoteMode) : -1};
    for (const auto *progress : assignments) {
      if (progress->usedCharts.contains(play.chart)) continue;
      if (!progress->chartProgress.contains(play.chart) &&
          !streak::eligible(progress->requirement, play)) continue;
      if (included.insert(play.chart).second)
        result.push_back(MusicSelectRepositoryProjection::projectSong(
            record, directory.id.value, input));
      break;
    }
  }
  return result;
}
