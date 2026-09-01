#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skin {

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
