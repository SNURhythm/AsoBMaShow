#include "skin/beatoraja/MusicSelectBarRenderer.h"

#include "music_select_runtime_ledger_assertions.h"

#include <iostream>
#include <ranges>
#include <string_view>

namespace {

using namespace skin;

int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

SkinSongListPresentation presentation(SkinObjectId object, double x,
                                      double y = 0) {
  return {.object = object,
          .destination = {.frames = {{.x = x,
                                      .y = y,
                                      .width = 100,
                                      .height = 20}}}};
}

SkinSongListObject completeSongList() {
  SkinSongListObject result{.center = 1, .clickable = {1, 0}};
  for (int index = 0; index < 61; ++index) {
    result.listOn.push_back(presentation(100 + index, index * 10, 100));
    result.listOff.push_back(presentation(200 + index, index * 10, 100));
  }
  for (int index = 0; index < 12; ++index) {
    result.text.push_back(presentation(300 + index, 2, 3));
    result.lamp.push_back(presentation(400 + index, 4, 5));
    result.playerLamp.push_back(presentation(500 + index, 6, 7));
    result.rivalLamp.push_back(presentation(600 + index, 8, 9));
  }
  for (int index = 0; index < 4; ++index) {
    result.trophy.push_back(presentation(700 + index, 10, 11));
  }
  for (int index = 0; index < 8; ++index) {
    result.level.push_back(presentation(800 + index, 12, 13));
  }
  for (int index = 0; index < 6; ++index) {
    result.label.push_back(presentation(900 + index, 14, 15));
  }
  result.graph = presentation(1000, 16, 17);
  return result;
}

MusicSelectSongListFrame completeFrame() {
  MusicSelectSongListFrame frame;
  frame.selectedIndex = 0;
  frame.wallClockSeconds = 86'400;
  frame.wallClockMillis = 86'400'000;
  frame.playerLnMode = 1;
  frame.rivalSelected = true;
  frame.bars = {
      {.kind = MusicSelectBarKind::Song,
       .title = "song",
       .exists = true,
       .addDateSeconds = 0,
       .lamp = 2,
       .rivalLamp = 3,
       .difficulty = 6,
       .level = 12,
       .featureFlags = MusicSelectFeatureUndefinedLn |
                       MusicSelectFeatureLongNote |
                       MusicSelectFeatureChargeNote |
                       MusicSelectFeatureMine |
                       MusicSelectFeatureRandom},
      {.kind = MusicSelectBarKind::Folder,
       .title = "folder",
       .exists = true,
       .addDateSeconds = 0},
      {.kind = MusicSelectBarKind::Grade,
       .title = "grade",
       .exists = true,
       .trophyName = "silvermedal"},
      {.kind = MusicSelectBarKind::SameFolder,
       .title = "same-folder",
       .exists = true},
  };
  return frame;
}

void testPinnedDrawOrderSlotsAndClassValues() {
  auto songList = completeSongList();
  auto frame = completeFrame();
  const auto plan = MusicSelectBarRenderer{}.plan(songList, frame);
  require(!plan.failure.has_value(), "valid SkinBar frame plans");
  require(plan.rows.size() == 60, "SkinBar prepares exactly 60 rows");
  require(plan.rows[1].value == 0 && plan.rows[2].value == 1 &&
              plan.rows[3].value == 3 && plan.rows[0].value == -1,
          "bar classes map to pinned values and SameFolder remains no-draw");
  require(plan.rows[1].textSlot == 3,
          "exactly addDate plus 24 hours is still new");
  require(plan.rows[1].y == 100.0,
          "type-5 bar position zero anchors relative children at the origin");
  require(plan.rows[1].x == 10.0,
          "bar origin x is the authored destination x");

  const auto phase = [](MusicSelectBarDrawFamily family) {
    switch (family) {
    case MusicSelectBarDrawFamily::BarImage: return 0;
    case MusicSelectBarDrawFamily::FolderGraph: return 1;
    case MusicSelectBarDrawFamily::Title: return 2;
    case MusicSelectBarDrawFamily::Trophy: return 3;
    case MusicSelectBarDrawFamily::Lamp:
    case MusicSelectBarDrawFamily::PlayerLamp:
    case MusicSelectBarDrawFamily::RivalLamp: return 4;
    case MusicSelectBarDrawFamily::Level: return 5;
    case MusicSelectBarDrawFamily::Label: return 6;
    }
    return 7;
  };
  int prior = 0;
  for (const auto &command : plan.commands) {
    require(phase(command.family) >= prior,
            "draw commands preserve bar/graph/title/trophy/lamp/level/label order");
    prior = phase(command.family);
    require(command.slot < MusicSelectBarRenderer::slotCount(command.family),
            "fixed SkinBar setters ignore every out-of-slot authored entry");
  }
  const auto has = [&](MusicSelectBarDrawFamily family, int slot) {
    return std::ranges::any_of(plan.commands, [&](const auto &command) {
      return command.family == family && command.slot == slot;
    });
  };
  require(has(MusicSelectBarDrawFamily::Trophy, 1),
          "silver trophy maps to slot 1");
  require(has(MusicSelectBarDrawFamily::PlayerLamp, 2) &&
              has(MusicSelectBarDrawFamily::RivalLamp, 3),
          "rival mode draws player then rival lamps");
  require(has(MusicSelectBarDrawFamily::Level, 6),
          "difficulty 6 uses the seventh level slot");
  require(has(MusicSelectBarDrawFamily::Label, 3) &&
              has(MusicSelectBarDrawFamily::Label, 2) &&
              has(MusicSelectBarDrawFamily::Label, 1),
          "charge, mine, and random labels retain source precedence");
  require(!has(MusicSelectBarDrawFamily::Label, 5) &&
              !has(MusicSelectBarDrawFamily::Level, 7),
          "authored label and level entries beyond fixed slots are inert");
}

void testPositionOneAnchorsChildrenBelowTheBarOrigin() {
  auto songList = completeSongList();
  songList.position = 1;
  auto frame = completeFrame();
  const auto plan = MusicSelectBarRenderer{}.plan(songList, frame);
  require(plan.rows[1].y == 120.0,
          "SkinBar position one anchors relative children one bar height down");
}

void testTextFallbackAndNewBoundary() {
  auto songList = completeSongList();
  songList.text[2].object = 0;
  songList.text[3].object = 0;
  songList.text[4].object = 0;
  auto frame = completeFrame();
  frame.rivalSelected = false;
  frame.wallClockSeconds = 86'401;
  const auto plan = MusicSelectBarRenderer{}.plan(songList, frame);
  require(plan.rows[1].textSlot == 0,
          "missing song-specific text falls back to normal slot after 24h");
  require(plan.rows[2].textSlot == 0,
          "missing folder-specific text falls back to normal slot");
  require(std::ranges::any_of(plan.commands, [](const auto &command) {
            return command.family == MusicSelectBarDrawFamily::Lamp &&
                   command.slot == 2;
          }),
          "non-rival mode uses the ordinary lamp family");
}

void testUndefinedLongNoteUsesBeatorajaLnModeIndex() {
  auto songList = completeSongList();
  auto frame = completeFrame();
  frame.bars.front().featureFlags = MusicSelectFeatureUndefinedLn;
  frame.playerLnMode = 0;

  const auto lnPlan = MusicSelectBarRenderer{}.plan(songList, frame);
  const auto hasLabel = [](const MusicSelectBarRenderPlan &plan, int slot) {
    return std::ranges::any_of(plan.commands, [slot](const auto &command) {
      return command.family == MusicSelectBarDrawFamily::Label &&
             command.slot == static_cast<std::size_t>(slot);
    });
  };
  require(hasLabel(lnPlan, 0),
          "undefined LN uses Beatoraja PlayerConfig LN index zero");

  frame.playerLnMode = 1;
  const auto cnPlan = MusicSelectBarRenderer{}.plan(songList, frame);
  require(hasLabel(cnPlan, 3),
          "undefined LN uses Beatoraja PlayerConfig CN index one");

  frame.playerLnMode = 2;
  const auto hcnPlan = MusicSelectBarRenderer{}.plan(songList, frame);
  require(hasLabel(hcnPlan, 4),
          "undefined LN uses Beatoraja PlayerConfig HCN index two");
}

void testClickableUsesAuthoredOrderAndInclusiveDestination() {
  auto songList = completeSongList();
  auto frame = completeFrame();
  const auto selected = MusicSelectBarRenderer{}.pointer(
      songList, frame, {.button = 0, .x = 15, .y = 110});
  require(selected.consumed && selected.selectIndex == 0 &&
              !selected.closeDirectory,
          "position one does not move the authored pointer hitbox");
  const auto closed = MusicSelectBarRenderer{}.pointer(
      songList, frame, {.button = 2, .x = 10, .y = 100});
  require(closed.consumed && !closed.selectIndex && closed.closeDirectory,
          "every non-primary pointer button closes the directory");
}

void testMovementInterpolatesTowardThePinnedAdjacentSlot() {
  auto songList = completeSongList();
  auto frame = completeFrame();
  frame.movementDirection = 100;
  frame.wallClockMillis = 1'000;
  frame.movementEndMillis = 1'050;
  const auto plan = MusicSelectBarRenderer{}.plan(songList, frame);
  require(plan.rows[1].x == 15.0,
          "positive movement interpolates halfway toward the following slot");
}

} // namespace

int main(int argc, char **argv) {
  testPinnedDrawOrderSlotsAndClassValues();
  testPositionOneAnchorsChildrenBelowTheBarOrigin();
  testTextFallbackAndNewBoundary();
  testUndefinedLongNoteUsesBeatorajaLnModeIndex();
  testClickableUsesAuthoredOrderAndInclusiveDestination();
  testMovementInterpolatesTowardThePinnedAdjacentSlot();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_bar_renderer_tests", failures,
      "music-select bar renderer test(s) failed",
      "music-select bar renderer tests passed");
}
