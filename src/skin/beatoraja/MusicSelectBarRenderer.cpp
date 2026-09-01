#include "MusicSelectBarRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>

namespace skin {
namespace {

struct PreparedDestination {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
};

std::optional<PreparedDestination>
destinationAt(const SkinSongListPresentation &presentation,
              std::int64_t elapsedMillis) {
  const auto &body = presentation.destination;
  if (presentation.object == 0 || body.frames.empty()) {
    return std::nullopt;
  }
  std::int64_t time = elapsedMillis;
  const int end = body.frames.back().timeMillis;
  if (body.loop >= 0 && end > body.loop && time > end) {
    time = body.loop + (time - body.loop) % (end - body.loop);
  }
  const SkinDestinationFrame *selected = &body.frames.front();
  for (const auto &candidate : body.frames) {
    if (candidate.timeMillis <= time) {
      selected = &candidate;
    }
  }
  return PreparedDestination{.x = selected->x,
                             .y = selected->y,
                             .width = selected->width,
                             .height = selected->height};
}

std::optional<std::size_t>
wrappedBarIndex(const MusicSelectSongListFrame &frame, int row, int center) {
  if (frame.bars.empty()) {
    return std::nullopt;
  }
  const std::int64_t count = static_cast<std::int64_t>(frame.bars.size());
  const std::int64_t raw = static_cast<std::int64_t>(frame.selectedIndex) +
                           count * 100 + row - center;
  const std::int64_t index = raw % count;
  if (index < 0 || index >= count) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(index);
}

int barValue(const MusicSelectBarFrame &bar) {
  switch (bar.kind) {
  case MusicSelectBarKind::Table:
  case MusicSelectBarKind::Hash:
  case MusicSelectBarKind::Executable:
    return 2;
  case MusicSelectBarKind::Grade:
    return bar.exists ? 3 : 4;
  case MusicSelectBarKind::RandomCourse:
    return bar.exists ? 2 : 4;
  case MusicSelectBarKind::Folder:
    return 1;
  case MusicSelectBarKind::Song:
    return bar.exists ? 0 : 4;
  case MusicSelectBarKind::SearchWord:
    return 6;
  case MusicSelectBarKind::Command:
  case MusicSelectBarKind::Container:
    return 5;
  case MusicSelectBarKind::SameFolder:
    return -1;
  }
  return -1;
}

bool directory(const MusicSelectBarFrame &bar) {
  switch (bar.kind) {
  case MusicSelectBarKind::Folder:
  case MusicSelectBarKind::Table:
  case MusicSelectBarKind::Hash:
  case MusicSelectBarKind::Command:
  case MusicSelectBarKind::Container:
  case MusicSelectBarKind::SearchWord:
  case MusicSelectBarKind::SameFolder:
    return true;
  default:
    return false;
  }
}

const SkinSongListPresentation *slot(
    std::span<const SkinSongListPresentation> values, std::size_t index,
    std::size_t fixedCount) {
  return index < fixedCount && index < values.size() &&
                 values[index].object != 0
             ? &values[index]
             : nullptr;
}

int textSlot(const SkinSongListObject &songList,
             const MusicSelectBarFrame &bar, int value,
             std::int64_t nowSeconds) {
  int result = value;
  if (result >= 2) {
    result += 4;
    if (!slot(songList.text, static_cast<std::size_t>(result), 11)) {
      result = 0;
    }
    return result;
  }
  if (result == 0) {
    result = nowSeconds > bar.addDateSeconds + 86'400 ? 2 : 3;
    if (!slot(songList.text, static_cast<std::size_t>(result), 11)) {
      result = result == 3 ? 1 : 0;
    }
    return result;
  }
  result = nowSeconds > bar.addDateSeconds + 86'400 ? 4 : 5;
  if (!slot(songList.text, static_cast<std::size_t>(result), 11)) {
    result = result == 5 ? 1 : 0;
  }
  return result;
}

void appendCommand(std::vector<MusicSelectBarDrawCommand> &commands,
                   MusicSelectBarDrawFamily family,
                   const MusicSelectBarRow &row,
                   const SkinSongListPresentation &presentation,
                   std::size_t slotIndex, std::int64_t elapsedMillis,
                   std::string text = {}, int value = 0,
                   std::array<int, 11> folderLampCounts = {},
                   std::array<int, 28> folderRankCounts = {}) {
  const auto destination = destinationAt(presentation, elapsedMillis);
  commands.push_back({.family = family,
                      .row = row.row,
                      .barIndex = row.barIndex,
                      .slot = slotIndex,
                      .object = presentation.object,
                      .offsetX = row.x + (destination ? destination->x : 0.0),
                      .offsetY = row.y + (destination ? destination->y : 0.0),
                      .text = std::move(text),
                      .value = value,
                      .folderLampCounts = folderLampCounts,
                      .folderRankCounts = folderRankCounts});
}

} // namespace

MusicSelectBarRenderPlan MusicSelectBarRenderer::plan(
    const SkinSongListObject &songList,
    const MusicSelectSongListFrame &frame) const {
  MusicSelectBarRenderPlan result;
  result.rows.resize(barCount);
  std::array<double, barCount> barHeights{};
  for (std::size_t rowIndex = 0; rowIndex < barCount; ++rowIndex) {
    auto &row = result.rows[rowIndex];
    row.row = rowIndex;
    const bool selected = static_cast<int>(rowIndex) == songList.center;
    const auto &presentations = selected ? songList.listOn : songList.listOff;
    const auto *barPresentation = slot(presentations, rowIndex, barCount);
    const auto destination =
        barPresentation
            ? destinationAt(*barPresentation, frame.elapsedMillis)
            : std::nullopt;
    const auto barIndex = wrappedBarIndex(
        frame, static_cast<int>(rowIndex), songList.center);
    if (!barPresentation || !destination || !barIndex) {
      continue;
    }
    row.barIndex = *barIndex;
    row.value = barValue(frame.bars[*barIndex]);
    if (row.value == -1) {
      continue;
    }
    row.x = destination->x;
    row.y = destination->y;
    barHeights[rowIndex] = destination->height;
    row.textSlot = textSlot(songList, frame.bars[*barIndex], row.value,
                            frame.wallClockSeconds);
  }

  if (frame.movementDirection != 0 &&
      frame.movementEndMillis > frame.wallClockMillis) {
    const double lerp = frame.movementDirection < 0
                            ? static_cast<double>(frame.wallClockMillis -
                                                  frame.movementEndMillis) /
                                  frame.movementDirection
                            : static_cast<double>(frame.movementEndMillis -
                                                  frame.wallClockMillis) /
                                  frame.movementDirection;
    const auto stationaryRows = result.rows;
    for (std::size_t index = 0; index < result.rows.size(); ++index) {
      auto &row = result.rows[index];
      if (row.value == -1) {
        continue;
      }
      const int nextIndex = static_cast<int>(index) +
                            (frame.movementDirection >= 0 ? 1 : -1);
      if (nextIndex < 0 || nextIndex >= static_cast<int>(result.rows.size()) ||
          stationaryRows[static_cast<std::size_t>(nextIndex)].value == -1) {
        continue;
      }
      const auto &next = stationaryRows[static_cast<std::size_t>(nextIndex)];
      row.x += (next.x - row.x) * std::clamp(lerp, -1.0, 1.0);
      row.y += (next.y - row.y) * lerp;
    }
  }

  for (auto &row : result.rows) {
    if (row.value == -1) {
      continue;
    }
    row.x = static_cast<double>(static_cast<int>(row.x));
    row.y = static_cast<double>(static_cast<int>(
        row.y + (songList.center == 1 ? barHeights[row.row] : 0.0)));
  }

  const auto forEachDrawnRow = [&](auto &&action) {
    for (const auto &row : result.rows) {
      if (row.value != -1) {
        action(row, frame.bars[row.barIndex]);
      }
    }
  };

  forEachDrawnRow([&](const auto &row, const auto &) {
    const bool selected = static_cast<int>(row.row) == songList.center;
    const auto &presentations = selected ? songList.listOn : songList.listOff;
    if (const auto *value = slot(presentations, row.row, barCount)) {
      appendCommand(result.commands, MusicSelectBarDrawFamily::BarImage, row,
                    *value, row.row, frame.elapsedMillis, {}, row.value);
    }
  });
  forEachDrawnRow([&](const auto &row, const auto &bar) {
    if (directory(bar) && songList.graph && songList.graph->object != 0) {
      appendCommand(result.commands,
                    MusicSelectBarDrawFamily::FolderGraph, row,
                    *songList.graph, 0, frame.elapsedMillis, {}, 0,
                    bar.folderLampCounts, bar.folderRankCounts);
    }
  });
  forEachDrawnRow([&](const auto &row, const auto &bar) {
    if (const auto *value =
            slot(songList.text, static_cast<std::size_t>(row.textSlot), 11)) {
      appendCommand(result.commands, MusicSelectBarDrawFamily::Title, row,
                    *value, static_cast<std::size_t>(row.textSlot),
                    frame.elapsedMillis, bar.title);
    }
  });
  constexpr std::array<std::string_view, 3> trophies{
      "bronzemedal", "silvermedal", "goldmedal"};
  forEachDrawnRow([&](const auto &row, const auto &bar) {
    if (bar.kind != MusicSelectBarKind::Grade) {
      return;
    }
    const auto trophy = std::ranges::find(trophies, bar.trophyName);
    if (trophy == trophies.end()) {
      return;
    }
    const auto index = static_cast<std::size_t>(trophy - trophies.begin());
    if (const auto *value = slot(songList.trophy, index, 3)) {
      appendCommand(result.commands, MusicSelectBarDrawFamily::Trophy, row,
                    *value, index, frame.elapsedMillis);
    }
  });
  forEachDrawnRow([&](const auto &row, const auto &bar) {
    if (frame.rivalSelected) {
      if (bar.lamp >= 0) {
        if (const auto *value = slot(songList.playerLamp,
                                     static_cast<std::size_t>(bar.lamp), 11)) {
          appendCommand(result.commands,
                        MusicSelectBarDrawFamily::PlayerLamp, row, *value,
                        static_cast<std::size_t>(bar.lamp),
                        frame.elapsedMillis);
        }
      }
      if (bar.rivalLamp >= 0) {
        if (const auto *value = slot(
                songList.rivalLamp,
                static_cast<std::size_t>(bar.rivalLamp), 11)) {
          appendCommand(result.commands,
                        MusicSelectBarDrawFamily::RivalLamp, row, *value,
                        static_cast<std::size_t>(bar.rivalLamp),
                        frame.elapsedMillis);
        }
      }
    } else if (bar.lamp >= 0) {
      if (const auto *value =
              slot(songList.lamp, static_cast<std::size_t>(bar.lamp), 11)) {
        appendCommand(result.commands, MusicSelectBarDrawFamily::Lamp, row,
                      *value, static_cast<std::size_t>(bar.lamp),
                      frame.elapsedMillis);
      }
    }
  });
  forEachDrawnRow([&](const auto &row, const auto &bar) {
    if (bar.kind != MusicSelectBarKind::Song || !bar.exists) {
      return;
    }
    const int difficulty = bar.difficulty >= 0 && bar.difficulty < 7
                               ? bar.difficulty
                               : 0;
    if (const auto *value = slot(songList.level,
                                 static_cast<std::size_t>(difficulty), 7)) {
      appendCommand(result.commands, MusicSelectBarDrawFamily::Level, row,
                    *value, static_cast<std::size_t>(difficulty),
                    frame.elapsedMillis, {}, bar.level);
    }
  });
  forEachDrawnRow([&](const auto &row, const auto &bar) {
    int ln = -1;
    if ((bar.featureFlags & MusicSelectFeatureUndefinedLn) != 0) {
      ln = frame.playerLnMode;
    }
    if ((bar.featureFlags & MusicSelectFeatureLongNote) != 0) {
      ln = std::max(ln, 0);
    }
    if ((bar.featureFlags & MusicSelectFeatureChargeNote) != 0) {
      ln = std::max(ln, 1);
    }
    if ((bar.featureFlags & MusicSelectFeatureHellChargeNote) != 0) {
      ln = std::max(ln, 2);
    }
    if (ln >= 0 && ln < 3) {
      constexpr std::array<std::size_t, 3> lnSlots{0, 3, 4};
      std::size_t label = lnSlots[static_cast<std::size_t>(ln)];
      const auto *value = slot(songList.label, label, 5);
      if (!value) {
        label = 0;
        value = slot(songList.label, label, 5);
      }
      if (value) {
        appendCommand(result.commands, MusicSelectBarDrawFamily::Label, row,
                      *value, label, frame.elapsedMillis);
      }
    }
    if ((bar.featureFlags & MusicSelectFeatureMine) != 0) {
      if (const auto *value = slot(songList.label, 2, 5)) {
        appendCommand(result.commands, MusicSelectBarDrawFamily::Label, row,
                      *value, 2, frame.elapsedMillis);
      }
    }
    if ((bar.featureFlags & MusicSelectFeatureRandom) != 0) {
      if (const auto *value = slot(songList.label, 1, 5)) {
        appendCommand(result.commands, MusicSelectBarDrawFamily::Label, row,
                      *value, 1, frame.elapsedMillis);
      }
    }
  });
  return result;
}

MusicSelectBarPointerResult MusicSelectBarRenderer::pointer(
    const SkinSongListObject &songList,
    const MusicSelectSongListFrame &frame,
    MusicSelectBarPointerInput pointerInput) const {
  for (const int row : songList.clickable) {
    if (row < 0 || row >= static_cast<int>(barCount)) {
      continue;
    }
    const bool selected = row == songList.center;
    const auto &presentations = selected ? songList.listOn : songList.listOff;
    const auto *barPresentation =
        slot(presentations, static_cast<std::size_t>(row), barCount);
    const auto destination =
        barPresentation
            ? destinationAt(*barPresentation, frame.elapsedMillis)
            : std::nullopt;
    const auto index = wrappedBarIndex(frame, row, songList.center);
    if (!destination || !index) {
      continue;
    }
    if (destination->x <= pointerInput.x &&
        destination->x + destination->width >= pointerInput.x &&
        destination->y <= pointerInput.y &&
        destination->y + destination->height >= pointerInput.y) {
      return {.consumed = true,
              .selectIndex = pointerInput.button == 0 ? index : std::nullopt,
              .closeDirectory = pointerInput.button != 0};
    }
  }
  return {};
}

} // namespace skin
