#include "MusicSelectRepositoryProjection.h"

#include "../path.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>

std::vector<ChartMetaRecord>
MusicSelectRepositoryProjection::loadDirectoryRecords(
    ChartRepository::Session &session, const MusicSelectBar &directory,
    int selectedLongNoteMode, const RecentScoreImprovements *improvements,
    std::stop_token stop) {
  const auto checkCancelled = [&] {
    if (stop.stop_requested()) throw std::runtime_error("folder status cancelled");
  };
  checkCancelled();
  ChartMetaQuery query;
  query.selectedLongNoteMode = selectedLongNoteMode;
  query.rawSongData = true;
  switch (directory.kind) {
  case skin::MusicSelectBarKind::Folder:
    if (!session.HasChartMetaForParentFolder(directory.directoryPath, stop)) return {};
    checkCancelled();
    query.recursiveFolder = directory.directoryPath;
    break;
  case skin::MusicSelectBarKind::Hash:
    query.tableId = directory.tableId;
    query.tableLevel = directory.tableLevel;
    break;
  case skin::MusicSelectBarKind::SearchWord:
    if (!directory.id.value.starts_with("search:")) return {};
    query.keyword = directory.id.value.substr(7);
    break;
  case skin::MusicSelectBarKind::Command: {
    if (improvements == nullptr) return {};
    const auto separator = directory.id.value.rfind(':');
    if (separator == std::string::npos) return {};
    const std::string_view suffix(directory.id.value.data() + separator + 1,
                                  directory.id.value.size() - separator - 1);
    int day = -1;
    const auto parsed =
        std::from_chars(suffix.data(), suffix.data() + suffix.size(), day);
    if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() ||
        day < 0 || day >= 30) return {};
    const auto &hashes = directory.id.value.starts_with("command:lamp-update:")
                             ? improvements->lamp[day]
                             : improvements->score[day];
    std::vector<std::filesystem::path> paths;
    for (const auto &hash : hashes) {
      checkCancelled();
      for (const auto &meta : session.SelectChartMetaByHash(hash, {}, stop)) {
        checkCancelled();
        paths.push_back(meta.BmsPath);
      }
    }
    checkCancelled();
    std::ranges::sort(paths, [&](const auto &left, const auto &right) {
      checkCancelled();
      return left < right;
    });
    std::vector<ChartMetaRecord> records;
    constexpr std::size_t batchSize = 1'024;
    for (std::size_t offset = 0; offset < paths.size(); offset += batchSize) {
      checkCancelled();
      auto batch = session.SelectChartMetaByPaths(std::span(paths).subspan(
          offset, std::min(batchSize, paths.size() - offset)), stop);
      if (batch.status != ChartMetaPathBatchReadStatus::Loaded) {
        throw std::runtime_error(batch.diagnostic);
      }
      for (auto &record : batch.records) {
        checkCancelled();
        records.push_back(std::move(record));
      }
    }
    return records;
  }
  default: return {};
  }
  std::vector<ChartMetaRecord> records;
  checkCancelled();
  session.QueryChartMeta(query, records, stop);
  checkCancelled();
  return records;
}

skin::MusicSelectBarFrame MusicSelectRepositoryProjection::loadFolderStatus(
    ChartRepository::Session &session, MusicSelectBar directory,
    MusicSelectRepositoryProjectionInput input, std::stop_token stop) {
  auto records = loadDirectoryRecords(session, directory,
                                      input.selectedLongNoteMode,
                                      input.recentScoreImprovements, stop);
  input.records = records;
  updateFolderStatus(directory, input, stop);
  return directory.presentation;
}

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
