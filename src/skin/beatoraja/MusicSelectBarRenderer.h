#pragma once

#include "BeatorajaSkinModel.h"
#include "../../music_select/MusicSelectTypes.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace skin {

enum class MusicSelectBarDrawFamily : std::uint8_t {
  BarImage,
  FolderGraph,
  Title,
  Trophy,
  Lamp,
  PlayerLamp,
  RivalLamp,
  Level,
  Label,
};

struct MusicSelectBarRow {
  std::size_t row = 0;
  std::size_t barIndex = 0;
  int value = -1;
  int textSlot = 0;
  double x = 0.0;
  double y = 0.0;
};

struct MusicSelectBarDrawCommand {
  MusicSelectBarDrawFamily family = MusicSelectBarDrawFamily::BarImage;
  std::size_t row = 0;
  std::size_t barIndex = 0;
  std::size_t slot = 0;
  SkinObjectId object = 0;
  double offsetX = 0.0;
  double offsetY = 0.0;
  std::string text;
  int value = 0;
  std::array<int, 11> folderLampCounts{};
};

struct MusicSelectBarRenderPlan {
  std::vector<MusicSelectBarRow> rows;
  std::vector<MusicSelectBarDrawCommand> commands;
  std::optional<std::string> failure;
};

struct MusicSelectBarPointerInput {
  int button = 0;
  double x = 0.0;
  double y = 0.0;
};

struct MusicSelectBarPointerResult {
  bool consumed = false;
  std::optional<std::size_t> selectIndex;
  bool closeDirectory = false;
};

class MusicSelectBarRenderer final {
public:
  static constexpr std::size_t barCount = 60;

  [[nodiscard]] MusicSelectBarRenderPlan
  plan(const SkinSongListObject &songList,
       const MusicSelectSongListFrame &frame) const;
  [[nodiscard]] MusicSelectBarPointerResult
  pointer(const SkinSongListObject &songList,
          const MusicSelectSongListFrame &frame,
          MusicSelectBarPointerInput pointer) const;

  [[nodiscard]] static constexpr std::size_t
  slotCount(MusicSelectBarDrawFamily family) noexcept {
    switch (family) {
    case MusicSelectBarDrawFamily::BarImage:
      return 60;
    case MusicSelectBarDrawFamily::FolderGraph:
      return 1;
    case MusicSelectBarDrawFamily::Title:
      return 11;
    case MusicSelectBarDrawFamily::Trophy:
      return 3;
    case MusicSelectBarDrawFamily::Lamp:
    case MusicSelectBarDrawFamily::PlayerLamp:
    case MusicSelectBarDrawFamily::RivalLamp:
      return 11;
    case MusicSelectBarDrawFamily::Level:
      return 7;
    case MusicSelectBarDrawFamily::Label:
      return 5;
    }
    return 0;
  }
};

} // namespace skin
