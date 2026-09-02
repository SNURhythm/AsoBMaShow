#include "MusicSelectRepositoryProjection.h"

MusicSelectRepositoryMetadata MusicSelectRepositoryProjection::loadMetadata(
    ChartRepository::Session &session, int selectedLongNoteMode) {
  MusicSelectRepositoryMetadata metadata;
  metadata.entries = session.SelectEffectiveEntries();
  metadata.folders = session.SelectFolderRecords();
  for (const auto &table : session.SelectDifficultyTables()) {
    MusicSelectDifficultyTableSource source{.info = table};
    for (const auto &level : session.SelectDifficultyLevels(table.id)) {
      ChartMetaQuery query;
      query.tableId = table.id;
      query.tableLevel = level.level;
      query.selectedLongNoteMode = selectedLongNoteMode;
      MusicSelectDifficultyLevelSource levelSource{.info = level};
      session.QueryChartMeta(query, levelSource.records);
      source.levels.push_back(std::move(levelSource));
    }
    for (const auto &group : session.SelectDifficultyCourseGroups(table.id)) {
      for (const auto &course :
           session.SelectDifficultyCourses(table.id, group.groupName)) {
        ChartMetaQuery query;
        query.courseId = course.id;
        query.selectedLongNoteMode = selectedLongNoteMode;
        MusicSelectCourseSource courseSource{.info = course};
        session.QueryChartMeta(query, courseSource.stages);
        source.courses.push_back(std::move(courseSource));
      }
    }
    metadata.tables.push_back(std::move(source));
  }
  return metadata;
}
