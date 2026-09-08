#include "MusicSelectPagedSongs.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

MusicSelectPagedSongs::MusicSelectPagedSongs(
    MusicSelectSongIndex index, RecordLoader loadRecords, Projector project,
    MusicSelectBarManagerConfig config)
    : index_(std::move(index)), loadRecords_(std::move(loadRecords)),
      project_(std::move(project)), config_(std::move(config)) {
  resolved_ = index_.configure(config_.modeFilter, config_.difficultyFilter,
                               config_.sortId);
  resetPages();
}

std::size_t MusicSelectPagedSongs::size() const noexcept {
  return index_.size();
}

std::shared_ptr<MusicSelectRowProvider> MusicSelectPagedSongs::clone() const {
  return std::make_shared<MusicSelectPagedSongs>(index_, loadRecords_, project_,
                                                 config_);
}

const MusicSelectBar &MusicSelectPagedSongs::at(std::size_t index) const {
  if (index >= size()) throw std::out_of_range("music-select row index");
  return pages_.get(index);
}

std::optional<std::size_t>
MusicSelectPagedSongs::indexOf(const MusicSelectBarId &id) const {
  return index_.indexOf(id);
}

std::pair<std::string, std::string> MusicSelectPagedSongs::configure(
    const std::string &mode, const std::string &difficulty,
    const std::string &sort) {
  if (config_.sortId == sort &&
      ((config_.modeFilter == mode && config_.difficultyFilter == difficulty) ||
       (resolved_.first == mode && resolved_.second == difficulty))) {
    return resolved_;
  }
  resolved_ = index_.configure(mode, difficulty, sort);
  config_ = {mode, difficulty, sort};
  resetPages();
  return resolved_;
}

const std::string &MusicSelectPagedSongs::diagnostic() const noexcept {
  return diagnostic_;
}

void MusicSelectPagedSongs::retryFailedPages() {
  if (!diagnostic_.empty()) resetPages();
}

void MusicSelectPagedSongs::resetPages() {
  diagnostic_.clear();
  pages_.reset(size(), [this](std::size_t offset, std::size_t limit) {
    return loadPage(offset, limit);
  });
}

MusicSelectBar MusicSelectPagedSongs::unavailableBar(std::size_t index) const {
  return {.id = index_.idAt(index),
          .kind = skin::MusicSelectBarKind::Song,
          .title = index_.titleAt(index),
          .presentation = {.kind = skin::MusicSelectBarKind::Song,
                           .title = index_.titleAt(index),
                           .exists = false}};
}

std::vector<MusicSelectBar> MusicSelectPagedSongs::loadPage(
    std::size_t offset, std::size_t limit) const {
  const auto count = std::min(limit, size() - offset);
  std::vector<std::filesystem::path> paths;
  paths.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    paths.push_back(index_.pathAt(offset + index));
  }
  ChartMetaPathBatchReadOutcome loaded;
  try {
    loaded = loadRecords_(paths);
  } catch (const std::exception &error) {
    loaded.diagnostic = error.what();
  }
  const bool success = loaded.status == ChartMetaPathBatchReadStatus::Loaded;
  if (!success) {
    diagnostic_ = loaded.diagnostic.empty() ? "Unable to load chart page."
                                            : loaded.diagnostic;
  }
  std::unordered_map<std::filesystem::path, const ChartMetaRecord *> records;
  if (success) {
    records.reserve(loaded.records.size());
    for (const auto &record : loaded.records) {
      records.try_emplace(record.meta.BmsPath, &record);
    }
  }
  std::vector<MusicSelectBar> result;
  result.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto found = records.find(paths[index]);
    if (found == records.end()) {
      result.push_back(unavailableBar(offset + index));
      continue;
    }
    auto bar = project_(*found->second);
    if (bar.id != index_.idAt(offset + index)) {
      diagnostic_ = "The library changed while loading this folder.";
      result.push_back(unavailableBar(offset + index));
    } else {
      result.push_back(std::move(bar));
    }
  }
  return result;
}
