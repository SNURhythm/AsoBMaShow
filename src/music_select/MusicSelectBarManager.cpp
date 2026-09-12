#include "MusicSelectBarManager.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <compare>
#include <ranges>
#include <string_view>
#include <utility>

namespace {

constexpr std::array<std::string_view, 10> kModeFilters{
    "ALL", "7KEY", "14KEY", "9KEY", "5KEY",
    "10KEY", "24KEY", "48KEY", "SINGLE", "DOUBLE"};
constexpr std::array<std::string_view, 9> kDifficultyFilters{
    "ALL", "BEGINNER", "NORMAL", "HYPER", "ANOTHER", "INSANE",
    "SCRATCH CHART", "LONG NOTE CHART", "SPEED CHANGE CHART"};

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

bool difficultyMatches(std::string_view filter,
                       const ChartMetaRecord &record) {
  const auto &meta = record.meta;
  if (filter == "ALL") return true;
  if (filter == "SCRATCH CHART") {
    return meta.TotalNotes > 0 &&
           (meta.TotalScratchNotes + meta.TotalBackSpinNotes) * 8 >=
               meta.TotalNotes;
  }
  if (filter == "LONG NOTE CHART") {
    return meta.TotalNotes > 0 &&
           (meta.TotalLongNotes + meta.TotalBackSpinNotes) * 20 >=
               meta.TotalNotes;
  }
  if (filter == "SPEED CHANGE CHART") {
    return meta.MinBpm != meta.MaxBpm || record.hasScrollChange ||
           record.hasBpmStop;
  }
  // Pinned Beatoraja DifficultyFilter.closestMatch deliberately ignores
  // SongData.difficulty here and chooses the nearest NOTES profile.
  constexpr std::array<std::pair<std::string_view, int>, 5> profiles{{
      {"BEGINNER", 0}, {"NORMAL", 500}, {"HYPER", 700},
      {"ANOTHER", 1300}, {"INSANE", 2700},
  }};
  std::string_view closest = profiles.front().first;
  int closestDistance = std::abs(meta.TotalNotes - profiles.front().second);
  for (const auto &[name, target] : profiles) {
    const int distance = std::abs(meta.TotalNotes - target);
    if (distance <= closestDistance) {
      closest = name;
      closestDistance = distance;
    }
  }
  return filter == closest;
}

std::string lowerAscii(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return result;
}

bool titleSortParticipates(const MusicSelectBar &bar) {
  return bar.kind == skin::MusicSelectBarKind::Song ||
         bar.kind == skin::MusicSelectBarKind::Folder;
}

int titleCompare(const MusicSelectBar &left, const MusicSelectBar &right) {
  const bool leftParticipates = titleSortParticipates(left);
  const bool rightParticipates = titleSortParticipates(right);
  if (!leftParticipates && !rightParticipates) return 0;
  if (!leftParticipates) return 1;
  if (!rightParticipates) return -1;

  const bool songs = left.kind == skin::MusicSelectBarKind::Song &&
                     right.kind == skin::MusicSelectBarKind::Song;
  const auto leftTitle = lowerAscii(
      songs && left.chart ? left.chart->meta.Title : left.title);
  const auto rightTitle = lowerAscii(
      songs && right.chart ? right.chart->meta.Title : right.title);
  if (leftTitle < rightTitle) return -1;
  if (leftTitle > rightTitle) return 1;
  if (songs && left.chart && right.chart) {
    return left.chart->meta.Difficulty - right.chart->meta.Difficulty;
  }
  return 0;
}

int sourceCompare(const MusicSelectBar &left, const MusicSelectBar &right,
                  std::string_view sortId) {
  if (sortId == "TITLE" || !left.chart || !right.chart) {
    return titleCompare(left, right);
  }
  if ((sortId == "ARTIST" || sortId == "BPM" || sortId == "LENGTH" ||
       sortId == "LEVEL") &&
      left.kind == skin::MusicSelectBarKind::Song &&
      right.kind == skin::MusicSelectBarKind::Song) {
    if (!left.presentation.exists && !right.presentation.exists) return 0;
    if (!left.presentation.exists) return 1;
    if (!right.presentation.exists) return -1;
  }
  const auto &a = left.chart->meta;
  const auto &b = right.chart->meta;
  if (sortId == "ARTIST") {
    const auto aa = lowerAscii(a.Artist);
    const auto bb = lowerAscii(b.Artist);
    return aa < bb ? -1 : aa > bb ? 1 : 0;
  }
  if (sortId == "BPM") return a.MaxBpm < b.MaxBpm ? -1 : a.MaxBpm > b.MaxBpm;
  if (sortId == "LENGTH") return a.PlayLength < b.PlayLength ? -1 : a.PlayLength > b.PlayLength;
  if (sortId == "LEVEL") {
    if (a.PlayLevel != b.PlayLevel) return a.PlayLevel < b.PlayLevel ? -1 : 1;
    return a.Difficulty - b.Difficulty;
  }
  if (sortId == "CLEAR") {
    if (!left.score && !right.score) return 0;
    if (!left.score) return 1;
    if (!right.score) return -1;
    return left.presentation.lamp - right.presentation.lamp;
  }
  if (sortId == "SCORE") {
    const int leftNotes = left.score ? left.score->maxScore / 2 : 0;
    const int rightNotes = right.score ? right.score->maxScore / 2 : 0;
    if (leftNotes == 0 && rightNotes == 0) return 0;
    if (leftNotes == 0) return 1;
    if (rightNotes == 0) return -1;
    const double la = static_cast<double>(left.score->score) / leftNotes;
    const double rb = static_cast<double>(right.score->score) / rightNotes;
    return la < rb ? -1 : la > rb;
  }
  if (sortId == "MISSCOUNT") {
    if (!left.score && !right.score) return 0;
    if (!left.score) return 1;
    if (!right.score) return -1;
    return left.score->badPoints.value_or(0) -
           right.score->badPoints.value_or(0);
  }
  if (sortId == "DURATION") {
    const bool leftHasDuration =
        left.score && left.score->averageJudgeMicros.has_value();
    const bool rightHasDuration =
        right.score && right.score->averageJudgeMicros.has_value();
    if (!leftHasDuration && !rightHasDuration) return 0;
    if (!leftHasDuration) return 1;
    if (!rightHasDuration) return -1;
    const std::uint32_t narrowed = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(*left.score->averageJudgeMicros) -
        static_cast<std::uint64_t>(*right.score->averageJudgeMicros));
    return std::bit_cast<std::int32_t>(narrowed);
  }
  if (sortId == "LASTUPDATE") {
    if (!left.score && !right.score) return 0;
    if (!left.score) return 1;
    if (!right.score) return -1;
    return left.score->lastPlayedUnixSeconds.value_or(0) <
                   right.score->lastPlayedUnixSeconds.value_or(0)
               ? -1
               : left.score->lastPlayedUnixSeconds.value_or(0) >
                         right.score->lastPlayedUnixSeconds.value_or(0);
  }
  if (sortId == "RIVALCOMPARE_CLEAR") {
    if ((!left.score || !left.rivalScore) &&
        (!right.score || !right.rivalScore)) return 0;
    if (!left.score || !left.rivalScore) return 1;
    if (!right.score || !right.rivalScore) return -1;
    return (left.presentation.lamp - left.presentation.rivalLamp) -
           (right.presentation.lamp - right.presentation.rivalLamp);
  }
  if (sortId == "RIVALCOMPARE_SCORE") {
    const int leftNotes = left.score ? left.score->maxScore / 2 : 0;
    const int rightNotes = right.score ? right.score->maxScore / 2 : 0;
    const int leftRivalNotes =
        left.rivalScore ? left.rivalScore->maxScore / 2 : 0;
    const int rightRivalNotes =
        right.rivalScore ? right.rivalScore->maxScore / 2 : 0;
    if ((leftNotes == 0 || leftRivalNotes == 0) &&
        (rightNotes == 0 || rightRivalNotes == 0)) return 0;
    if (leftNotes == 0 || leftRivalNotes == 0) return 1;
    if (rightNotes == 0 || rightRivalNotes == 0) return -1;
    const double la = static_cast<double>(left.score->score) / leftNotes -
                      static_cast<double>(left.rivalScore->score) /
                          leftRivalNotes;
    const double rb = static_cast<double>(right.score->score) / rightNotes -
                      static_cast<double>(right.rivalScore->score) /
                          rightRivalNotes;
    return la < rb ? -1 : la > rb;
  }
  return titleCompare(left, right);
}

std::size_t startIndex(const auto &values, std::string_view selected) {
  const auto found = std::ranges::find(values, selected);
  return static_cast<std::size_t>(std::distance(values.begin(), found));
}

} // namespace

int musicSelectFirstExistingReplay(const MusicSelectBar *bar) noexcept {
  if (bar != nullptr && bar->selectable) {
    for (std::size_t index = 0; index < bar->replayExists.size(); ++index) {
      if (bar->replayExists[index]) return static_cast<int>(index);
    }
  }
  return -1;
}

int musicSelectNextExistingReplay(const MusicSelectBar *bar,
                                  int selected) noexcept {
  if (bar != nullptr && bar->selectable) {
    for (int offset = 1;
         offset < static_cast<int>(bar->replayExists.size()); ++offset) {
      const int index =
          (offset + selected) % static_cast<int>(bar->replayExists.size());
      if (index >= 0 && bar->replayExists[static_cast<std::size_t>(index)]) {
        return index;
      }
    }
  }
  return selected;
}

std::string musicSelectSelectedHash(const MusicSelectBar *bar, bool sha256) {
  if (bar == nullptr || bar->kind != skin::MusicSelectBarKind::Song ||
      !bar->chart) {
    return {};
  }
  return sha256 ? bar->chart->meta.SHA256 : bar->chart->meta.MD5;
}

MusicSelectTableContext musicSelectTableContextForLaunch(
    const MusicSelectBarManagerReadView &snapshot) {
  MusicSelectTableContext result;
  bool importedTable = false;
  for (const auto &bar : snapshot.directoryBars) {
    if (bar.kind == skin::MusicSelectBarKind::Table) {
      importedTable = bar.tableId > 0;
      result.name = importedTable ? bar.title : std::string{};
      result.level.clear();
    } else if (importedTable &&
               bar.kind == skin::MusicSelectBarKind::Hash) {
      result.level = bar.title;
      break;
    }
  }
  result.fullName = result.level + result.name;
  return result;
}

MusicSelectBarManager::MusicSelectBarManager(MusicSelectProjection projection,
                                             MusicSelectBarManagerConfig config)
    : projection_(std::move(projection)), config_(std::move(config)) {
  rebuildProjectionIndex();
  rebuildRows();
}

std::size_t MusicSelectBarManager::rowCount() const noexcept {
  return rowProvider_ ? rowProvider_->size() : rows_->size();
}

const MusicSelectBar *MusicSelectBarManager::selected() const {
  if (selectedIndex_ >= rowCount()) return nullptr;
  return rowProvider_ ? &rowProvider_->at(selectedIndex_)
                      : &(*rows_)[selectedIndex_];
}

void MusicSelectBarManager::rebuildRows(
    std::optional<MusicSelectBarId> preferred, std::size_t fallbackIndex) {
  ++rowsRevision_;
  rowProvider_.reset();
  rowIndex_.clear();
  rows_ = std::make_shared<std::vector<MusicSelectBar>>();
  directoryBars_ = std::make_shared<std::vector<MusicSelectBar>>();
  directoryText_.clear();
  for (const auto &id : directory_) {
    if (const auto *bar = find(id)) {
      directoryBars_->push_back(*bar);
      directoryText_ += bar->title + " > ";
    }
  }
  if (!directory_.empty()) {
    const auto provider = rowProviders_.find(directory_.back().value);
    if (provider != rowProviders_.end()) {
      auto &state = provider->second;
      if (state.configuration &&
          (state.configuration->modeFilter != config_.modeFilter ||
           state.configuration->difficultyFilter != config_.difficultyFilter ||
           state.configuration->sortId != config_.sortId)) {
        state.provider = state.provider->clone();
      }
      rowProvider_ = state.provider;
      auto resolved = rowProvider_->configure(
          config_.modeFilter, config_.difficultyFilter, config_.sortId);
      config_.modeFilter = std::move(resolved.first);
      config_.difficultyFilter = std::move(resolved.second);
      state.configuration = config_;
      selectedIndex_ = rowCount() == 0 ? 0 : std::min(fallbackIndex, rowCount() - 1);
      if (preferred) {
        if (const auto index = rowProvider_->indexOf(*preferred);
            index && *index < rowCount()) {
          selectedIndex_ = *index;
        }
      }
      return;
    }
  }
  const std::vector<MusicSelectBarId> *ids = &projection_.root;
  bool showInvisibleCharts = false;
  bool sortable = true;
  if (!directory_.empty()) {
    const auto *directory = find(directory_.back());
    if (directory != nullptr) {
      ids = &directory->children;
      showInvisibleCharts = directory->showInvisibleCharts;
      sortable = directory->sortable;
    }
  }
  (*rows_).reserve(ids->size());
  for (const auto &id : *ids) {
    if (const auto *bar = find(id)) (*rows_).push_back(*bar);
  }
  if (!(*rows_).empty()) {
    const auto original = (*rows_);
    const std::size_t modeStart = startIndex(kModeFilters, config_.modeFilter);
    const std::size_t difficultyStart =
        startIndex(kDifficultyFilters, config_.difficultyFilter);
    bool filtered = false;
    for (std::size_t difficultyTrial = 0;
         difficultyTrial < kDifficultyFilters.size() && !filtered;
         ++difficultyTrial) {
      const auto difficulty = kDifficultyFilters[
          (difficultyStart + difficultyTrial) % kDifficultyFilters.size()];
      for (std::size_t modeTrial = 0; modeTrial < kModeFilters.size();
           ++modeTrial) {
        const auto mode =
            kModeFilters[(modeStart + modeTrial) % kModeFilters.size()];
        (*rows_).clear();
        for (const auto &bar : original) {
          if (!bar.chart || bar.chart->solidArchive ||
              ((showInvisibleCharts ||
                (bar.chart->songReviewFavorite & (4 | 8)) == 0) &&
               modeMatches(mode, songMode(bar.chart->meta)) &&
               difficultyMatches(difficulty, *bar.chart))) {
            (*rows_).push_back(bar);
          }
        }
        if (!(*rows_).empty()) {
          config_.modeFilter = mode;
          config_.difficultyFilter = difficulty;
          filtered = true;
          break;
        }
      }
    }
    if (!filtered) (*rows_) = original;
    if (sortable) {
      std::stable_sort((*rows_).begin(), (*rows_).end(), [this](const auto &left,
                                                          const auto &right) {
        return sourceCompare(left, right, config_.sortId) < 0;
      });
    }
  }
  rowIndex_.clear();
  rowIndex_.reserve(rows_->size());
  for (std::size_t index = 0; index < rows_->size(); ++index) {
    rowIndex_.try_emplace((*rows_)[index].id.value, index);
  }
  selectedIndex_ = 0;
  if (preferred) {
    const auto found = std::ranges::find((*rows_), *preferred,
                                         &MusicSelectBar::id);
    if (found != (*rows_).end()) {
      selectedIndex_ = static_cast<std::size_t>(found - (*rows_).begin());
    }
  }
}

bool MusicSelectBarManager::open(const MusicSelectBarId &id) {
  const MusicSelectBarId directoryId = id;
  const auto *bar = find(directoryId);
  if (bar == nullptr || !skin::musicSelectIsDirectoryBarKind(bar->kind)) {
    return false;
  }
  const auto *source = selected();
  if (source == nullptr) return false;
  const MusicSelectBarId sourceId = source->id;
  const bool hasProvider = rowProviders_.contains(directoryId.value);
  if (!hasProvider && bar->children.empty()) {
    rebuildRows(sourceId);
    return false;
  }
  sourceBars_.push_back(sourceId);
  directory_.push_back(directoryId);
  rebuildRows();
  if (hasProvider && rowCount() == 0) {
    static_cast<void>(close());
    return false;
  }
  return true;
}

bool MusicSelectBarManager::openSelected() {
  const auto *bar = selected();
  if (bar == nullptr) return false;
  const auto id = bar->id;
  return open(id);
}

bool MusicSelectBarManager::installRowProvider(
    const MusicSelectBarId &directory,
    std::shared_ptr<MusicSelectRowProvider> provider) {
  const auto *parent = find(directory);
  if (!provider || !parent ||
      !skin::musicSelectIsDirectoryBarKind(parent->kind)) return false;
  const bool active = !directory_.empty() && directory_.back() == directory;
  const auto previousIndex = selectedIndex_;
  std::optional<MusicSelectBarId> preferred;
  if (active) {
    if (const auto *bar = selected()) preferred = bar->id;
  }
  auto &state = rowProviders_[directory.value];
  if (state.provider != provider) {
    state = {.provider = std::move(provider)};
  }
  projection_.bars[projectionIndex_.at(directory.value)].childrenLoaded = true;
  if (active) {
    rebuildRows(preferred, previousIndex);
  }
  return true;
}

void MusicSelectBarManager::installFolderStatus(
    const MusicSelectBarId &id, const skin::MusicSelectBarFrame &frame) {
  const auto update = [&](MusicSelectBar &bar) {
    if (!skin::musicSelectIsDirectoryBarKind(bar.kind)) return;
    bar.presentation.folderLampCounts = frame.folderLampCounts;
    bar.presentation.folderRankCounts = frame.folderRankCounts;
    bar.presentation.lamp = frame.lamp;
    bar.presentation.rivalLamp = frame.rivalLamp;
  };
  if (const auto found = projectionIndex_.find(id.value);
      found != projectionIndex_.end()) {
    update(projection_.bars[found->second]);
  }
  if (const auto found = rowIndex_.find(id.value); found != rowIndex_.end()) {
    if (rows_.use_count() != 1) {
      rows_ = std::make_shared<std::vector<MusicSelectBar>>(*rows_);
    }
    update((*rows_)[found->second]);
  }
  for (std::size_t index = 0; index < directoryBars_->size(); ++index) {
    if ((*directoryBars_)[index].id != id) continue;
    if (directoryBars_.use_count() != 1) {
      directoryBars_ =
          std::make_shared<std::vector<MusicSelectBar>>(*directoryBars_);
    }
    update((*directoryBars_)[index]);
  }
}

bool MusicSelectBarManager::installChildren(
    const MusicSelectBarId &directory, std::vector<MusicSelectBar> children) {
  const auto *parent = find(directory);
  if (!parent || !skin::musicSelectIsDirectoryBarKind(parent->kind)) return false;
  const auto parentIndex = projectionIndex_.at(directory.value);
  const bool replacingProvider = rowProviders_.erase(directory.value) != 0;
  const bool active = !directory_.empty() && directory_.back() == directory;
  std::optional<MusicSelectBarId> preferred;
  if (replacingProvider && active) {
    if (const auto *bar = selected()) preferred = bar->id;
  }
  std::vector<MusicSelectBarId> ids;
  ids.reserve(children.size());
  for (auto &child : children) {
    ids.push_back(child.id);
    const auto [found, inserted] =
        projectionIndex_.try_emplace(child.id.value, projection_.bars.size());
    if (inserted) {
      projection_.bars.push_back(std::move(child));
    } else {
      projection_.bars[found->second] = std::move(child);
    }
  }
  projection_.bars[parentIndex].children = std::move(ids);
  projection_.bars[parentIndex].childrenLoaded = true;
  if (replacingProvider && active) rebuildRows(preferred);
  return true;
}

bool MusicSelectBarManager::openTransient(
    MusicSelectBar directory, std::vector<MusicSelectBar> children) {
  const auto *source = selected();
  if (source == nullptr) return false;
  const MusicSelectBarId sourceId = source->id;
  const MusicSelectBarId directoryId = directory.id;
  std::unordered_map<std::string, bool> childIds;
  for (const auto &child : children) childIds.emplace(child.id.value, true);
  const bool hasChild = std::ranges::any_of(
      directory.children, [&](const MusicSelectBarId &id) {
        return find(id) != nullptr || childIds.contains(id.value);
      });
  if (!hasChild) return false;
  const auto install = [&](MusicSelectBar bar) {
    const auto [found, inserted] =
        projectionIndex_.try_emplace(bar.id.value, projection_.bars.size());
    if (inserted) {
      projection_.bars.push_back(std::move(bar));
    } else {
      projection_.bars[found->second] = std::move(bar);
    }
  };
  for (auto &child : children) install(std::move(child));
  install(std::move(directory));
  rowProviders_.erase(directoryId.value);
  sourceBars_.push_back(sourceId);
  directory_.push_back(directoryId);
  rebuildRows();
  return rowCount() != 0;
}

bool MusicSelectBarManager::close() {
  if (directory_.empty()) return false;
  const MusicSelectBarId source =
      sourceBars_.empty() ? directory_.back() : sourceBars_.back();
  if (rowProviders_.erase(directory_.back().value) != 0) {
    const auto found = projectionIndex_.find(directory_.back().value);
    if (found != projectionIndex_.end()) {
      projection_.bars[found->second].childrenLoaded = false;
    }
  }
  directory_.pop_back();
  if (!sourceBars_.empty()) sourceBars_.pop_back();
  rebuildRows(source);
  return true;
}

void MusicSelectBarManager::move(bool increase, int movementDirection,
                                 std::int64_t movementEndMillis) {
  const auto count = rowCount();
  if (count == 0) return;
  if (increase) {
    selectedIndex_ = (selectedIndex_ + 1) % count;
  } else {
    selectedIndex_ = (selectedIndex_ + count - 1) % count;
  }
  movementDirection_ = movementDirection;
  movementEndMillis_ = movementEndMillis;
}

bool MusicSelectBarManager::select(const MusicSelectBarId &id) {
  if (rowProvider_) {
    const auto index = rowProvider_->indexOf(id);
    if (!index || *index >= rowCount()) return false;
    selectedIndex_ = *index;
    return true;
  }
  const auto found = rowIndex_.find(id.value);
  if (found == rowIndex_.end()) return false;
  selectedIndex_ = found->second;
  return true;
}

std::vector<MusicSelectBar>
MusicSelectBarManager::childrenOf(const MusicSelectBarId &id) const {
  std::vector<MusicSelectBar> result;
  if (const auto found = rowProviders_.find(id.value);
      found != rowProviders_.end()) {
    const auto provider = found->second.provider;
    const auto count = provider->size();
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      result.push_back(provider->at(index));
    }
    return result;
  }
  const auto *directory = find(id);
  if (directory == nullptr) return result;
  result.reserve(directory->children.size());
  for (const auto &child : directory->children) {
    if (const auto *bar = find(child)) result.push_back(*bar);
  }
  return result;
}

void MusicSelectBarManager::setSelectedPosition(float value) {
  const auto count = rowCount();
  if (count == 0) return;
  if (value >= 0.0F && value < 1.0F) {
    selectedIndex_ = static_cast<std::size_t>(count * value);
  }
}

void MusicSelectBarManager::configure(MusicSelectBarManagerConfig config) {
  std::optional<MusicSelectBarId> preferred;
  if (const auto *bar = selected()) preferred = bar->id;
  config_ = std::move(config);
  rebuildRows(preferred);
}

void MusicSelectBarManager::refresh(MusicSelectProjection projection) {
  std::optional<MusicSelectBarId> preferred;
  if (const auto *bar = selected()) preferred = bar->id;
  rowProvider_.reset();
  rowProviders_.clear();
  projection_ = std::move(projection);
  rebuildProjectionIndex();
  for (std::size_t index = 0; index < directory_.size(); ++index) {
    const auto *bar = find(directory_[index]);
    if (bar == nullptr || bar->children.empty()) {
      directory_.resize(index);
      sourceBars_.resize(std::min(sourceBars_.size(), index));
      break;
    }
  }
  rebuildRows(preferred);
}

MusicSelectBarManagerSnapshot MusicSelectBarManager::snapshot() const {
  std::string directoryText;
  std::vector<MusicSelectBar> directoryBars;
  for (const auto &id : directory_) {
    if (const auto *bar = find(id)) {
      directoryText += bar->title + " > ";
      directoryBars.push_back(*bar);
    }
  }
  return {.rows = (*rows_),
          .selectedIndex = selectedIndex_,
          .directory = directory_,
          .directoryBars = std::move(directoryBars),
          .directoryText = std::move(directoryText),
          .movementDirection = movementDirection_,
          .movementEndMillis = movementEndMillis_,
          .resolvedModeFilter = config_.modeFilter,
          .resolvedDifficultyFilter = config_.difficultyFilter,
          .rowsRevision = rowsRevision_,
          .rowProvider = rowProvider_};
}

void MusicSelectBarManager::rebuildProjectionIndex() {
  projectionIndex_.clear();
  projectionIndex_.reserve(projection_.bars.size());
  for (std::size_t index = 0; index < projection_.bars.size(); ++index) {
    projectionIndex_.try_emplace(projection_.bars[index].id.value, index);
  }
}

const MusicSelectBar *MusicSelectBarManager::find(
    const MusicSelectBarId &id) const {
  const auto found = projectionIndex_.find(id.value);
  return found == projectionIndex_.end() ? nullptr
                                        : &projection_.bars[found->second];
}

MusicSelectBarManagerReadView MusicSelectBarManager::readView() const {
  return {.rows = *rows_,
          .selectedIndex = selectedIndex_,
          .directory = directory_,
          .directoryBars = *directoryBars_,
          .directoryText = directoryText_,
          .movementDirection = movementDirection_,
          .movementEndMillis = movementEndMillis_,
          .resolvedModeFilter = config_.modeFilter,
          .resolvedDifficultyFilter = config_.difficultyFilter,
          .rowOwner = rows_,
          .directoryOwner = directoryBars_,
          .rowsRevision = rowsRevision_,
          .rowProvider = rowProvider_};
}

MusicSelectTableContext musicSelectTableContextForLaunch(
    const MusicSelectBarManagerSnapshot &snapshot) {
  return musicSelectTableContextForLaunch(
      MusicSelectBarManagerReadView{.directoryBars = snapshot.directoryBars});
}

skin::MusicSelectSongListFrame MusicSelectBarManager::songListFrame() const {
  skin::MusicSelectSongListFrame result;
  result.selectedIndex = selectedIndex_;
  result.movementDirection = movementDirection_;
  result.movementEndMillis = movementEndMillis_;
  result.indexedBars = rows_;
  result.rowProvider = rowProvider_;
  return result;
}

std::vector<MusicSelectBar> musicSelectProjectionChildren(
    const MusicSelectProjection &projection, const MusicSelectBarId &directory) {
  std::unordered_map<std::string, std::size_t> indexes;
  indexes.reserve(projection.bars.size());
  for (std::size_t index = 0; index < projection.bars.size(); ++index) {
    indexes.try_emplace(projection.bars[index].id.value, index);
  }
  const auto parent = indexes.find(directory.value);
  if (parent == indexes.end()) return {};
  const auto &children = projection.bars[parent->second].children;
  std::vector<MusicSelectBar> result;
  result.reserve(children.size());
  for (const auto &id : children) {
    const auto found = indexes.find(id.value);
    if (found != indexes.end()) {
      result.push_back(projection.bars[found->second]);
    }
  }
  return result;
}
