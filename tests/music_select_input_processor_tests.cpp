#include "music_select/MusicSelectInputProcessor.h"
#include "music_select_skin_ledger_evidence.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Assigned = MusicSelectAssignedKey;

int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool has(const std::vector<MusicSelectInputAction> &actions,
         MusicSelectInputActionKind kind, int value = 0) {
  return std::ranges::any_of(actions, [&](const auto &action) {
    return action.kind == kind && action.value == value;
  });
}

bool hasEvent(const std::vector<MusicSelectInputAction> &actions, int id,
              int argument1 = 0, int argument2 = 0) {
  return std::ranges::any_of(actions, [&](const auto &action) {
    return action.kind == MusicSelectInputActionKind::Event &&
           action.value == id && action.argument1 == argument1 &&
           action.argument2 == argument2;
  });
}

MusicSelectLogicalInput emptyInput() {
  MusicSelectLogicalInput input;
  input.keys.resize(18);
  input.changed.resize(18);
  input.analog.resize(18);
  input.analogDelta.resize(18);
  return input;
}

MusicSelectLogicalInput key(int index) {
  auto input = emptyInput();
  input.keys[static_cast<std::size_t>(index)] = true;
  input.changed[static_cast<std::size_t>(index)] = true;
  return input;
}

constexpr std::array kAllAssignedKeys{
    Assigned::Play,
    Assigned::Auto,
    Assigned::Replay,
    Assigned::Up,
    Assigned::Down,
    Assigned::FolderOpen,
    Assigned::FolderClose,
    Assigned::Practice,
    Assigned::Option1Up,
    Assigned::Option1Down,
    Assigned::GaugeUp,
    Assigned::GaugeDown,
    Assigned::OptionDpUp,
    Assigned::OptionDpDown,
    Assigned::HsFixUp,
    Assigned::HsFixDown,
    Assigned::Option2Up,
    Assigned::Option2Down,
    Assigned::TargetUp,
    Assigned::TargetDown,
    Assigned::JudgeArea,
    Assigned::NoMine,
    Assigned::BpmGuide,
    Assigned::LegacyNote,
    Assigned::Constant,
    Assigned::JudgeWindowUp,
    Assigned::JudgeWindowDown,
    Assigned::MarkNote,
    Assigned::BgaUp,
    Assigned::BgaDown,
    Assigned::GaugeAutoShiftUp,
    Assigned::GaugeAutoShiftDown,
    Assigned::DurationUp,
    Assigned::DurationDown,
    Assigned::NotesTimingUp,
    Assigned::NotesTimingDown,
    Assigned::NotesTimingAutoAdjust,
    Assigned::NextReplay,
};

using Assignment = std::set<Assigned>;

const std::array<Assignment, 9> kBeat7{{
    {Assigned::Play, Assigned::FolderOpen, Assigned::Option1Down,
     Assigned::JudgeWindowUp, Assigned::BgaDown},
    {Assigned::FolderClose, Assigned::Option1Up, Assigned::Constant,
     Assigned::GaugeAutoShiftDown},
    {Assigned::Practice, Assigned::FolderOpen, Assigned::GaugeDown,
     Assigned::JudgeArea, Assigned::NotesTimingAutoAdjust},
    {Assigned::FolderClose, Assigned::OptionDpDown, Assigned::LegacyNote,
     Assigned::DurationDown},
    {Assigned::FolderOpen, Assigned::Auto, Assigned::HsFixDown,
     Assigned::MarkNote, Assigned::NotesTimingDown},
    {Assigned::NextReplay, Assigned::Option2Up, Assigned::BpmGuide,
     Assigned::DurationUp},
    {Assigned::FolderOpen, Assigned::Replay, Assigned::Option2Down,
     Assigned::NoMine, Assigned::NotesTimingUp},
    {Assigned::Up, Assigned::TargetUp},
    {Assigned::Down, Assigned::TargetDown},
}};

const std::array<Assignment, 9> kPopn9{{
    {Assigned::Auto, Assigned::Option1Down, Assigned::JudgeWindowUp,
     Assigned::BgaDown},
    {Assigned::Option1Up, Assigned::Constant,
     Assigned::GaugeAutoShiftDown},
    {Assigned::FolderClose, Assigned::GaugeDown, Assigned::JudgeArea,
     Assigned::NotesTimingAutoAdjust},
    {Assigned::Down, Assigned::OptionDpDown, Assigned::LegacyNote,
     Assigned::DurationDown},
    {Assigned::Play, Assigned::FolderOpen, Assigned::HsFixDown,
     Assigned::MarkNote, Assigned::NotesTimingDown},
    {Assigned::Up, Assigned::Option2Up, Assigned::BpmGuide,
     Assigned::DurationUp},
    {Assigned::Practice, Assigned::FolderOpen, Assigned::Option2Down,
     Assigned::NoMine, Assigned::NotesTimingUp},
    {Assigned::TargetUp, Assigned::NextReplay},
    {Assigned::Replay, Assigned::TargetDown},
}};

void assertMatrix(MusicSelectKeyLayout layout,
                  const std::array<Assignment, 9> &expected,
                  bool doubled) {
  const std::size_t lanes = doubled ? 18 : 9;
  for (std::size_t lane = 0; lane < lanes; ++lane) {
    const auto &laneExpected = expected[doubled && lane >= 9 ? lane - 9 : lane];
    for (const auto keyValue : kAllAssignedKeys) {
      require(musicSelectKeyAssigned(layout, lane, keyValue) ==
                  laneExpected.contains(keyValue),
              "music-select key matrix matches the pinned source");
    }
  }
}

void testSourceKeyMatricesAreLiteralAndComplete() {
  assertMatrix(MusicSelectKeyLayout::Beat7K, kBeat7, false);
  assertMatrix(MusicSelectKeyLayout::Popn9K, kPopn9, false);
  assertMatrix(MusicSelectKeyLayout::Beat14K, kBeat7, true);
  for (const auto keyValue : kAllAssignedKeys) {
    require(!musicSelectKeyAssigned(MusicSelectKeyLayout::Beat7K, 9,
                                    keyValue) &&
                !musicSelectKeyAssigned(MusicSelectKeyLayout::Popn9K, 9,
                                        keyValue) &&
                !musicSelectKeyAssigned(MusicSelectKeyLayout::Beat14K, 18,
                                        keyValue),
            "out-of-matrix key indexes have no assignment");
  }
}

void testNormalBarBranches() {
  MusicSelectInputProcessor processor({});
  require(has(processor.process(key(0), 1), MusicSelectInputActionKind::Play),
          "BEAT_7K lane 1 plays a selectable bar");
  require(has(processor.process(key(2), 2),
              MusicSelectInputActionKind::Practice),
          "BEAT_7K lane 3 starts practice");
  require(has(processor.process(key(4), 3),
              MusicSelectInputActionKind::Autoplay),
          "BEAT_7K lane 5 starts autoplay");

  auto replay = key(6);
  replay.selectedReplay = 2;
  require(has(processor.process(replay, 4),
              MusicSelectInputActionKind::Replay, 2),
          "replay uses the selected replay slot");
  (void)processor.process(emptyInput(), 5);
  replay.selectedReplay = -1;
  require(has(processor.process(replay, 6),
              MusicSelectInputActionKind::Play),
          "missing selected replay follows source play fallback");

  auto eventPractice = key(2);
  eventPractice.eventMode = true;
  require(has(processor.process(eventPractice, 6),
              MusicSelectInputActionKind::Play),
          "event mode turns practice into play");
  auto eventAuto = key(4);
  eventAuto.eventMode = true;
  require(has(processor.process(eventAuto, 7),
              MusicSelectInputActionKind::Play),
          "event mode turns autoplay into play");
  auto eventReplay = key(6);
  eventReplay.eventMode = true;
  eventReplay.selectedReplay = 0;
  require(has(processor.process(eventReplay, 8),
              MusicSelectInputActionKind::Play),
          "event mode turns replay into play");

  auto directory = key(0);
  directory.currentBar = MusicSelectInputBarKind::Directory;
  require(has(processor.process(directory, 9),
              MusicSelectInputActionKind::OpenFolder),
          "folder-open assignment opens a directory");
  auto close = key(1);
  close.currentBar = MusicSelectInputBarKind::Directory;
  require(has(processor.process(close, 10),
              MusicSelectInputActionKind::CloseFolder),
          "folder-close assignment closes a directory");

  MusicSelectInputProcessor popn({.layout = MusicSelectKeyLayout::Popn9K});
  require(has(popn.process(key(0), 11),
              MusicSelectInputActionKind::Autoplay) &&
              has(popn.process(key(4), 12),
                  MusicSelectInputActionKind::Play) &&
              has(popn.process(key(6), 13),
                  MusicSelectInputActionKind::Practice),
          "POPN_9K uses its source play/auto/practice positions");
  MusicSelectInputProcessor double7(
      {.layout = MusicSelectKeyLayout::Beat14K});
  require(has(double7.process(key(9), 14),
              MusicSelectInputActionKind::Play),
          "BEAT_14K repeats the 7K assignments for player two");
}

void testPanelPriorityAndEveryPanelAssignment() {
  MusicSelectInputProcessor processor({});
  (void)processor.process(emptyInput(), 9);

  const std::array<std::pair<int, std::pair<int, int>>, 7> playOptions{{
      {0, {42, 1}}, {1, {42, -1}}, {2, {40, 1}}, {3, {54, 1}},
      {4, {55, 1}}, {5, {43, -1}}, {6, {43, 1}},
  }};
  for (const auto &[lane, event] : playOptions) {
    auto input = key(lane);
    input.start = true;
    const auto actions = processor.process(input, 10 + lane);
    require(has(actions, MusicSelectInputActionKind::SetPanel, 1) &&
                hasEvent(actions, event.first, event.second) &&
                !has(actions, MusicSelectInputActionKind::Play),
            "start-only panel consumes every source play-option assignment");
  }

  const auto close = processor.process(emptyInput(), 20);
  require(has(close, MusicSelectInputActionKind::OptionClose) &&
              has(close, MusicSelectInputActionKind::SetPanel, 0),
          "releasing option keys closes the panel before normal input");

  constexpr std::array assistActions{
      MusicSelectInputActionKind::ToggleCustomJudge,
      MusicSelectInputActionKind::ToggleConstant,
      MusicSelectInputActionKind::ToggleShowJudgeArea,
      MusicSelectInputActionKind::ToggleLegacyNote,
      MusicSelectInputActionKind::ToggleMarkProcessedNote,
      MusicSelectInputActionKind::ToggleBpmGuide,
      MusicSelectInputActionKind::ToggleNoMine,
  };
  for (std::size_t lane = 0; lane < assistActions.size(); ++lane) {
    auto input = key(static_cast<int>(lane));
    input.select = true;
    const auto actions = processor.process(input, 30 + lane);
    require(has(actions, MusicSelectInputActionKind::SetPanel, 2) &&
                has(actions, assistActions[lane]) &&
                has(actions, MusicSelectInputActionKind::OptionChange),
            "select-only panel consumes every source assist assignment");
  }

  const std::array<std::pair<int, std::pair<int, int>>, 7> details{{
      {0, {72, 0}}, {1, {78, 0}}, {2, {75, 0}}, {3, {59, -1}},
      {4, {74, -1}}, {5, {59, 1}}, {6, {74, 0}},
  }};
  for (const auto &[lane, event] : details) {
    auto input = key(lane);
    input.start = true;
    input.select = true;
    const auto actions = processor.process(input, 50 + lane);
    require(has(actions, MusicSelectInputActionKind::SetPanel, 3) &&
                hasEvent(actions, event.first, event.second),
            "start+select consumes every source detail assignment");
  }
  auto num5 = key(0);
  num5.controlHeld.insert(MusicSelectControlKey::Num5);
  const auto num5Actions = processor.process(num5, 60);
  require(has(num5Actions, MusicSelectInputActionKind::SetPanel, 3) &&
              hasEvent(num5Actions, 72),
          "held NUM5 opens the same detail panel as start+select");
}

void testWheelAnalogAndRepeatState() {
  MusicSelectInputProcessor target(
      {.scrollDurationLowMillis = 100,
       .scrollDurationHighMillis = 30,
       .analogTicksPerScroll = 4});
  auto input = key(7);
  input.start = true;
  input.analog[7] = true;
  input.analogDelta[7] = 9;
  input.wheel = -1;
  const auto first = target.process(input, 1000);
  require(std::ranges::count_if(first, [](const auto &action) {
            return action.kind == MusicSelectInputActionKind::Event &&
                   action.value == 77 && action.argument1 == -1;
          }) == 3,
          "wheel and analog target-up ticks accumulate before target events");

  auto negativeAnalog = key(7);
  negativeAnalog.start = true;
  negativeAnalog.analog[7] = true;
  negativeAnalog.analogDelta[7] = -200;
  const auto negative = target.process(negativeAnalog, 1001);
  require(std::ranges::none_of(negative, [](const auto &action) {
            return action.kind == MusicSelectInputActionKind::Event &&
                   action.value == 77;
          }),
          "negative analog differences are discarded by getAnalogDiffAndReset");

  MusicSelectInputProcessor repeat(
      {.scrollDurationLowMillis = 100,
       .scrollDurationHighMillis = 30,
       .analogTicksPerScroll = 4});
  auto held = key(7);
  (void)repeat.process(held, 1000);
  const auto before = repeat.process(held, 1050);
  const auto after = repeat.process(held, 1101);
  require(std::ranges::none_of(before, [](const auto &action) {
            return action.kind == MusicSelectInputActionKind::MoveNext;
          }) &&
              has(after, MusicSelectInputActionKind::MoveNext),
          "ordinary non-analog repeat waits for durationlow then durationhigh");

  MusicSelectInputProcessor interrupted(
      {.scrollDurationLowMillis = 100,
       .scrollDurationHighMillis = 30,
       .analogTicksPerScroll = 4});
  (void)interrupted.process(held, 1000);
  (void)interrupted.process(emptyInput(), 1050);
  const auto repressed = interrupted.process(held, 1101);
  require(!has(repressed, MusicSelectInputActionKind::MoveNext),
          "BarRenderer keyinput stays false when a key is re-pressed before "
          "the pending duration expires");
}

void testDurationAccelerationAfterFiftyRepeats() {
  MusicSelectInputProcessor processor(
      {.scrollDurationLowMillis = 100,
       .scrollDurationHighMillis = 30,
       .analogTicksPerScroll = 3});
  auto held = key(3);
  held.start = true;
  held.select = true;
  require(hasEvent(processor.process(held, 1000), 59, -1, 0),
          "duration change emits one unit on initial press");
  bool accelerated = false;
  for (int repeat = 1; repeat <= 51; ++repeat) {
    const auto actions = processor.process(held, 1100 + repeat * 31);
    if (repeat == 51) accelerated = hasEvent(actions, 59, -1, 10);
  }
  require(accelerated,
          "duration change emits ten units only after fifty repeat events");
  auto releasedInDetail = emptyInput();
  releasedInDetail.start = true;
  releasedInDetail.select = true;
  (void)processor.process(releasedInDetail, 3000);
  require(hasEvent(processor.process(held, 3001), 59, -1, 0),
          "releasing duration input resets its repeat counter");
}

void testControlsCommandsPostActionsAndExitOrder() {
  MusicSelectInputProcessor processor({});
  auto input = emptyInput();
  input.currentBar = MusicSelectInputBarKind::Directory;
  input.selectedBarChanged = true;
  input.controlPressed = {MusicSelectControlKey::Num0,
                          MusicSelectControlKey::Num1,
                          MusicSelectControlKey::Num2,
                          MusicSelectControlKey::Num3,
                          MusicSelectControlKey::Num4,
                          MusicSelectControlKey::Num7,
                          MusicSelectControlKey::Num8,
                          MusicSelectControlKey::Num9,
                          MusicSelectControlKey::Right,
                          MusicSelectControlKey::Left,
                          MusicSelectControlKey::Escape};
  input.commands = {MusicSelectCommandKey::AutoplayFolder,
                    MusicSelectCommandKey::OpenIr,
                    MusicSelectCommandKey::AddFavoriteSong,
                    MusicSelectCommandKey::AddFavoriteChart,
                    MusicSelectCommandKey::UpdateFolder,
                    MusicSelectCommandKey::OpenExplorer,
                    MusicSelectCommandKey::CopyMd5,
                    MusicSelectCommandKey::CopySha256};
  const auto actions = processor.process(input, 0);
  require(actions.size() >= 20 &&
              actions[0].kind == MusicSelectInputActionKind::SearchPrompt &&
              actions[1].kind == MusicSelectInputActionKind::Event &&
              actions[1].value == 11 && actions[2].value == 12 &&
              actions[3].value == 308 &&
              actions[4].kind ==
                  MusicSelectInputActionKind::CommandNextReplay,
          "NUM search, mode, sort, LN, and replay commands retain source order");
  require(has(actions, MusicSelectInputActionKind::OpenFolder) &&
              has(actions, MusicSelectInputActionKind::CloseFolder) &&
              has(actions, MusicSelectInputActionKind::AutoplayFolder) &&
              has(actions, MusicSelectInputActionKind::CommandSameFolder) &&
              hasEvent(actions, 79) && hasEvent(actions, 17) &&
              hasEvent(actions, 210) && hasEvent(actions, 89) &&
              hasEvent(actions, 90) && hasEvent(actions, 211) &&
              hasEvent(actions, 212) &&
              has(actions, MusicSelectInputActionKind::CopyMd5) &&
              has(actions, MusicSelectInputActionKind::CopySha256),
          "every source control and KeyCommand branch emits its action");
  const auto moved = std::ranges::find_if(actions, [](const auto &action) {
    return action.kind == MusicSelectInputActionKind::SelectedBarMoved;
  });
  const auto timer = std::ranges::find_if(actions, [](const auto &action) {
    return action.kind == MusicSelectInputActionKind::SongBarChangeTimer;
  });
  const auto update = std::ranges::find_if(actions, [](const auto &action) {
    return action.kind == MusicSelectInputActionKind::Event &&
           action.value == 211;
  });
  require(moved < timer && timer < update &&
              actions.back().kind ==
                  MusicSelectInputActionKind::ExitApplication,
          "selection, timer, post commands, and ESC retain source order");
}

std::vector<std::string> ledgerIds() {
  return {
      "select.behavior.music-select-input-processor.input-0",
      "select.behavior.music-select-key-property.get-analog-change-2",
      "select.behavior.music-select-key-property.is-non-analog-pressed-3",
      "select.behavior.music-select-key-property.is-pressed-3",
      "select.constant.music-select-key-property.values",
      "select.input.music-select-input-processor",
      "select.input.music-select-key-property.beat-14-k",
      "select.input.music-select-key-property.beat-7-k",
      "select.input.music-select-key-property.popn-9-k",
      "select.input.music-select-key.auto",
      "select.input.music-select-key.bga-down",
      "select.input.music-select-key.bga-up",
      "select.input.music-select-key.bpmguide",
      "select.input.music-select-key.constant",
      "select.input.music-select-key.down",
      "select.input.music-select-key.duration-down",
      "select.input.music-select-key.duration-up",
      "select.input.music-select-key.folder-close",
      "select.input.music-select-key.folder-open",
      "select.input.music-select-key.gauge-down",
      "select.input.music-select-key.gauge-up",
      "select.input.music-select-key.gaugeautoshift-down",
      "select.input.music-select-key.gaugeautoshift-up",
      "select.input.music-select-key.hsfix-down",
      "select.input.music-select-key.hsfix-up",
      "select.input.music-select-key.judgearea",
      "select.input.music-select-key.judgewindow-down",
      "select.input.music-select-key.judgewindow-up",
      "select.input.music-select-key.legacynote",
      "select.input.music-select-key.marknote",
      "select.input.music-select-key.next-replay",
      "select.input.music-select-key.nomine",
      "select.input.music-select-key.notesdisplaytiming-autoadjust",
      "select.input.music-select-key.notesdisplaytiming-down",
      "select.input.music-select-key.notesdisplaytiming-up",
      "select.input.music-select-key.option1-down",
      "select.input.music-select-key.option1-up",
      "select.input.music-select-key.option2-down",
      "select.input.music-select-key.option2-up",
      "select.input.music-select-key.optiondp-down",
      "select.input.music-select-key.optiondp-up",
      "select.input.music-select-key.play",
      "select.input.music-select-key.practice",
      "select.input.music-select-key.replay",
      "select.input.music-select-key.target-down",
      "select.input.music-select-key.target-up",
      "select.input.music-select-key.up",
  };
}

} // namespace

int main(int argc, char **argv) {
  testSourceKeyMatricesAreLiteralAndComplete();
  testNormalBarBranches();
  testPanelPriorityAndEveryPanelAssignment();
  testWheelAnalogAndRepeatState();
  testDurationAccelerationAfterFiftyRepeats();
  testControlsCommandsPostActionsAndExitOrder();
  return music_select_skin_ledger_evidence::finish(
      argc, argv, "music_select_input_processor_tests", failures, ledgerIds(),
      "music-select input processor assertion(s) failed",
      "music-select input processor tests passed");
}
