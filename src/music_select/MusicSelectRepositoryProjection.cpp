#include "MusicSelectRepositoryProjection.h"

#include "../scene/play/GameplayGaugeTypes.h"
#include "../path.h"

#include <algorithm>
#include <map>
#include <ranges>

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

int songFeatures(const bms_parser::ChartMeta &meta) {
  int features = 0;
  if (meta.TotalLongNotes > 0 || meta.TotalBackSpinNotes > 0) {
    switch (meta.LnMode) {
    case 1:
      features |= skin::MusicSelectFeatureLongNote;
      break;
    case 2:
      features |= skin::MusicSelectFeatureChargeNote;
      break;
    case 3:
      features |= skin::MusicSelectFeatureHellChargeNote;
      break;
    default:
      features |= skin::MusicSelectFeatureUndefinedLn;
      break;
    }
  }
  if (meta.TotalLandmineNotes > 0) {
    features |= skin::MusicSelectFeatureMine;
  }
  if (meta.RandomSeed.has_value() || !meta.RandomValues.empty()) {
    features |= skin::MusicSelectFeatureRandom;
  }
  return features;
}

std::filesystem::path physicalFolder(const ChartMetaRecord &record) {
  if (!record.meta.Folder.empty()) return record.meta.Folder.lexically_normal();
  return record.meta.BmsPath.parent_path().lexically_normal();
}

} // namespace

const MusicSelectBar *MusicSelectProjection::find(
    const MusicSelectBarId &id) const {
  const auto found = std::ranges::find(bars, id, &MusicSelectBar::id);
  return found == bars.end() ? nullptr : &*found;
}

MusicSelectProjection MusicSelectRepositoryProjection::project(
    MusicSelectRepositoryProjectionInput input) const {
  MusicSelectProjection result{.repositoryRevision = input.repositoryRevision};
  std::map<std::filesystem::path, std::size_t> folders;
  result.bars.reserve(input.records.size() * 2);

  for (const auto &record : input.records) {
    const auto folderPath = physicalFolder(record);
    auto folder = folders.find(folderPath);
    if (folder == folders.end()) {
      const std::size_t index = result.bars.size();
      const MusicSelectBarId id{folderIdentity(folderPath)};
      result.root.push_back(id);
      result.bars.push_back(
          {.id = id,
           .kind = skin::MusicSelectBarKind::Folder,
           .title = folderPath.filename().empty()
                        ? fspath_to_utf8(folderPath)
                        : fspath_to_utf8(folderPath.filename()),
           .directoryPath = folderPath,
           .presentation = {.kind = skin::MusicSelectBarKind::Folder,
                            .title = folderPath.filename().empty()
                                         ? fspath_to_utf8(folderPath)
                                         : fspath_to_utf8(folderPath.filename()),
                            .exists = true},
           .selectable = true,
           .sortable = true});
      folder = folders.emplace(folderPath, index).first;
    }

    int lamp = 0;
    std::optional<ScoreBestSnapshot> score;
    if (input.scoreFor) {
      score = input.scoreFor(record.meta, input.selectedLongNoteMode);
      if (score) lamp = beatorajaClearType(score->clearType);
    }
    const MusicSelectBarId songId{chartIdentity(record)};
    const std::string title = fullTitle(record.meta);
    result.bars.push_back(
        {.id = songId,
         .kind = skin::MusicSelectBarKind::Song,
         .title = title,
         .chart = record,
         .score = std::move(score),
         .presentation = {.kind = skin::MusicSelectBarKind::Song,
                          .title = title,
                          .exists = !record.unavailable &&
                                    !record.meta.BmsPath.empty(),
                          .lamp = lamp,
                          .difficulty = record.meta.Difficulty,
                          .level = static_cast<int>(record.meta.PlayLevel),
                          .featureFlags = songFeatures(record.meta)},
         .selectable = true});
    auto &folderBar = result.bars[folder->second];
    folderBar.children.push_back(songId);
    ++folderBar.presentation.folderLampCounts[static_cast<std::size_t>(lamp)];
  }
  return result;
}
