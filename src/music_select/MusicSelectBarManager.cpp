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

int titleCompare(const MusicSelectBar &left, const MusicSelectBar &right) {
  const auto leftTitle = lowerAscii(
      left.chart ? left.chart->meta.Title : left.title);
  const auto rightTitle = lowerAscii(
      right.chart ? right.chart->meta.Title : right.title);
  if (leftTitle < rightTitle) return -1;
  if (leftTitle > rightTitle) return 1;
  if (left.chart && right.chart) {
    return left.chart->meta.Difficulty - right.chart->meta.Difficulty;
  }
  return 0;
}

int sourceCompare(const MusicSelectBar &left, const MusicSelectBar &right,
                  std::string_view sortId) {
  if (!left.chart || !right.chart) return titleCompare(left, right);
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
    return left.score->badPoints.value_or(left.score->comboBreak.value_or(0)) -
           right.score->badPoints.value_or(right.score->comboBreak.value_or(0));
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

MusicSelectBarManager::MusicSelectBarManager(MusicSelectProjection projection,
                                             MusicSelectBarManagerConfig config)
    : projection_(std::move(projection)), config_(std::move(config)) {
  rebuildRows();
}

const MusicSelectBar *MusicSelectBarManager::selected() const {
  return selectedIndex_ < rows_.size() ? &rows_[selectedIndex_] : nullptr;
}

void MusicSelectBarManager::rebuildRows(
    std::optional<MusicSelectBarId> preferred) {
  rows_.clear();
  const std::vector<MusicSelectBarId> *ids = &projection_.root;
  bool showInvisibleCharts = false;
  if (!directory_.empty()) {
    const auto *directory = projection_.find(directory_.back());
    if (directory != nullptr) {
      ids = &directory->children;
      showInvisibleCharts = directory->showInvisibleCharts;
    }
  }
  rows_.reserve(ids->size());
  for (const auto &id : *ids) {
    if (const auto *bar = projection_.find(id)) rows_.push_back(*bar);
  }
  if (!rows_.empty()) {
    const auto original = rows_;
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
        rows_.clear();
        for (const auto &bar : original) {
          if (!bar.chart ||
              ((showInvisibleCharts ||
                (bar.chart->songReviewFavorite & (4 | 8)) == 0) &&
               modeMatches(mode, songMode(bar.chart->meta)) &&
               difficultyMatches(difficulty, *bar.chart))) {
            rows_.push_back(bar);
          }
        }
        if (!rows_.empty()) {
          config_.modeFilter = mode;
          config_.difficultyFilter = difficulty;
          filtered = true;
          break;
        }
      }
    }
    if (!filtered) rows_ = original;
    if (!directory_.empty()) {
      const auto *directory = projection_.find(directory_.back());
      if (directory && directory->sortable) {
        std::stable_sort(rows_.begin(), rows_.end(), [this](const auto &left,
                                                            const auto &right) {
          return sourceCompare(left, right, config_.sortId) < 0;
        });
      }
    }
  }
  selectedIndex_ = 0;
  if (preferred) {
    const auto found = std::ranges::find(rows_, *preferred,
                                         &MusicSelectBar::id);
    if (found != rows_.end()) {
      selectedIndex_ = static_cast<std::size_t>(found - rows_.begin());
    }
  }
}

bool MusicSelectBarManager::openSelected() {
  const auto *bar = selected();
  if (bar == nullptr || bar->children.empty()) return false;
  const MusicSelectBarId id = bar->id;
  sourceBars_.push_back(id);
  directory_.push_back(id);
  rebuildRows();
  return true;
}

bool MusicSelectBarManager::openTransient(
    MusicSelectBar directory, std::vector<MusicSelectBar> children) {
  const auto *source = selected();
  if (source == nullptr) return false;
  const MusicSelectBarId sourceId = source->id;
  const MusicSelectBarId directoryId = directory.id;
  const bool hasChild = std::ranges::any_of(
      directory.children, [&](const MusicSelectBarId &id) {
        return projection_.find(id) != nullptr ||
               std::ranges::find(children, id, &MusicSelectBar::id) !=
                   children.end();
      });
  if (!hasChild) return false;
  for (auto &child : children) {
    const auto found = std::ranges::find(projection_.bars, child.id,
                                         &MusicSelectBar::id);
    if (found == projection_.bars.end()) {
      projection_.bars.push_back(std::move(child));
    } else {
      *found = std::move(child);
    }
  }
  const auto found = std::ranges::find(projection_.bars, directoryId,
                                       &MusicSelectBar::id);
  if (found == projection_.bars.end()) {
    projection_.bars.push_back(std::move(directory));
  } else {
    *found = std::move(directory);
  }
  sourceBars_.push_back(sourceId);
  directory_.push_back(directoryId);
  rebuildRows();
  return !rows_.empty();
}

bool MusicSelectBarManager::close() {
  if (directory_.empty()) return false;
  const MusicSelectBarId source =
      sourceBars_.empty() ? directory_.back() : sourceBars_.back();
  directory_.pop_back();
  if (!sourceBars_.empty()) sourceBars_.pop_back();
  rebuildRows(source);
  return true;
}

void MusicSelectBarManager::move(bool increase, int movementDirection,
                                 std::int64_t movementEndMillis) {
  if (rows_.empty()) return;
  if (increase) {
    selectedIndex_ = (selectedIndex_ + 1) % rows_.size();
  } else {
    selectedIndex_ = (selectedIndex_ + rows_.size() - 1) % rows_.size();
  }
  movementDirection_ = movementDirection;
  movementEndMillis_ = movementEndMillis;
}

bool MusicSelectBarManager::select(const MusicSelectBarId &id) {
  const auto found = std::ranges::find(rows_, id, &MusicSelectBar::id);
  if (found == rows_.end()) return false;
  selectedIndex_ = static_cast<std::size_t>(found - rows_.begin());
  return true;
}

void MusicSelectBarManager::setSelectedPosition(float value) {
  if (rows_.empty()) return;
  if (value >= 0.0F && value < 1.0F) {
    selectedIndex_ = static_cast<std::size_t>(rows_.size() * value);
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
  projection_ = std::move(projection);
  for (std::size_t index = 0; index < directory_.size(); ++index) {
    const auto *bar = projection_.find(directory_[index]);
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
  for (const auto &id : directory_) {
    if (const auto *bar = projection_.find(id)) {
      directoryText += bar->title + " > ";
    }
  }
  return {.rows = rows_,
          .selectedIndex = selectedIndex_,
          .directory = directory_,
          .directoryText = std::move(directoryText),
          .movementDirection = movementDirection_,
          .movementEndMillis = movementEndMillis_,
          .resolvedModeFilter = config_.modeFilter,
          .resolvedDifficultyFilter = config_.difficultyFilter};
}
