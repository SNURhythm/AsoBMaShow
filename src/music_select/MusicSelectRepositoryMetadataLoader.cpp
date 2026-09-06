#include "MusicSelectRepositoryProjection.h"

#include "../path.h"

#include <algorithm>

MusicSelectRepositoryMetadata MusicSelectRepositoryProjection::loadMetadata(
    ChartRepository::Session &session, int) {
  MusicSelectRepositoryMetadata metadata;
  metadata.entries = session.SelectEffectiveEntries();
  metadata.folders = session.SelectFolderRecords();
  for (const auto &path : session.SelectChartMetaFolders()) {
    const auto exists = std::ranges::any_of(
        metadata.folders, [&](const ChartFolderRecord &folder) {
          return std::filesystem::path(folder.path).lexically_normal() ==
                 path.lexically_normal();
        });
    if (!exists) {
      metadata.folders.push_back({.path = fspath_to_path_t(path)});
    }
  }
  for (const auto &table : session.SelectDifficultyTables()) {
    metadata.tables.push_back({.info = table});
  }
  return metadata;
}

std::optional<MusicSelectDifficultyTableSource>
MusicSelectRepositoryProjection::loadTableMetadata(
    ChartRepository::Session &session, int tableId, int selectedLongNoteMode) {
  const auto tables = session.SelectDifficultyTables();
  const auto table = std::ranges::find(tables, tableId,
                                       &DifficultyTableInfo::id);
  if (table == tables.end()) return std::nullopt;

  MusicSelectDifficultyTableSource source{.info = *table};
  for (const auto &level : session.SelectDifficultyLevels(tableId)) {
    source.levels.push_back({.info = level});
  }
  for (const auto &group : session.SelectDifficultyCourseGroups(tableId)) {
    for (const auto &course :
         session.SelectDifficultyCourses(tableId, group.groupName)) {
      ChartMetaQuery query;
      query.courseId = course.id;
      query.selectedLongNoteMode = selectedLongNoteMode;
      MusicSelectCourseSource courseSource{.info = course};
      session.QueryChartMeta(query, courseSource.stages);
      source.courses.push_back(std::move(courseSource));
    }
  }
  return source;
}
