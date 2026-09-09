#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <optional>
#include <utility>
#include <vector>

#include "../repositories/ChartRepository.h"

struct MusicSelectBar;
class MusicSelectRowProvider;

namespace skin {

struct MusicSelectPropertyValues {
  std::map<int, bool> booleans;
  std::map<int, std::int64_t> integers;
  std::map<int, std::int64_t> imageIndexes;
  std::map<int, double> rates;
  std::map<int, double> floats;
  std::map<int, std::string> strings;
  std::map<int, std::int64_t> timers;
  std::map<std::string, bool, std::less<>> namedBooleans;
  std::map<std::string, std::int64_t, std::less<>> namedIntegers;
  std::map<std::string, std::int64_t, std::less<>> namedImageIndexes;
  std::map<std::string, double, std::less<>> namedRates;
  std::map<std::string, double, std::less<>> namedFloats;
  std::map<std::string, std::string, std::less<>> namedStrings;
  std::map<std::string, std::int64_t, std::less<>> namedTimers;
};

enum class MusicSelectBarKind : std::uint8_t {
  Song,
  Folder,
  Table,
  Hash,
  Executable,
  Grade,
  RandomCourse,
  Command,
  Container,
  SearchWord,
  SameFolder,
};

[[nodiscard]] inline constexpr bool
musicSelectIsDirectoryBarKind(MusicSelectBarKind kind) noexcept {
  switch (kind) {
  case MusicSelectBarKind::Folder:
  case MusicSelectBarKind::Table:
  case MusicSelectBarKind::Hash:
  case MusicSelectBarKind::Command:
  case MusicSelectBarKind::Container:
  case MusicSelectBarKind::SearchWord:
  case MusicSelectBarKind::SameFolder:
    return true;
  case MusicSelectBarKind::Song:
  case MusicSelectBarKind::Executable:
  case MusicSelectBarKind::Grade:
  case MusicSelectBarKind::RandomCourse:
    return false;
  }
  return false;
}

[[nodiscard]] inline constexpr bool
musicSelectIsSelectableBarKind(MusicSelectBarKind kind) noexcept {
  switch (kind) {
  case MusicSelectBarKind::Song:
  case MusicSelectBarKind::Executable:
  case MusicSelectBarKind::Grade:
  case MusicSelectBarKind::RandomCourse:
    return true;
  case MusicSelectBarKind::Folder:
  case MusicSelectBarKind::Table:
  case MusicSelectBarKind::Hash:
  case MusicSelectBarKind::Command:
  case MusicSelectBarKind::Container:
  case MusicSelectBarKind::SearchWord:
  case MusicSelectBarKind::SameFolder:
    return false;
  }
  return false;
}

inline constexpr int MusicSelectFeatureUndefinedLn = 1;
inline constexpr int MusicSelectFeatureMine = 2;
inline constexpr int MusicSelectFeatureRandom = 4;
inline constexpr int MusicSelectFeatureLongNote = 8;
inline constexpr int MusicSelectFeatureChargeNote = 16;
inline constexpr int MusicSelectFeatureHellChargeNote = 32;

enum class MusicSelectCourseConstraint : std::uint8_t {
  Class,
  Mirror,
  Random,
  NoSpeed,
  NoGood,
  NoGreat,
  GaugeLr2,
  Gauge5Keys,
  Gauge7Keys,
  Gauge9Keys,
  Gauge24Keys,
  Ln,
  Cn,
  Hcn,
};

struct MusicSelectCourseStage {
  std::optional<std::string> title;
  bool hasPath = false;
};

struct MusicSelectBarFrame {
  MusicSelectBarKind kind = MusicSelectBarKind::Song;
  std::string title;
  bool exists = false;
  std::int64_t addDateSeconds = 0;
  int lamp = 0;
  int rivalLamp = 0;
  int difficulty = 0;
  int level = 0;
  int featureFlags = 0;
  std::string trophyName;
  std::array<int, 11> folderLampCounts{};
  std::array<int, 28> folderRankCounts{};
};

struct MusicSelectSongListFrame {
  std::vector<MusicSelectBarFrame> bars;
  std::size_t selectedIndex = 0;
  std::int64_t elapsedMillis = 0;
  std::int64_t wallClockSeconds = 0;
  std::int64_t wallClockMillis = 0;
  // Beatoraja PlayerConfig.getLnmode(): LN=0, CN=1, HCN=2.
  int playerLnMode = 0;
  bool rivalSelected = false;
  int movementDirection = 0;
  std::int64_t movementEndMillis = 0;
  std::shared_ptr<const std::vector<MusicSelectBar>> indexedBars;
  std::shared_ptr<MusicSelectRowProvider> rowProvider;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] const MusicSelectBarFrame &at(std::size_t index) const;
};

} // namespace skin

struct MusicSelectBarId {
  std::string value;

  auto operator<=>(const MusicSelectBarId &) const = default;
};

struct MusicSelectBar {
  MusicSelectBarId id;
  skin::MusicSelectBarKind kind = skin::MusicSelectBarKind::Song;
  std::string title;
  std::filesystem::path directoryPath;
  int tableId = 0;
  std::string tableUrl;
  std::string tableLevel;
  int courseId = 0;
  std::string courseKey;
  std::string courseGroupName;
  std::string courseConstraintJson;
  std::vector<ChartMetaRecord> courseCharts;
  std::optional<ChartMetaRecord> chart;
  std::optional<ScoreBestSnapshot> score;
  std::optional<ScoreBestSnapshot> rivalScore;
  std::array<bool, 4> replayExists{};
  std::vector<skin::MusicSelectCourseStage> courseStages;
  std::vector<skin::MusicSelectCourseConstraint> courseConstraints;
  int courseTotalNotes = 0;
  std::vector<MusicSelectBarId> children;
  skin::MusicSelectBarFrame presentation;
  bool selectable = false;
  bool sortable = false;
  bool childrenLoaded = true;
  bool showInvisibleCharts = false;
};

[[nodiscard]] inline bool
musicSelectIsSolidArchiveDirectory(const MusicSelectBar &bar) {
  return bar.kind == skin::MusicSelectBarKind::Container &&
         bar.id.value == "container:solid-archives";
}

[[nodiscard]] inline bool
musicSelectIsUnzipAllAction(const MusicSelectBar &bar) {
  return bar.kind == skin::MusicSelectBarKind::Executable &&
         bar.id.value == "action:unzip-all-archives";
}

[[nodiscard]] inline bool
musicSelectIsSolidArchiveAction(const MusicSelectBar &bar) {
  return bar.kind == skin::MusicSelectBarKind::Executable && bar.chart &&
         bar.chart->solidArchive && !bar.chart->unavailable &&
         !bar.chart->meta.BmsPath.empty();
}

class MusicSelectRowProvider {
public:
  virtual ~MusicSelectRowProvider() = default;
  [[nodiscard]] virtual std::shared_ptr<MusicSelectRowProvider> clone() const = 0;
  [[nodiscard]] virtual std::size_t size() const noexcept = 0;
  [[nodiscard]] virtual const std::string &diagnostic() const noexcept {
    static const std::string empty;
    return empty;
  }
  [[nodiscard]] virtual const MusicSelectBar &at(std::size_t index) const = 0;
  [[nodiscard]] virtual std::optional<std::size_t>
  indexOf(const MusicSelectBarId &) const = 0;
  virtual std::pair<std::string, std::string> configure(
      const std::string &modeFilter, const std::string &difficultyFilter,
      const std::string &sortId) = 0;
};

struct MusicSelectProjection {
  std::vector<MusicSelectBar> bars;
  std::vector<MusicSelectBarId> root;
  std::uint64_t repositoryRevision = 0;

  [[nodiscard]] const MusicSelectBar *find(const MusicSelectBarId &) const;
};

inline std::size_t skin::MusicSelectSongListFrame::size() const noexcept {
  if (rowProvider) return rowProvider->size();
  return indexedBars ? indexedBars->size() : bars.size();
}

inline const skin::MusicSelectBarFrame &
skin::MusicSelectSongListFrame::at(std::size_t index) const {
  if (rowProvider) return rowProvider->at(index).presentation;
  return indexedBars ? indexedBars->at(index).presentation : bars.at(index);
}
