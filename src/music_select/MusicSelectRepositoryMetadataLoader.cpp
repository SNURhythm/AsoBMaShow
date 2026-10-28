#include "MusicSelectRepositoryProjection.h"
#include "MusicSelectAssignmentFolders.h"
#include <map>
#include <set>

#include "../path.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <unordered_set>

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
  case skin::MusicSelectBarKind::Container:
    if (!musicSelectIsSolidArchiveDirectory(directory)) return {};
    query.solidArchivesOnly = true;
    break;
  case skin::MusicSelectBarKind::Folder:
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
  if (directory.kind == skin::MusicSelectBarKind::Folder ||
      directory.kind == skin::MusicSelectBarKind::SearchWord) {
    const bool search = directory.kind == skin::MusicSelectBarKind::SearchWord;
    if (search && (!directory.id.value.starts_with("search:") ||
                   directory.id.value.size() == 7)) return directory.presentation;
    MusicSelectFolderStatusAccumulator accumulator(directory.presentation, input, stop);
    bms_parser::ChartMeta meta;
    session.VisitRawPhysicalFolderStatistics(search ? std::filesystem::path{} : directory.directoryPath,
        [&](const ChartFolderStatisticsRow &row) {
          meta.SHA256 = row.sha256;
          meta.KeyMode = row.keyMode;
          meta.LnMode = row.longNoteMode;
          meta.TotalLongNotes = row.totalLongNotes;
          meta.TotalBackSpinNotes = row.totalBackSpinNotes;
          accumulator.add(meta, row.hasPath);
        }, stop, search ? directory.id.value.substr(7) : std::string{});
    return accumulator.finish();
  }
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
  metadata.solidArchiveCount = session.CountSolidArchives();
  metadata.entries = session.SelectEffectiveEntries();
  metadata.folders = session.SelectFolderRecords();
  const auto chartFolders = session.SelectRawChartMetaFolders();
  std::unordered_set<std::filesystem::path> folderPaths;
  folderPaths.reserve(metadata.folders.size() + chartFolders.size());
  for (const auto &folder : metadata.folders) {
    folderPaths.insert(std::filesystem::path(folder.path).lexically_normal());
  }
  for (const auto &path : chartFolders) {
    if (folderPaths.insert(path.lexically_normal()).second) {
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

std::vector<MusicSelectAssignmentChart> loadMusicSelectAssignmentCharts(
    ChartRepository::Session &session, const MusicSelectRepositoryMetadata &metadata,
    const kombo::Day &day, int selectedLongNoteMode) {
  if (day.points >= day.config.dailyGoal &&
      kombo::bonusPoints(day) >= kombo::bonusLimit(day)) return {};
  std::map<std::string, MusicSelectAssignmentChart> candidates;
  std::set<std::pair<std::string, std::string>> levels;
  std::set<std::string> sticky;
  for (const auto &progress : day.assignments) {
    for (const auto &level : progress.requirement.levels)
      levels.emplace(level.source, level.level);
    for (const auto &[hash, achievement] : progress.chartProgress)
      if (!progress.usedCharts.contains(hash)) sticky.insert(hash);
  }
  // Query each selected level once, using indexed metadata only.
  for (const auto &[source, level] : levels) {
    const auto table = std::ranges::find_if(metadata.tables, [&](const auto &table) {
      return table.info.sourceUrl == source;
    });
    if (table == metadata.tables.end()) continue;
    std::vector<ChartMetaRecord> records;
    ChartMetaQuery query;
    query.tableId = table->info.id;
    query.tableLevel = level;
    query.exactTableLevel = true;
    query.selectedLongNoteMode = selectedLongNoteMode;
    session.QueryChartMeta(query, records);
    for (auto &record : records) {
      if (record.unavailable || record.meta.SHA256.empty()) continue;
      const auto hash = record.meta.SHA256;
      auto [entry, inserted] = candidates.try_emplace(hash);
      if (inserted) entry->second.record = std::move(record);
      entry->second.levels.push_back({source, level, {}});
    }
  }
  // Sticky charts may no longer belong to an installed table/level.
  std::vector<std::filesystem::path> paths;
  for (const auto &hash : sticky) {
    if (candidates.contains(hash)) continue;
    for (const auto &meta : session.SelectChartMetaByHash(hash, {}))
      paths.push_back(meta.BmsPath);
  }
  constexpr std::size_t batchSize = 1'024;
  for (std::size_t offset = 0; offset < paths.size(); offset += batchSize) {
    auto batch = session.SelectChartMetaByPaths(std::span(paths).subspan(
        offset, std::min(batchSize, paths.size() - offset)));
    if (batch.status != ChartMetaPathBatchReadStatus::Loaded)
      throw std::runtime_error(batch.diagnostic);
    for (auto &record : batch.records) {
      const auto hash = record.meta.SHA256;
      if (!record.unavailable && !hash.empty())
        candidates.try_emplace(hash, MusicSelectAssignmentChart{std::move(record), {}});
    }
  }
  std::vector<MusicSelectAssignmentChart> result;
  result.reserve(candidates.size());
  for (auto &[hash, source] : candidates) result.push_back(std::move(source));
  return result;
}
