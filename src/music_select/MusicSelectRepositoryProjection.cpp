#include "MusicSelectRepositoryProjection.h"

#include "../BmsMetadataText.h"
#include "../CourseConstraintUtils.h"
#include "../path.h"
#include "../scene/play/GameplayGaugeTypes.h"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>

namespace {

std::string fullTitle(const bms_parser::ChartMeta &meta) {
  return meta.SubTitle.empty() ? meta.Title : meta.Title + " " + meta.SubTitle;
}

std::string chartIdentity(const ChartMetaRecord &record) {
  if (!record.meta.SHA256.empty()) return "sha256:" + record.meta.SHA256;
  if (!record.meta.MD5.empty()) return "md5:" + record.meta.MD5;
  return "path:" + fspath_to_utf8(record.meta.BmsPath.lexically_normal());
}

std::string folderIdentity(const std::filesystem::path &path) {
  return "folder:" + fspath_to_utf8(path.lexically_normal());
}

bool pathAtOrInside(const std::filesystem::path &path,
                    const std::filesystem::path &root) {
  if (path.empty() || root.empty()) return false;
  const auto normalizedPath = path.lexically_normal();
  const auto normalizedRoot = root.lexically_normal();
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
  if (!record.meta.Folder.empty()) return record.meta.Folder.lexically_normal();
  return record.meta.BmsPath.parent_path().lexically_normal();
}

skin::MusicSelectCourseConstraint courseConstraint(int id) {
  return static_cast<skin::MusicSelectCourseConstraint>(id - 1);
}

struct ProjectionBuilder {
  MusicSelectRepositoryProjectionInput input;
  MusicSelectProjection result;

  std::optional<ScoreBestSnapshot> score(const ChartMetaRecord &record) const {
    return input.scoreFor
               ? input.scoreFor(record.meta, input.selectedLongNoteMode)
               : std::nullopt;
  }

  MusicSelectBarId addSong(const ChartMetaRecord &record,
                           std::string_view context) {
    const MusicSelectBarId id{std::string(context) + ":" +
                              chartIdentity(record)};
    auto best = score(record);
    const int lamp = best ? beatorajaClearType(best->clearType) : 0;
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
                 const std::vector<ChartMetaRecord> &records) const {
    for (const auto &record : records) {
      if (record.unavailable || record.meta.BmsPath.empty() ||
          !modeMatches(input.modeFilter, songMode(record.meta))) {
        continue;
      }
      const auto best = score(record);
      const int lamp = best ? beatorajaClearType(best->clearType) : 0;
      ++directory.presentation
            .folderLampCounts[static_cast<std::size_t>(lamp)];
      int rank = 0;
      if (best && best->maxScore > 0) {
        rank = std::min(27, best->score * 27 / best->maxScore);
      }
      ++directory.presentation
            .folderRankCounts[static_cast<std::size_t>(rank)];
    }
    const auto firstLamp = std::ranges::find_if(
        directory.presentation.folderLampCounts,
        [](int count) { return count > 0; });
    directory.presentation.lamp =
        firstLamp == directory.presentation.folderLampCounts.end()
            ? 0
            : static_cast<int>(std::distance(
                  directory.presentation.folderLampCounts.begin(),
                  firstLamp));
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
          child.children =
              addElementSongs(records, child.id.value);
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
  std::vector<std::filesystem::path> physicalRoots;
  const auto ensureFolder = [&](const std::filesystem::path &path)
      -> FolderNode & {
    const auto normalized = path.lexically_normal();
    return folders.try_emplace(normalized, FolderNode{.path = normalized})
        .first->second;
  };
  auto addChild = [&](const std::filesystem::path &parent,
                      const std::filesystem::path &child) {
    auto &children = ensureFolder(parent).children;
    if (std::ranges::find(children, child.lexically_normal()) ==
        children.end()) {
      children.push_back(child.lexically_normal());
    }
  };

  if (input.metadata != nullptr) {
    for (const auto &entry : input.metadata->entries) {
      const std::filesystem::path root(entry.path);
      if (root.empty()) continue;
      physicalRoots.push_back(root.lexically_normal());
      (void)ensureFolder(root);
    }
  }
  for (const auto &record : input.records) {
    const auto folder = physicalFolder(record);
    ensureFolder(folder).records.push_back(&record);
    auto root = std::ranges::find_if(physicalRoots, [&](const auto &candidate) {
      return pathAtOrInside(folder, candidate);
    });
    if (root == physicalRoots.end()) {
      if (std::ranges::find(physicalRoots, folder) == physicalRoots.end()) {
        physicalRoots.push_back(folder);
      }
      continue;
    }
    auto current = folder;
    while (current != *root) {
      const auto parent = current.parent_path();
      addChild(parent, current);
      current = parent;
    }
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
                         .exists = true},
        .selectable = true,
        .sortable = true,
    };
    if (!node.records.empty()) {
      folder.children = builder.addPhysicalSongs(node.records, folder.id.value);
      std::vector<ChartMetaRecord> values;
      values.reserve(node.records.size());
      for (const auto *record : node.records) values.push_back(*record);
      builder.aggregate(folder, values);
    }
    for (const auto &child : node.children) {
      folder.children.push_back(addFolder(child));
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
