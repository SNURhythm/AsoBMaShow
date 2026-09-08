#include "MusicSelectSongIndex.h"

#include "../path.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace {

constexpr std::array<std::string_view, 10> kModes{
    "ALL", "7KEY", "14KEY", "9KEY", "5KEY",
    "10KEY", "24KEY", "48KEY", "SINGLE", "DOUBLE"};
constexpr std::array<std::string_view, 9> kDifficulties{
    "ALL", "BEGINNER", "NORMAL", "HYPER", "ANOTHER", "INSANE",
    "SCRATCH CHART", "LONG NOTE CHART", "SPEED CHANGE CHART"};
constexpr std::size_t kMissingPosition = std::numeric_limits<std::size_t>::max();

void checkCancelled(std::stop_token stop) {
  if (stop.stop_requested()) throw std::runtime_error("song index cancelled");
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

std::uint16_t modeMask(const bms_parser::ChartMeta &meta) {
  const int mode = songMode(meta);
  if (mode == 0) return (1U << kModes.size()) - 1;
  std::uint16_t mask = 1;
  constexpr std::array modes{7, 14, 9, 5, 10, 25, 50};
  for (std::size_t position = 0; position < modes.size(); ++position) {
    if (mode == modes[position]) mask |= 1U << (position + 1);
  }
  if (mode == 5 || mode == 7) mask |= 1U << 8;
  if (mode == 10 || mode == 14) mask |= 1U << 9;
  return mask;
}

std::uint16_t difficultyMask(const ChartMetaRecord &record) {
  const auto &meta = record.meta;
  std::uint16_t mask = 1;
  constexpr std::array profiles{0, 500, 700, 1300, 2700};
  std::size_t closest = 0;
  int closestDistance = std::abs(meta.TotalNotes - profiles.front());
  for (std::size_t position = 0; position < profiles.size(); ++position) {
    const int distance = std::abs(meta.TotalNotes - profiles[position]);
    if (distance <= closestDistance) {
      closest = position;
      closestDistance = distance;
    }
  }
  mask |= 1U << (closest + 1);
  if (meta.TotalNotes > 0 &&
      (meta.TotalScratchNotes + meta.TotalBackSpinNotes) * 8 >= meta.TotalNotes) {
    mask |= 1U << 6;
  }
  if (meta.TotalNotes > 0 &&
      (meta.TotalLongNotes + meta.TotalBackSpinNotes) * 20 >= meta.TotalNotes) {
    mask |= 1U << 7;
  }
  if (meta.MinBpm != meta.MaxBpm || record.hasScrollChange || record.hasBpmStop) {
    mask |= 1U << 8;
  }
  return mask;
}

int clearLamp(int rank) {
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

std::string lowerAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

std::size_t startIndex(const auto &values, std::string_view selected) {
  return static_cast<std::size_t>(
      std::distance(values.begin(), std::ranges::find(values, selected)));
}

}

MusicSelectSongIndex::MusicSelectSongIndex(std::string directoryContext)
    : directoryContext_(std::move(directoryContext)) {}

void MusicSelectSongIndex::add(const ChartMetaRecord &record,
                               const std::optional<ScoreBestSnapshot> &score,
                               int clearRank) {
  if (finished_) throw std::logic_error("song index already finished");
  const auto &meta = record.meta;
  std::string identity;
  if (!meta.SHA256.empty()) {
    identity = "sha256:" + meta.SHA256;
  } else if (!meta.MD5.empty()) {
    identity = "md5:" + meta.MD5;
  } else {
    identity = "path:" + fspath_to_utf8(meta.BmsPath.lexically_normal());
  }
  const int scoreNotes = score ? score->maxScore / 2 : 0;
  Entry entry{
      .path = meta.BmsPath,
      .id = {directoryContext_ + ":" + identity},
      .title = meta.Title,
      .titleKey = lowerAscii(meta.Title),
      .artistKey = lowerAscii(meta.Artist),
      .maxBpm = meta.MaxBpm,
      .level = meta.PlayLevel,
      .scoreRate = scoreNotes != 0
                       ? static_cast<double>(score->score) / scoreNotes : 0,
      .length = meta.PlayLength,
      .duration = score ? score->averageJudgeMicros.value_or(0) : 0,
      .lastPlayed = score ? score->lastPlayedUnixSeconds.value_or(0) : 0,
      .difficulty = meta.Difficulty,
      .lamp = clearLamp(clearRank),
      .badPoints = score ? score->badPoints.value_or(0) : 0,
      .modes = modeMask(meta),
      .difficulties = difficultyMask(record),
      .hidden = (record.songReviewFavorite & (4 | 8)) != 0,
      .exists = !record.unavailable && !meta.BmsPath.empty(),
      .hasScore = score.has_value(),
      .hasScoreRate = scoreNotes != 0,
      .hasDuration = score && score->averageJudgeMicros.has_value()};
  hashes_.push_back(meta.SHA256);
  try {
    entries_.push_back(std::move(entry));
  } catch (...) {
    hashes_.pop_back();
    throw;
  }
}

void MusicSelectSongIndex::finish(std::stop_token stop) {
  checkCancelled(stop);
  if (finished_) {
    configuration_.reset();
    return;
  }
  std::unordered_set<std::string_view> seen;
  seen.reserve(hashes_.size());
  std::vector<std::size_t> representatives;
  representatives.reserve(entries_.size());
  for (std::size_t position = 0; position < entries_.size(); ++position) {
    checkCancelled(stop);
    if (seen.insert(hashes_[position]).second) {
      representatives.push_back(position);
    }
  }
  std::vector<Entry> compact;
  compact.reserve(representatives.size());
  for (auto position = representatives.rbegin();
       position != representatives.rend(); ++position) {
    checkCancelled(stop);
    compact.push_back(entries_[*position]);
  }
  std::vector<std::size_t> identities;
  std::vector<std::size_t> rows;
  std::vector<std::size_t> positions;
  identities.reserve(compact.size());
  rows.reserve(compact.size());
  positions.reserve(compact.size());
  for (std::size_t position = 0; position < compact.size(); ++position) {
    checkCancelled(stop);
    identities.push_back(position);
    rows.push_back(position);
    positions.push_back(position);
  }
  std::sort(identities.begin(), identities.end(), [&](auto left, auto right) {
    checkCancelled(stop);
    return compact[left].id < compact[right].id;
  });
  checkCancelled(stop);
  entries_.swap(compact);
  identityOrder_.swap(identities);
  rows_.swap(rows);
  positions_.swap(positions);
  std::vector<std::string>{}.swap(hashes_);
  finished_ = true;
}

std::pair<std::string, std::string> MusicSelectSongIndex::configure(
    std::string_view mode, std::string_view difficulty, std::string_view sort,
    std::stop_token stop) {
  checkCancelled(stop);
  if (!finished_) throw std::logic_error("song index not finished");
  if (configuration_ && configuration_->mode == mode &&
      configuration_->difficulty == difficulty && configuration_->sort == sort) {
    return configuration_->resolved;
  }
  std::pair<std::string, std::string> resolved{mode, difficulty};
  std::vector<std::size_t> rows;
  rows.reserve(entries_.size());
  if (!entries_.empty()) {
    const auto modeStart = startIndex(kModes, mode);
    const auto difficultyStart = startIndex(kDifficulties, difficulty);
    bool filtered = false;
    for (std::size_t difficultyTrial = 0;
         difficultyTrial < kDifficulties.size() && !filtered; ++difficultyTrial) {
      const auto selectedDifficulty =
          (difficultyStart + difficultyTrial) % kDifficulties.size();
      for (std::size_t modeTrial = 0; modeTrial < kModes.size(); ++modeTrial) {
        const auto selectedMode = (modeStart + modeTrial) % kModes.size();
        rows.clear();
        for (std::size_t position = 0; position < entries_.size(); ++position) {
          checkCancelled(stop);
          const auto &entry = entries_[position];
          if (!entry.hidden && (entry.modes & (1U << selectedMode)) != 0 &&
              (entry.difficulties & (1U << selectedDifficulty)) != 0) {
            rows.push_back(position);
          }
        }
        if (!rows.empty()) {
          resolved.first = kModes[selectedMode];
          resolved.second = kDifficulties[selectedDifficulty];
          filtered = true;
          break;
        }
      }
    }
    if (!filtered) {
      for (std::size_t position = 0; position < entries_.size(); ++position) {
        checkCancelled(stop);
        rows.push_back(position);
      }
    }
    std::stable_sort(rows.begin(), rows.end(), [&](auto left, auto right) {
      checkCancelled(stop);
      return compare(entries_[left], entries_[right], sort) < 0;
    });
  }
  std::vector<std::size_t> positions;
  positions.reserve(entries_.size());
  for (std::size_t position = 0; position < entries_.size(); ++position) {
    checkCancelled(stop);
    positions.push_back(kMissingPosition);
  }
  for (std::size_t position = 0; position < rows.size(); ++position) {
    checkCancelled(stop);
    positions[rows[position]] = position;
  }
  Configuration configuration{std::string(mode), std::string(difficulty),
                                std::string(sort), resolved};
  checkCancelled(stop);
  rows_.swap(rows);
  positions_.swap(positions);
  configuration_ = std::move(configuration);
  return resolved;
}

int MusicSelectSongIndex::compare(const Entry &left, const Entry &right,
                                  std::string_view sort) {
  if (sort == "ARTIST" || sort == "BPM" || sort == "LENGTH" || sort == "LEVEL") {
    if (!left.exists && !right.exists) return 0;
    if (!left.exists) return 1;
    if (!right.exists) return -1;
  }
  if (sort == "ARTIST") {
    return left.artistKey < right.artistKey ? -1 : left.artistKey > right.artistKey;
  }
  if (sort == "BPM") {
    return left.maxBpm < right.maxBpm ? -1 : left.maxBpm > right.maxBpm;
  }
  if (sort == "LENGTH") {
    return left.length < right.length ? -1 : left.length > right.length;
  }
  if (sort == "LEVEL") {
    if (left.level != right.level) return left.level < right.level ? -1 : 1;
    return left.difficulty - right.difficulty;
  }
  if (sort == "CLEAR" || sort == "MISSCOUNT" || sort == "LASTUPDATE") {
    if (!left.hasScore && !right.hasScore) return 0;
    if (!left.hasScore) return 1;
    if (!right.hasScore) return -1;
    if (sort == "CLEAR") return left.lamp - right.lamp;
    if (sort == "MISSCOUNT") return left.badPoints - right.badPoints;
    return left.lastPlayed < right.lastPlayed ? -1 : left.lastPlayed > right.lastPlayed;
  }
  if (sort == "SCORE") {
    if (!left.hasScoreRate && !right.hasScoreRate) return 0;
    if (!left.hasScoreRate) return 1;
    if (!right.hasScoreRate) return -1;
    return left.scoreRate < right.scoreRate ? -1 : left.scoreRate > right.scoreRate;
  }
  if (sort == "DURATION") {
    if (!left.hasDuration && !right.hasDuration) return 0;
    if (!left.hasDuration) return 1;
    if (!right.hasDuration) return -1;
    const std::uint32_t narrowed = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(left.duration) -
        static_cast<std::uint64_t>(right.duration));
    return std::bit_cast<std::int32_t>(narrowed);
  }
  if (sort == "RIVALCOMPARE_CLEAR" || sort == "RIVALCOMPARE_SCORE") return 0;
  if (left.titleKey < right.titleKey) return -1;
  if (left.titleKey > right.titleKey) return 1;
  return left.difficulty - right.difficulty;
}

std::size_t MusicSelectSongIndex::size() const noexcept { return rows_.size(); }

const std::filesystem::path &MusicSelectSongIndex::pathAt(std::size_t index) const {
  return entries_[rows_.at(index)].path;
}

const MusicSelectBarId &MusicSelectSongIndex::idAt(std::size_t index) const {
  return entries_[rows_.at(index)].id;
}

const std::string &MusicSelectSongIndex::titleAt(std::size_t index) const {
  return entries_[rows_.at(index)].title;
}

std::optional<std::size_t>
MusicSelectSongIndex::indexOf(const MusicSelectBarId &id) const {
  const auto found = std::lower_bound(
      identityOrder_.begin(), identityOrder_.end(), id,
      [&](std::size_t position, const MusicSelectBarId &target) {
        return entries_[position].id < target;
      });
  if (found == identityOrder_.end() || entries_[*found].id != id ||
      positions_[*found] == kMissingPosition) {
    return std::nullopt;
  }
  return positions_[*found];
}
