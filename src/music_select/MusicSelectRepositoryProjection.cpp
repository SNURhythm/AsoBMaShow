#include "MusicSelectRepositoryProjection.h"

#include "../BmsMetadataText.h"
#include "../CourseConstraintUtils.h"
#include "../path.h"
#include "../scene/play/GameplayGaugeTypes.h"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>

namespace {

std::string fullTitle(const bms_parser::ChartMeta &meta) {
  return meta.SubTitle.empty() ? meta.Title : meta.Title + " " + meta.SubTitle;
}

std::string chartIdentity(const ChartMetaRecord &record) {
  if (!record.meta.SHA256.empty()) return "sha256:" + record.meta.SHA256;
  if (!record.meta.MD5.empty()) return "md5:" + record.meta.MD5;
  return "path:" + fspath_to_utf8(record.meta.BmsPath.lexically_normal());
}

std::filesystem::path normalizedFolderPath(const std::filesystem::path &path) {
  auto normalized = path.lexically_normal();
  while (normalized.has_relative_path() && normalized.filename().empty()) {
    normalized = normalized.parent_path();
  }
  return normalized;
}

std::string folderIdentity(const std::filesystem::path &path) {
  return "folder:" + fspath_to_utf8(normalizedFolderPath(path));
}

bool pathAtOrInside(const std::filesystem::path &path,
                    const std::filesystem::path &root) {
  if (path.empty() || root.empty()) return false;
  const auto normalizedPath = normalizedFolderPath(path);
  const auto normalizedRoot = normalizedFolderPath(root);
  if (normalizedPath == normalizedRoot) return true;
  const auto relative = normalizedPath.lexically_relative(normalizedRoot);
  if (relative.empty() || relative.is_absolute()) return false;
  const auto first = relative.begin();
  return first != relative.end() && *first != std::filesystem::path("..") &&
         *first != std::filesystem::path(".");
}

int beatorajaClearType(int rank) {
  if (rank == kNoClearTypeRank) return 0;
  if (rank >= kClearTypeFullComboRank) return 8;
  if (rank >= kClearTypeExHardClearRank) return 7;
  if (rank >= kClearTypeHardClearRank) return 6;
  if (rank >= kClearTypeNormalClearRank) return 5;
  if (rank >= kClearTypeEasyClearRank) return 4;
  if (rank >= kClearTypeLightAssistedEasyClearRank) return 3;
  if (rank >= kClearTypeAssistedEasyClearRank) return 2;
  return 1;
}

int songMode(const bms_parser::ChartMeta &meta) {
  if (meta.KeyMode == 5 && !meta.IsDP) return 5;
  if (meta.KeyMode == 7 && !meta.IsDP) return 7;
  if (meta.KeyMode == 9 && !meta.IsDP) return 9;
  if (meta.KeyMode == 10 || (meta.KeyMode == 5 && meta.IsDP)) return 10;
  if (meta.KeyMode == 14 || (meta.KeyMode == 7 && meta.IsDP)) return 14;
  if (meta.KeyMode == 24 && !meta.IsDP) return 25;
  if (meta.KeyMode == 48 || (meta.KeyMode == 24 && meta.IsDP)) return 50;
  return 0;
}

bool modeMatches(std::string_view filter, int mode) {
  if (mode == 0 || filter == "ALL") return true;
  if (filter == "7KEY") return mode == 7;
  if (filter == "14KEY") return mode == 14;
  if (filter == "9KEY") return mode == 9;
  if (filter == "5KEY") return mode == 5;
  if (filter == "10KEY") return mode == 10;
  if (filter == "24KEY") return mode == 25;
  if (filter == "48KEY") return mode == 50;
  if (filter == "SINGLE") return mode == 5 || mode == 7;
  if (filter == "DOUBLE") return mode == 10 || mode == 14;
  return false;
}

int songFeatures(const ChartMetaRecord &record) {
  const auto &meta = record.meta;
  int features = 0;
  if (meta.TotalLongNotes > 0 || meta.TotalBackSpinNotes > 0) {
    switch (meta.LnMode) {
    case 1: features |= skin::MusicSelectFeatureLongNote; break;
    case 2: features |= skin::MusicSelectFeatureChargeNote; break;
    case 3: features |= skin::MusicSelectFeatureHellChargeNote; break;
    default: features |= skin::MusicSelectFeatureUndefinedLn; break;
    }
  }
  if (meta.TotalLandmineNotes > 0) {
    features |= skin::MusicSelectFeatureMine;
  }
  if (record.hasRandomSequence) {
    features |= skin::MusicSelectFeatureRandom;
  }
  return features;
}

std::filesystem::path physicalFolder(const ChartMetaRecord &record) {
  if (!record.meta.Folder.empty()) return normalizedFolderPath(record.meta.Folder);
  return normalizedFolderPath(record.meta.BmsPath.parent_path());
}

skin::MusicSelectCourseConstraint courseConstraint(int id) {
  return static_cast<skin::MusicSelectCourseConstraint>(id - 1);
}

}

MusicSelectFolderStatusAccumulator::MusicSelectFolderStatusAccumulator(
    skin::MusicSelectBarFrame frame, MusicSelectRepositoryProjectionInput input,
    std::stop_token stop)
    : frame_(std::move(frame)), input_(input), stop_(stop) {
  checkCancelled();
  frame_.folderLampCounts = {};
  frame_.folderRankCounts = {};
  frame_.lamp = 0;
  frame_.rivalLamp = 0;
}

void MusicSelectFolderStatusAccumulator::add(const bms_parser::ChartMeta &meta,
                                            bool available) {
  checkCancelled();
  if (!available || !modeMatches(input_.modeFilter, songMode(meta))) return;
  const auto best = input_.scoreFor
                        ? input_.scoreFor(meta, input_.selectedLongNoteMode)
                        : std::nullopt;
  checkCancelled();
  const int clear = input_.clearFor
                        ? input_.clearFor(meta, input_.selectedLongNoteMode)
                        : best ? best->clearType : kNoClearTypeRank;
  checkCancelled();
  ++frame_.folderLampCounts[static_cast<std::size_t>(beatorajaClearType(clear))];
  int rank = 0;
  if (best && best->maxScore > 0) {
    rank = static_cast<int>(std::clamp<std::int64_t>(
        static_cast<std::int64_t>(best->score) * 27 / best->maxScore, 0, 27));
  }
  ++frame_.folderRankCounts[static_cast<std::size_t>(rank)];
}

skin::MusicSelectBarFrame MusicSelectFolderStatusAccumulator::finish() {
  const auto firstLamp = std::ranges::find_if(
      frame_.folderLampCounts, [](int count) { return count > 0; });
  frame_.lamp = firstLamp == frame_.folderLampCounts.end()
                    ? 0
                    : static_cast<int>(std::distance(
                          frame_.folderLampCounts.begin(), firstLamp));
  checkCancelled();
  return std::move(frame_);
}

void MusicSelectFolderStatusAccumulator::checkCancelled() const {
  if (stop_.stop_requested()) throw std::runtime_error("folder status cancelled");
}

namespace {

struct ProjectionBuilder {
  MusicSelectRepositoryProjectionInput input;
  MusicSelectProjection result;

  std::optional<ScoreBestSnapshot> score(const ChartMetaRecord &record) const {
    return input.scoreFor
               ? input.scoreFor(record.meta, input.selectedLongNoteMode)
               : std::nullopt;
  }

  int clearLamp(const ChartMetaRecord &record,
                const std::optional<ScoreBestSnapshot> &best) const {
    const int rank = input.clearFor
                         ? input.clearFor(record.meta, input.selectedLongNoteMode)
                         : best ? best->clearType : kNoClearTypeRank;
    return beatorajaClearType(rank);
  }

  MusicSelectBarId addSong(const ChartMetaRecord &record,
                           std::string_view context) {
    const MusicSelectBarId id{std::string(context) + ":" +
                              chartIdentity(record)};
    auto best = score(record);
    const int lamp = clearLamp(record, best);
    const std::string title = fullTitle(record.meta);
    result.bars.push_back(
        {.id = id,
         .kind = skin::MusicSelectBarKind::Song,
         .title = title,
         .chart = record,
         .score = std::move(best),
         .replayExists =
             input.replayExistsFor
                 ? input.replayExistsFor(record, input.selectedLongNoteMode)
                 : std::array<bool, 4>{},
         .presentation = {.kind = skin::MusicSelectBarKind::Song,
                          .title = title,
                          .exists = !record.unavailable &&
                                    !record.meta.BmsPath.empty(),
                          .addDateSeconds = record.addDateSeconds,
                          .lamp = lamp,
                          .difficulty = record.meta.Difficulty,
                          .level = static_cast<int>(record.meta.PlayLevel),
                          .featureFlags = songFeatures(record)},
         .selectable = true});
    return id;
  }

  std::vector<MusicSelectBarId>
  addPhysicalSongs(const std::vector<const ChartMetaRecord *> &records,
                   std::string_view context) {
    std::vector<const ChartMetaRecord *> unique;
    std::set<std::string> hashes;
    for (const auto *record : records) {
      if (record != nullptr && hashes.insert(record->meta.SHA256).second) {
        unique.push_back(record);
      }
    }
    std::vector<MusicSelectBarId> ids;
    ids.reserve(unique.size());
    for (auto record = unique.rbegin(); record != unique.rend(); ++record) {
      ids.push_back(addSong(**record, context));
    }
    return ids;
  }

  std::vector<MusicSelectBarId>
  addElementSongs(const std::vector<ChartMetaRecord> &records,
                  std::string_view context) {
    std::vector<MusicSelectBarId> ids;
    ids.reserve(records.size());
    for (const auto &record : records) ids.push_back(addSong(record, context));
    return ids;
  }

  void aggregate(MusicSelectBar &directory,
                 std::span<const ChartMetaRecord> records,
                 std::stop_token stop = {}) const {
    MusicSelectFolderStatusAccumulator accumulator(directory.presentation, input, stop);
    for (const auto &record : records) {
      accumulator.add(record.meta, !record.unavailable && !record.meta.BmsPath.empty());
    }
    directory.presentation = accumulator.finish();
  }

  void addCommands() {
    for (const auto &[prefix, title] :
         {std::pair{"lamp-update", "LAMP UPDATE"},
          std::pair{"score-update", "SCORE UPDATE"}}) {
      MusicSelectBar container{
          .id = {std::string("container:") + prefix},
          .kind = skin::MusicSelectBarKind::Container,
          .title = title,
          .presentation = {.kind = skin::MusicSelectBarKind::Container,
                           .title = title,
                           .exists = true},
          .selectable = true,
          .sortable = true,
      };
      const bool lamp = std::string_view(prefix) == "lamp-update";
      for (int day = 0; day < 30; ++day) {
        const std::string dayTitle =
            day == 0 ? "TODAY" : std::to_string(day) + "DAYS AGO";
        const MusicSelectBarId childId{
            std::string("command:") + prefix + ":" + std::to_string(day)};
        container.children.push_back(childId);
        MusicSelectBar child{
            .id = childId,
            .kind = skin::MusicSelectBarKind::Command,
            .title = dayTitle,
            .presentation = {.kind = skin::MusicSelectBarKind::Command,
                             .title = dayTitle,
                             .exists = true},
            .selectable = true,
            .sortable = true};
        if (input.recentScoreImprovements != nullptr) {
          const auto &hashes = lamp
                                   ? input.recentScoreImprovements->lamp[day]
                                   : input.recentScoreImprovements->score[day];
          std::vector<ChartMetaRecord> records;
          for (const auto &record : input.records) {
            if (hashes.contains(
                    asobmshow::bms_metadata::normalizedHash(
                        record.meta.SHA256))) {
              records.push_back(record);
            }
          }
          std::vector<const ChartMetaRecord *> physicalRecords;
          physicalRecords.reserve(records.size());
          for (const auto &record : records) physicalRecords.push_back(&record);
          child.children = addPhysicalSongs(physicalRecords, child.id.value);
          aggregate(child, records);
        }
        result.bars.push_back(std::move(child));
      }
      result.root.push_back(container.id);
      result.bars.push_back(std::move(container));
    }
  }
};

} // namespace

const MusicSelectBar *MusicSelectProjection::find(
    const MusicSelectBarId &id) const {
  const auto found = std::ranges::find(bars, id, &MusicSelectBar::id);
  return found == bars.end() ? nullptr : &*found;
}

MusicSelectProjection MusicSelectRepositoryProjection::projectRoot(
    const MusicSelectRepositoryMetadata &metadata,
    std::span<const std::string> searches,
    std::uint64_t repositoryRevision) const {
  MusicSelectProjection result{.repositoryRevision = repositoryRevision};
  std::vector<std::filesystem::path> physicalRoots;
  for (const auto &entry : metadata.entries) {
    const auto path = normalizedFolderPath(std::filesystem::path(entry.path));
    if (path.empty() ||
        std::ranges::any_of(physicalRoots, [&](const auto &candidate) {
          return pathAtOrInside(path, candidate);
        })) {
      continue;
    }
    std::erase_if(physicalRoots, [&](const auto &candidate) {
      return pathAtOrInside(candidate, path);
    });
    physicalRoots.push_back(path.lexically_normal());
  }
  for (const auto &path : physicalRoots) {
    const std::string title = path.filename().empty()
                                  ? fspath_to_utf8(path)
                                  : fspath_to_utf8(path.filename());
    MusicSelectBar folder{
        .id = {folderIdentity(path)},
        .kind = skin::MusicSelectBarKind::Folder,
        .title = title,
        .directoryPath = path,
        .presentation = {.kind = skin::MusicSelectBarKind::Folder,
                         .title = title,
                         .exists = true},
        .selectable = true,
        .sortable = true,
        .childrenLoaded = false,
    };
    result.root.push_back(folder.id);
    result.bars.push_back(std::move(folder));
  }

  MusicSelectBar localCourses{
      .id = {"table:course"},
      .kind = skin::MusicSelectBarKind::Table,
      .title = "COURSE",
      .presentation = {.kind = skin::MusicSelectBarKind::Table,
                       .title = "COURSE",
                       .exists = true},
      .selectable = true,
      .sortable = true,
      .childrenLoaded = false,
  };
  result.root.push_back(localCourses.id);
  result.bars.push_back(std::move(localCourses));

  for (const auto &source : metadata.tables) {
    MusicSelectBar table{
        .id = {"table:" + std::to_string(source.info.id)},
        .kind = skin::MusicSelectBarKind::Table,
        .title = source.info.name,
        .tableId = source.info.id,
        .tableUrl = source.info.sourceUrl,
        .presentation = {.kind = skin::MusicSelectBarKind::Table,
                         .title = source.info.name,
                         .exists = true},
        .selectable = true,
        .sortable = true,
        .childrenLoaded = false,
    };
    result.root.push_back(table.id);
    result.bars.push_back(std::move(table));
  }

  for (const auto &[id, title] :
       {std::pair{"lamp-update", "LAMP UPDATE"},
        std::pair{"score-update", "SCORE UPDATE"}}) {
    MusicSelectBar container{
        .id = {std::string("container:") + id},
        .kind = skin::MusicSelectBarKind::Container,
        .title = title,
        .presentation = {.kind = skin::MusicSelectBarKind::Container,
                         .title = title,
                         .exists = true},
        .selectable = true,
        .sortable = true,
        .childrenLoaded = false,
    };
    result.root.push_back(container.id);
    result.bars.push_back(std::move(container));
  }

  for (const auto &text : searches) {
    const std::string title = "Search : '" + text + "'";
    MusicSelectBar search{
        .id = {"search:" + text},
        .kind = skin::MusicSelectBarKind::SearchWord,
        .title = title,
        .presentation = {.kind = skin::MusicSelectBarKind::SearchWord,
                         .title = title,
                         .exists = true},
        .selectable = true,
        .sortable = true,
        .childrenLoaded = false,
    };
    result.root.push_back(search.id);
    result.bars.push_back(std::move(search));
  }
  return result;
}

MusicSelectBar MusicSelectRepositoryProjection::projectSong(
    const ChartMetaRecord &record, std::string_view context,
    MusicSelectRepositoryProjectionInput input) {
  ProjectionBuilder builder{.input = std::move(input)};
  builder.addSong(record, context);
  return std::move(builder.result.bars.front());
}

std::vector<MusicSelectBar>
MusicSelectRepositoryProjection::projectDirectoryFolders(
    const MusicSelectRepositoryMetadata &metadata,
    const std::filesystem::path &directory) {
  const auto parent = normalizedFolderPath(directory);
  std::map<std::filesystem::path, std::int64_t> children;
  for (const auto &record : metadata.folders) {
    const auto candidate = normalizedFolderPath(record.path);
    const auto relative = candidate.lexically_relative(parent);
    if (relative.empty() || relative.is_absolute()) continue;
    const auto first = relative.begin();
    if (first == relative.end() || *first == "." || *first == "..") continue;
    const auto path = parent / *first;
    const auto found = children.try_emplace(path, 0).first;
    if (candidate == path) found->second = record.addDateSeconds;
  }
  std::vector<MusicSelectBar> result;
  result.reserve(children.size());
  for (const auto &[path, addDate] : children) {
    const auto title = path.filename().empty() ? fspath_to_utf8(path)
                                               : fspath_to_utf8(path.filename());
    result.push_back({
        .id = {folderIdentity(path)},
        .kind = skin::MusicSelectBarKind::Folder,
        .title = title,
        .directoryPath = path,
        .presentation = {.kind = skin::MusicSelectBarKind::Folder,
                         .title = title,
                         .exists = true,
                         .addDateSeconds = addDate},
        .selectable = true,
        .sortable = true,
        .childrenLoaded = false,
    });
  }
  return result;
}

void MusicSelectRepositoryProjection::updateFolderStatus(
    MusicSelectBar &directory, MusicSelectRepositoryProjectionInput input,
    std::stop_token stop) {
  switch (directory.kind) {
  case skin::MusicSelectBarKind::Folder:
  case skin::MusicSelectBarKind::Hash:
  case skin::MusicSelectBarKind::SearchWord:
  case skin::MusicSelectBarKind::Command:
    ProjectionBuilder{.input = input}.aggregate(directory, input.records, stop);
    break;
  default: break;
  }
}

MusicSelectProjection MusicSelectRepositoryProjection::project(
    MusicSelectRepositoryProjectionInput input) const {
  ProjectionBuilder builder{.input = input,
                            .result = {.repositoryRevision =
                                           input.repositoryRevision}};

  struct FolderNode {
    std::filesystem::path path;
    std::vector<std::filesystem::path> children;
    std::vector<const ChartMetaRecord *> records;
  };
  std::map<std::filesystem::path, FolderNode> folders;
  std::map<std::filesystem::path, std::int64_t> folderAddDates;
  std::vector<std::filesystem::path> physicalRoots;
  const auto ensureFolder = [&](const std::filesystem::path &path)
      -> FolderNode & {
    const auto normalized = normalizedFolderPath(path);
    return folders.try_emplace(normalized, FolderNode{.path = normalized})
        .first->second;
  };
  auto addChild = [&](const std::filesystem::path &parent,
                      const std::filesystem::path &child) {
    auto &children = ensureFolder(parent).children;
    if (std::ranges::find(children, normalizedFolderPath(child)) ==
        children.end()) {
      children.push_back(normalizedFolderPath(child));
    }
  };
  auto addPhysicalRoot = [&](const std::filesystem::path &path) {
    const auto normalized = normalizedFolderPath(path);
    if (std::ranges::any_of(physicalRoots, [&](const auto &candidate) {
          return pathAtOrInside(normalized, candidate);
        })) {
      return;
    }
    std::size_t insertionIndex = physicalRoots.size();
    for (std::size_t index = 0; index < physicalRoots.size(); ++index) {
      if (pathAtOrInside(physicalRoots[index], normalized)) {
        insertionIndex = std::min(insertionIndex, index);
      }
    }
    std::erase_if(physicalRoots, [&](const auto &candidate) {
      return pathAtOrInside(candidate, normalized);
    });
    insertionIndex = std::min(insertionIndex, physicalRoots.size());
    physicalRoots.insert(physicalRoots.begin() + insertionIndex, normalized);
  };

  if (input.metadata != nullptr) {
    for (const auto &record : input.metadata->folders) {
      const auto path = normalizedFolderPath(std::filesystem::path(record.path));
      if (!path.empty()) {
        folderAddDates.insert_or_assign(path, record.addDateSeconds);
      }
    }
    for (const auto &entry : input.metadata->entries) {
      const std::filesystem::path root(entry.path);
      if (root.empty()) continue;
      addPhysicalRoot(root);
      (void)ensureFolder(root);
    }
  }
  for (const auto &record : input.records) {
    const auto folder = physicalFolder(record);
    const auto parent = folder.parent_path();
    ensureFolder(folder).records.push_back(&record);
    ensureFolder(parent).records.push_back(&record);
    const auto root = std::ranges::find_if(
        physicalRoots, [&](const auto &candidate) {
          return pathAtOrInside(folder, candidate);
        });
    if (root == physicalRoots.end()) addPhysicalRoot(parent);
  }
  const auto connectFolderToRoot = [&](const std::filesystem::path &path) {
    auto current = normalizedFolderPath(path);
    const auto root = std::ranges::find_if(
        physicalRoots, [&](const auto &candidate) {
          return pathAtOrInside(current, candidate);
        });
    if (root == physicalRoots.end()) return;
    while (current != *root) {
      const auto parent = current.parent_path();
      addChild(parent, current);
      current = parent;
    }
  };
  if (input.metadata != nullptr) {
    for (const auto &entry : input.metadata->entries) {
      connectFolderToRoot(entry.path);
    }
    // FolderBar queries FolderData when the current directory has no songs.
    // Its persisted rows therefore remain real selector bars even when a
    // branch contains no chart record to otherwise introduce it here.
    for (const auto &record : input.metadata->folders) {
      const auto path = normalizedFolderPath(std::filesystem::path(record.path));
      if (path.empty() ||
          std::ranges::none_of(physicalRoots, [&](const auto &root) {
            return pathAtOrInside(path, root);
          })) {
        continue;
      }
      (void)ensureFolder(path);
      connectFolderToRoot(path);
    }
  }
  for (const auto &record : input.records) {
    connectFolderToRoot(physicalFolder(record));
  }

  std::function<MusicSelectBarId(const std::filesystem::path &)> addFolder;
  addFolder = [&](const std::filesystem::path &path) {
    auto &node = ensureFolder(path);
    const std::string title =
        node.path.filename().empty() ? fspath_to_utf8(node.path)
                                     : fspath_to_utf8(node.path.filename());
    MusicSelectBar folder{
        .id = {folderIdentity(node.path)},
        .kind = skin::MusicSelectBarKind::Folder,
        .title = title,
        .directoryPath = node.path,
        .presentation = {.kind = skin::MusicSelectBarKind::Folder,
                         .title = title,
                         .exists = true,
                         .addDateSeconds =
                             folderAddDates.contains(node.path)
                                 ? folderAddDates.at(node.path)
                                 : 0},
        .selectable = true,
        .sortable = true,
    };
    if (!node.records.empty()) {
      std::vector<const ChartMetaRecord *> descendants;
      std::vector<ChartMetaRecord> values;
      for (const auto &record : input.records) {
        if (!pathAtOrInside(physicalFolder(record), node.path)) continue;
        descendants.push_back(&record);
        values.push_back(record);
      }
      folder.children = builder.addPhysicalSongs(descendants, folder.id.value);
      builder.aggregate(folder, values);
    } else {
      for (const auto &child : node.children) {
        folder.children.push_back(addFolder(child));
      }
    }
    const auto id = folder.id;
    builder.result.bars.push_back(std::move(folder));
    return id;
  };
  for (const auto &root : physicalRoots) {
    builder.result.root.push_back(addFolder(root));
  }

  MusicSelectBar localCourses{
      .id = {"table:course"},
      .kind = skin::MusicSelectBarKind::Table,
      .title = "COURSE",
      .presentation = {.kind = skin::MusicSelectBarKind::Table,
                       .title = "COURSE",
                       .exists = true},
      .selectable = true,
      .sortable = true,
  };
  builder.result.root.push_back(localCourses.id);
  builder.result.bars.push_back(std::move(localCourses));

  if (input.metadata != nullptr) {
    for (const auto &tableSource : input.metadata->tables) {
      MusicSelectBar table{
          .id = {"table:" + std::to_string(tableSource.info.id)},
          .kind = skin::MusicSelectBarKind::Table,
          .title = tableSource.info.name,
          .tableId = tableSource.info.id,
          .tableUrl = tableSource.info.sourceUrl,
          .presentation = {.kind = skin::MusicSelectBarKind::Table,
                           .title = tableSource.info.name,
                           .exists = true},
          .selectable = true,
          .sortable = true,
      };
      for (const auto &levelSource : tableSource.levels) {
        MusicSelectBar level{
            .id = {"hash:table:" + std::to_string(tableSource.info.id) +
                   ":" + levelSource.info.level},
            .kind = skin::MusicSelectBarKind::Hash,
            .title = levelSource.info.tableSymbol + levelSource.info.level,
            .tableId = tableSource.info.id,
            .tableLevel = levelSource.info.level,
            .presentation = {.kind = skin::MusicSelectBarKind::Hash,
                             .title = levelSource.info.tableSymbol +
                                      levelSource.info.level,
                             .exists = true},
            .selectable = true,
            .sortable = true,
        };
        level.children =
            builder.addElementSongs(levelSource.records, level.id.value);
        builder.aggregate(level, levelSource.records);
        table.children.push_back(level.id);
        builder.result.bars.push_back(std::move(level));
      }
      for (const auto &courseSource : tableSource.courses) {
        MusicSelectBar grade{
            .id = {"grade:" + std::to_string(courseSource.info.id)},
            .kind = skin::MusicSelectBarKind::Grade,
            .title = courseSource.info.name,
            .tableId = tableSource.info.id,
            .courseId = courseSource.info.id,
            .courseKey = courseSource.info.courseKey,
            .courseGroupName = courseSource.info.groupName,
            .courseConstraintJson = courseSource.info.constraintJson,
            .courseCharts = courseSource.stages,
            .presentation = {.kind = skin::MusicSelectBarKind::Grade,
                             .title = courseSource.info.name,
                             // CourseData.validate rejects zero-song courses
                             // before Beatoraja constructs a GradeBar.
                             .exists = !courseSource.stages.empty() &&
                                       std::ranges::all_of(
                                           courseSource.stages,
                                           [](const auto &stage) {
                                             return !stage.unavailable &&
                                                    !stage.meta.BmsPath.empty();
                                           })},
            .selectable = true,
        };
        for (const int id : beatorajaCourseConstraintIdsFromJson(
                 courseSource.info.constraintJson)) {
          grade.courseConstraints.push_back(courseConstraint(id));
        }
        for (const auto &stage : courseSource.stages) {
          grade.courseStages.push_back(
              {.title = fullTitle(stage.meta),
               .hasPath = !stage.unavailable &&
                          !stage.meta.BmsPath.empty()});
          grade.courseTotalNotes += stage.meta.TotalNotes;
          grade.presentation.featureFlags |= songFeatures(stage);
        }
        if (input.courseScoresFor) {
          const auto scores = input.courseScoresFor(
              grade.courseKey, grade.courseId, input.selectedLongNoteMode,
              !courseSource.stages.empty() &&
                  courseSource.stages.front().meta.IsDP);
          int rank = kNoClearTypeRank;
          for (const auto &score : scores) {
            if (score) rank = std::max(rank, score->clearType);
          }
          grade.presentation.lamp = beatorajaClearType(rank);
          grade.score = scores.front();

          // GradeBar checks trophies from last to first against the normal,
          // mirror, and random ScoreData records. Each record independently
          // retains its highest EX score and lowest BP.
          for (auto trophy = courseSource.info.trophies.rbegin();
               trophy != courseSource.info.trophies.rend(); ++trophy) {
            const bool qualified = std::ranges::any_of(
                scores, [&](const std::optional<ScoreBestSnapshot> &score) {
                  return score && score->badPoints &&
                         score->maxScore != 0 &&
                         trophy->missRate >=
                             *score->badPoints * 200.0 / score->maxScore &&
                         trophy->scoreRate <=
                             score->score * 100.0 / score->maxScore;
                });
            if (qualified) {
              grade.presentation.trophyName = trophy->name;
              break;
            }
          }
        }
        if (grade.presentation.exists && input.courseReplayExistsFor) {
          grade.replayExists = input.courseReplayExistsFor(
              grade, input.selectedLongNoteMode);
        }
        table.children.push_back(grade.id);
        builder.result.bars.push_back(std::move(grade));
      }
      builder.result.root.push_back(table.id);
      builder.result.bars.push_back(std::move(table));
    }
  }

  builder.addCommands();
  for (const auto &searchSource : input.searches) {
    const std::string title = "Search : '" + searchSource.text + "'";
    MusicSelectBar search{
        .id = {"search:" + searchSource.text},
        .kind = skin::MusicSelectBarKind::SearchWord,
        .title = title,
        .presentation = {.kind = skin::MusicSelectBarKind::SearchWord,
                         .title = title,
                         .exists = true},
        .selectable = true,
        .sortable = true,
    };
    std::vector<const ChartMetaRecord *> records;
    records.reserve(searchSource.records.size());
    for (const auto &record : searchSource.records) records.push_back(&record);
    search.children = builder.addPhysicalSongs(records, search.id.value);
    builder.aggregate(search, searchSource.records);
    builder.result.root.push_back(search.id);
    builder.result.bars.push_back(std::move(search));
  }
  return std::move(builder.result);
}
