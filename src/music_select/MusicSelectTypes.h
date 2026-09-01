#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <optional>
#include <vector>

#include "../repositories/ChartRepository.h"

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
  int playerLnMode = 0;
  bool rivalSelected = false;
  int movementDirection = 0;
  std::int64_t movementEndMillis = 0;
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
};

struct MusicSelectProjection {
  std::vector<MusicSelectBar> bars;
  std::vector<MusicSelectBarId> root;
  std::uint64_t repositoryRevision = 0;

  [[nodiscard]] const MusicSelectBar *find(const MusicSelectBarId &) const;
};
