#include "MusicSelectPhysicalDirectory.h"

#include "MusicSelectPagedSongs.h"
#include "MusicSelectReplaySlots.h"

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace {

void checkCancelled(std::stop_token stop) {
  if (stop.stop_requested()) {
    throw std::runtime_error("Physical directory loading cancelled");
  }
}

}

MusicSelectDirectoryLoader::Content loadMusicSelectPhysicalDirectory(
    ChartRepository &repository, const MusicSelectRepositoryMetadata &metadata,
    const MusicSelectBar &directory, std::shared_ptr<const ScoreBestCache> best,
    std::shared_ptr<const ScoreClearRankCache> clears,
    const std::filesystem::path &replayRoot,
    const MusicSelectBarManagerConfig &config, int selectedLongNoteMode,
    std::stop_token stop) {
  checkCancelled(stop);
  auto opened = repository.OpenSession();
  if (!opened) throw std::runtime_error("Unable to open chart repository session");
  auto session = std::make_shared<ChartRepository::Session>(std::move(*opened));
  checkCancelled(stop);
  if (!session->HasChartMetaForFolderOrParentFolder(directory.directoryPath, stop)) {
    checkCancelled(stop);
    auto children = MusicSelectRepositoryProjection::projectDirectoryFolders(
        metadata, directory.directoryPath);
    checkCancelled(stop);
    return {.children = std::move(children)};
  }

  MusicSelectRepositoryProjectionInput input;
  input.selectedLongNoteMode = selectedLongNoteMode;
  if (best) {
    input.scoreFor = [best = std::move(best)](const bms_parser::ChartMeta &meta,
                                             int longNoteMode) {
      return best->bestFor(meta, longNoteMode);
    };
  }
  if (clears) {
    input.clearFor = [clears = std::move(clears)](const bms_parser::ChartMeta &meta,
                                                 int longNoteMode) {
      return clears->bestRankFor(meta, longNoteMode);
    };
  }
  input.replayExistsFor = [replayRoot](const ChartMetaRecord &record,
                                      int longNoteMode) {
    return musicSelectExistingChartReplaySlots(record, longNoteMode, replayRoot);
  };

  MusicSelectSongIndex index(directory.id.value);
  session->VisitChartMetaSelection(directory.directoryPath,
      [&](const ChartMetaRecord &record) {
        checkCancelled(stop);
        const auto score = input.scoreFor
            ? input.scoreFor(record.meta, selectedLongNoteMode) : std::nullopt;
        const int clearRank = input.clearFor
            ? input.clearFor(record.meta, selectedLongNoteMode)
            : score ? score->clearType : kNoClearTypeRank;
        index.add(record, score, clearRank);
      }, stop);
  index.finish(stop);
  index.configure(config.modeFilter, config.difficultyFilter, config.sortId, stop);
  checkCancelled(stop);
  auto primingStop = std::make_shared<std::stop_token>(stop);
  auto provider = std::make_shared<MusicSelectPagedSongs>(
      std::move(index),
      [session = std::move(session), primingStop](
          std::span<const std::filesystem::path> paths) {
        return session->SelectChartMetaByPaths(paths, *primingStop);
      },
      [context = directory.id.value, input = std::move(input), primingStop](
          const ChartMetaRecord &record) {
        checkCancelled(*primingStop);
        auto bar = MusicSelectRepositoryProjection::projectSong(record, context, input);
        checkCancelled(*primingStop);
        return bar;
      }, config);
  checkCancelled(stop);
  if (provider->size() != 0) {
    for (const auto position : {std::size_t{0}, provider->size() - 1}) {
      checkCancelled(stop);
      (void)provider->at(position);
      checkCancelled(stop);
      if (!provider->diagnostic().empty()) {
        throw std::runtime_error(provider->diagnostic());
      }
    }
  }
  *primingStop = {};
  return {.provider = std::move(provider)};
}

MusicSelectDirectoryLoader::Content loadMusicSelectPhysicalDirectoryAutoplay(
    ChartRepository &repository, const MusicSelectBar &directory,
    int selectedLongNoteMode, std::stop_token stop) {
  checkCancelled(stop);
  auto session = repository.OpenSession();
  if (!session) throw std::runtime_error("Unable to open chart repository session");
  checkCancelled(stop);
  if (!session->HasChartMetaForFolderOrParentFolder(directory.directoryPath, stop)) {
    checkCancelled(stop);
    return {};
  }
  const auto records = MusicSelectRepositoryProjection::loadDirectoryRecords(
      *session, directory, selectedLongNoteMode, nullptr, stop);
  std::unordered_set<std::string_view> hashes;
  std::vector<const ChartMetaRecord *> unique;
  unique.reserve(records.size());
  for (const auto &record : records) {
    checkCancelled(stop);
    if (hashes.insert(record.meta.SHA256).second) unique.push_back(&record);
  }
  MusicSelectDirectoryLoader::Content content;
  content.children.reserve(unique.size());
  for (auto record = unique.rbegin(); record != unique.rend(); ++record) {
    checkCancelled(stop);
    content.children.push_back(MusicSelectRepositoryProjection::projectSong(
        **record, directory.id.value, {}));
  }
  checkCancelled(stop);
  return content;
}
