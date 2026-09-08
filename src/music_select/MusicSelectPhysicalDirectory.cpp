#include "MusicSelectPhysicalDirectory.h"

#include "MusicSelectSqlSongs.h"
#include "MusicSelectReplaySlots.h"
#include "MusicSelectSongIndex.h"

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace {

void checkCancelled(std::stop_token stop) {
  if (stop.stop_requested()) {
    throw std::runtime_error("Physical directory loading cancelled");
  }
}

MusicSelectSqlSongs::ResolvedQuery resolveLegacyDurationQuery(
    const std::shared_ptr<ChartRepository::Session> &session,
    const ChartSelectorQuery &query, const std::string &context,
    const std::shared_ptr<std::stop_token> &stop) {
  auto index = std::make_shared<MusicSelectSongIndex>(context);
  session->VisitChartMetaSelection(query.recursiveFolder,
      [&](const ChartMetaRecord &record) {
        checkCancelled(*stop);
        const auto score = query.best
            ? query.best->bestFor(record.meta, query.selectedLongNoteMode)
            : std::nullopt;
        const int clearRank = query.clears
            ? query.clears->bestRankFor(record.meta, query.selectedLongNoteMode)
            : score ? score->clearType : kNoClearTypeRank;
        index->add(record, score, clearRank);
      }, *stop, query.keyword);
  index->finish(*stop);
  const auto filters = index->configure(query.modeFilter, query.difficultyFilter,
                                         query.sortId, *stop);
  std::shared_ptr<const MusicSelectSongIndex> rows = std::move(index);
  return {.count = rows->size(),
          .resolvedFilters = filters,
          .loadPage = [session, rows, stop, context](std::size_t offset, std::size_t limit) {
            std::vector<std::filesystem::path> paths;
            for (std::size_t position = offset;
                 position < rows->size() && paths.size() < limit; ++position) {
              checkCancelled(*stop);
              paths.push_back(rows->pathAt(position));
            }
            auto loaded = session->SelectChartMetaByPaths(paths, *stop);
            if (loaded.status != ChartMetaPathBatchReadStatus::Loaded) {
              throw std::runtime_error(loaded.diagnostic.empty()
                  ? "Unable to load chart page." : loaded.diagnostic);
            }
            for (std::size_t position = 0; position < loaded.records.size(); ++position) {
              checkCancelled(*stop);
              const auto &meta = loaded.records[position].meta;
              const auto identity = !meta.SHA256.empty() ? "sha256:" + meta.SHA256
                  : !meta.MD5.empty() ? "md5:" + meta.MD5
                  : "path:" + fspath_to_utf8(meta.BmsPath.lexically_normal());
              if (rows->idAt(offset + position).value != context + ":" + identity) {
                throw std::runtime_error("The library changed while loading this folder.");
              }
            }
            return std::move(loaded.records);
          },
          .findIndex = [rows, context](std::string_view identity) {
            return rows->indexOf({context + ":" + std::string(identity)});
          }};
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
  const bool search = directory.kind == skin::MusicSelectBarKind::SearchWord;
  constexpr std::string_view searchPrefix = "search:";
  if (search && (!directory.id.value.starts_with(searchPrefix) ||
                 directory.id.value.size() == searchPrefix.size())) {
    throw std::runtime_error("Invalid search directory");
  }
  if (!search && !session->HasChartMetaForFolderOrParentFolder(directory.directoryPath, stop)) {
    checkCancelled(stop);
    auto children = MusicSelectRepositoryProjection::projectDirectoryFolders(
        metadata, directory.directoryPath);
    checkCancelled(stop);
    return {.children = std::move(children)};
  }

  ChartSelectorQuery selectorQuery{
      .recursiveFolder = search ? std::filesystem::path{} : directory.directoryPath,
      .keyword = search ? directory.id.value.substr(searchPrefix.size()) : std::string{},
      .selectedLongNoteMode = selectedLongNoteMode,
      .best = best,
      .clears = clears};
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

  auto primingStop = std::make_shared<std::stop_token>(stop);
  auto provider = std::make_shared<MusicSelectSqlSongs>(
      directory.id.value,
      [session = std::move(session), selectorQuery, primingStop,
       context = directory.id.value](
          const MusicSelectBarManagerConfig &requested) {
        auto query = selectorQuery;
        query.modeFilter = requested.modeFilter;
        query.difficultyFilter = requested.difficultyFilter;
        query.sortId = requested.sortId;
        std::size_t count;
        try {
          count = session->ResolveChartSelectorQuery(query, *primingStop);
        } catch (const ChartSelectorDurationCompatibilityRequired &) {
          return resolveLegacyDurationQuery(session, query, context, primingStop);
        }
        return MusicSelectSqlSongs::ResolvedQuery{
            .count = count,
            .resolvedFilters = {query.modeFilter, query.difficultyFilter},
            .loadPage = [session, query, primingStop](std::size_t offset,
                                                     std::size_t limit) {
              return session->SelectChartSelectorPage(query, offset, limit,
                                                       *primingStop);
            },
            .findIndex = [session, query, primingStop](std::string_view identity) {
              return session->FindChartSelectorIndex(query, identity, *primingStop);
            }};
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
  if (directory.kind != skin::MusicSelectBarKind::SearchWord &&
      !session->HasChartMetaForFolderOrParentFolder(directory.directoryPath, stop)) {
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
