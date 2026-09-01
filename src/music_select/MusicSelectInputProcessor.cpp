#include "MusicSelectInputProcessor.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace {

using Key = MusicSelectAssignedKey;

constexpr std::uint64_t bit(Key value) {
  return std::uint64_t{1} << static_cast<unsigned>(value);
}

constexpr std::array<std::uint64_t, 9> kBeat7{{
    bit(Key::Play) | bit(Key::FolderOpen) | bit(Key::Option1Down) |
        bit(Key::JudgeWindowUp) | bit(Key::BgaDown),
    bit(Key::FolderClose) | bit(Key::Option1Up) | bit(Key::Constant) |
        bit(Key::GaugeAutoShiftDown),
    bit(Key::Practice) | bit(Key::FolderOpen) | bit(Key::GaugeDown) |
        bit(Key::JudgeArea) | bit(Key::NotesTimingAutoAdjust),
    bit(Key::FolderClose) | bit(Key::OptionDpDown) | bit(Key::LegacyNote) |
        bit(Key::DurationDown),
    bit(Key::FolderOpen) | bit(Key::Auto) | bit(Key::HsFixDown) |
        bit(Key::MarkNote) | bit(Key::NotesTimingDown),
    bit(Key::NextReplay) | bit(Key::Option2Up) | bit(Key::BpmGuide) |
        bit(Key::DurationUp),
    bit(Key::FolderOpen) | bit(Key::Replay) | bit(Key::Option2Down) |
        bit(Key::NoMine) | bit(Key::NotesTimingUp),
    bit(Key::Up) | bit(Key::TargetUp),
    bit(Key::Down) | bit(Key::TargetDown),
}};

constexpr std::array<std::uint64_t, 9> kPopn9{{
    bit(Key::Auto) | bit(Key::Option1Down) | bit(Key::JudgeWindowUp) |
        bit(Key::BgaDown),
    bit(Key::Option1Up) | bit(Key::Constant) |
        bit(Key::GaugeAutoShiftDown),
    bit(Key::FolderClose) | bit(Key::GaugeDown) | bit(Key::JudgeArea) |
        bit(Key::NotesTimingAutoAdjust),
    bit(Key::Down) | bit(Key::OptionDpDown) | bit(Key::LegacyNote) |
        bit(Key::DurationDown),
    bit(Key::Play) | bit(Key::FolderOpen) | bit(Key::HsFixDown) |
        bit(Key::MarkNote) | bit(Key::NotesTimingDown),
    bit(Key::Up) | bit(Key::Option2Up) | bit(Key::BpmGuide) |
        bit(Key::DurationUp),
    bit(Key::Practice) | bit(Key::FolderOpen) | bit(Key::Option2Down) |
        bit(Key::NoMine) | bit(Key::NotesTimingUp),
    bit(Key::TargetUp) | bit(Key::NextReplay),
    bit(Key::Replay) | bit(Key::TargetDown),
}};

std::uint64_t assignment(MusicSelectKeyLayout layout, std::size_t lane) {
  if (layout == MusicSelectKeyLayout::Popn9K) {
    return lane < kPopn9.size() ? kPopn9[lane] : 0;
  }
  if (layout == MusicSelectKeyLayout::Beat14K && lane >= 9) lane -= 9;
  return lane < kBeat7.size() ? kBeat7[lane] : 0;
}

bool held(const MusicSelectLogicalInput &input, std::size_t lane) {
  return lane < input.keys.size() && input.keys[lane];
}

bool analog(const MusicSelectLogicalInput &input, std::size_t lane) {
  return lane < input.analog.size() && input.analog[lane];
}

bool pressed(MusicSelectLogicalInput &input, MusicSelectKeyLayout layout,
             Key key, bool resetState, bool nonAnalog = false) {
  for (std::size_t lane = 0; lane < input.keys.size(); ++lane) {
    if ((assignment(layout, lane) & bit(key)) == 0 || !held(input, lane) ||
        (nonAnalog && analog(input, lane))) {
      continue;
    }
    if (!resetState) return true;
    if (lane < input.changed.size() && input.changed[lane]) {
      input.changed[lane] = false;
      return true;
    }
  }
  return false;
}

int analogChange(MusicSelectLogicalInput &input, MusicSelectKeyLayout layout,
                 Key key) {
  int ticks = 0;
  for (std::size_t lane = 0; lane < input.keys.size(); ++lane) {
    if ((assignment(layout, lane) & bit(key)) == 0 || !analog(input, lane)) {
      continue;
    }
    if (lane < input.analogDelta.size()) {
      ticks += std::max(0, std::clamp(input.analogDelta[lane], -200, 200));
      input.analogDelta[lane] = 0;
    }
  }
  return ticks;
}

bool control(const MusicSelectLogicalInput &input, MusicSelectControlKey key) {
  return input.controlPressed.contains(key);
}

bool controlHeld(const MusicSelectLogicalInput &input,
                 MusicSelectControlKey key) {
  return input.controlHeld.contains(key);
}

void emitEvent(std::vector<MusicSelectInputAction> &out, int id,
               int argument1 = 0, int argument2 = 0) {
  out.push_back({.kind = MusicSelectInputActionKind::Event,
                 .value = id,
                 .argument1 = argument1,
                 .argument2 = argument2});
}

void emit(std::vector<MusicSelectInputAction> &out,
          MusicSelectInputActionKind kind, int value = 0) {
  out.push_back({.kind = kind, .value = value});
}

} // namespace

MusicSelectInputProcessor::MusicSelectInputProcessor(
    MusicSelectInputProcessorConfig config)
    : config_(config) {}

bool musicSelectKeyAssigned(MusicSelectKeyLayout layout, std::size_t lane,
                            MusicSelectAssignedKey key) noexcept {
  return (assignment(layout, lane) & bit(key)) != 0;
}

std::vector<MusicSelectInputAction> MusicSelectInputProcessor::process(
    MusicSelectLogicalInput input, std::int64_t nowMillis) {
  std::vector<MusicSelectInputAction> out;
  if (control(input, MusicSelectControlKey::Num0))
    emit(out, MusicSelectInputActionKind::SearchPrompt);
  if (control(input, MusicSelectControlKey::Num1)) emitEvent(out, 11);
  if (control(input, MusicSelectControlKey::Num2)) emitEvent(out, 12);
  if (control(input, MusicSelectControlKey::Num3)) emitEvent(out, 308);

  const bool num5 = controlHeld(input, MusicSelectControlKey::Num5);
  if (!input.start && !input.select && !num5) {
    isOptionKeyReleased_ = true;
    if (isOptionKeyPressed_) {
      isOptionKeyPressed_ = false;
      emit(out, MusicSelectInputActionKind::OptionClose);
    }
  }

  if (control(input, MusicSelectControlKey::Num4) ||
      (!input.start && !input.select && !num5 &&
       pressed(input, config_.layout, Key::NextReplay, true))) {
    emit(out, MusicSelectInputActionKind::CommandNextReplay);
  }

  auto openOption = [&] {
    if (isOptionKeyReleased_) {
      isOptionKeyPressed_ = true;
      isOptionKeyReleased_ = false;
      emit(out, MusicSelectInputActionKind::OptionOpen);
    }
  };

  if (input.start && !input.select) {
    emit(out, MusicSelectInputActionKind::ResetBarInput);
    emit(out, MusicSelectInputActionKind::SetPanel, 1);
    openOption();
    if (pressed(input, config_.layout, Key::Option1Down, true))
      emitEvent(out, 42, 1);
    if (pressed(input, config_.layout, Key::Option1Up, true))
      emitEvent(out, 42, -1);
    if (pressed(input, config_.layout, Key::GaugeDown, true))
      emitEvent(out, 40, 1);
    if (pressed(input, config_.layout, Key::GaugeUp, true))
      emitEvent(out, 40, -1);
    if (pressed(input, config_.layout, Key::OptionDpDown, true))
      emitEvent(out, 54, 1);
    if (pressed(input, config_.layout, Key::OptionDpUp, true))
      emitEvent(out, 54, -1);
    if (pressed(input, config_.layout, Key::Option2Down, true))
      emitEvent(out, 43, 1);
    if (pressed(input, config_.layout, Key::Option2Up, true))
      emitEvent(out, 43, -1);
    if (pressed(input, config_.layout, Key::HsFixDown, true))
      emitEvent(out, 55, 1);
    if (pressed(input, config_.layout, Key::HsFixUp, true))
      emitEvent(out, 55, -1);

    int movement = -input.wheel;
    analogScrollBuffer_ +=
        analogChange(input, config_.layout, Key::TargetUp) -
        analogChange(input, config_.layout, Key::TargetDown);
    movement += analogScrollBuffer_ / config_.analogTicksPerScroll;
    analogScrollBuffer_ %= config_.analogTicksPerScroll;
    if (pressed(input, config_.layout, Key::TargetUp, false, true) ||
        controlHeld(input, MusicSelectControlKey::Down)) {
      if (duration_ == 0) {
        movement = 1;
        duration_ = nowMillis + config_.scrollDurationLowMillis;
        angle_ = config_.scrollDurationLowMillis;
      }
      if (nowMillis > duration_) {
        duration_ = nowMillis + config_.scrollDurationHighMillis;
        movement = 1;
        angle_ = config_.scrollDurationHighMillis;
      }
    } else if (pressed(input, config_.layout, Key::TargetDown, false, true) ||
               controlHeld(input, MusicSelectControlKey::Up)) {
      if (duration_ == 0) {
        movement = -1;
        duration_ = nowMillis + config_.scrollDurationLowMillis;
        angle_ = -config_.scrollDurationLowMillis;
      }
      if (nowMillis > duration_) {
        duration_ = nowMillis + config_.scrollDurationHighMillis;
        movement = -1;
        angle_ = -config_.scrollDurationHighMillis;
      }
    } else if (nowMillis > duration_) {
      duration_ = 0;
    }
    while (movement > 0) {
      emitEvent(out, 77, -1);
      emit(out, MusicSelectInputActionKind::ScratchSound);
      --movement;
    }
    while (movement < 0) {
      emitEvent(out, 77, 1);
      emit(out, MusicSelectInputActionKind::ScratchSound);
      ++movement;
    }
  } else if (input.select && !input.start) {
    emit(out, MusicSelectInputActionKind::ResetBarInput);
    emit(out, MusicSelectInputActionKind::SetPanel, 2);
    openOption();
    const std::array<std::pair<Key, MusicSelectInputActionKind>, 7> toggles{{
        {Key::JudgeWindowUp, MusicSelectInputActionKind::ToggleCustomJudge},
        {Key::Constant, MusicSelectInputActionKind::ToggleConstant},
        {Key::JudgeArea, MusicSelectInputActionKind::ToggleShowJudgeArea},
        {Key::LegacyNote, MusicSelectInputActionKind::ToggleLegacyNote},
        {Key::MarkNote, MusicSelectInputActionKind::ToggleMarkProcessedNote},
        {Key::BpmGuide, MusicSelectInputActionKind::ToggleBpmGuide},
        {Key::NoMine, MusicSelectInputActionKind::ToggleNoMine},
    }};
    for (const auto &[key, kind] : toggles) {
      if (pressed(input, config_.layout, key, true)) {
        emit(out, kind);
        emit(out, MusicSelectInputActionKind::OptionChange);
      }
    }
  } else if (num5 || (input.start && input.select)) {
    emit(out, MusicSelectInputActionKind::ResetBarInput);
    emit(out, MusicSelectInputActionKind::SetPanel, 3);
    openOption();
    if (pressed(input, config_.layout, Key::BgaDown, true)) emitEvent(out, 72);
    if (pressed(input, config_.layout, Key::GaugeAutoShiftDown, true))
      emitEvent(out, 78);
    if (pressed(input, config_.layout, Key::NotesTimingDown, true))
      emitEvent(out, 74, -1);
    const bool durationDown =
        pressed(input, config_.layout, Key::DurationDown, false);
    const bool durationUp =
        !durationDown && pressed(input, config_.layout, Key::DurationUp, false);
    if (durationDown || durationUp) {
      const int direction = durationDown ? -1 : 1;
      if (timeChangeDuration_ == 0) {
        timeChangeDuration_ = nowMillis + config_.scrollDurationLowMillis;
        emitEvent(out, 59, direction);
      } else if (nowMillis > timeChangeDuration_) {
        ++countChangeDuration_;
        timeChangeDuration_ = nowMillis + config_.scrollDurationHighMillis;
        emitEvent(out, 59, direction, countChangeDuration_ > 50 ? 10 : 0);
      }
    } else {
      timeChangeDuration_ = 0;
      countChangeDuration_ = 0;
    }
    if (pressed(input, config_.layout, Key::NotesTimingUp, true))
      emitEvent(out, 74);
    if (pressed(input, config_.layout, Key::NotesTimingAutoAdjust, true))
      emitEvent(out, 75);
  } else {
    emit(out, MusicSelectInputActionKind::BarInput);
    emit(out, MusicSelectInputActionKind::SetPanel, 0);
    int movement = -input.wheel;
    analogScrollBuffer_ += analogChange(input, config_.layout, Key::Up) -
                           analogChange(input, config_.layout, Key::Down);
    movement += analogScrollBuffer_ / config_.analogTicksPerScroll;
    analogScrollBuffer_ %= config_.analogTicksPerScroll;
    if (movement != 0) {
      const int remainingScroll =
          angle_ == 0
              ? 0
              : static_cast<int>(std::max<std::int64_t>(
                    0, duration_ - nowMillis)) /
                    angle_;
      const int remaining = std::clamp(remainingScroll + movement, -2, 2);
      if (remaining == 0) {
        angle_ = 0;
        duration_ = nowMillis;
      } else {
        const int scrollDuration = 120 / remaining / remaining;
        angle_ = scrollDuration / remaining;
        duration_ = nowMillis + scrollDuration;
      }
    }
    if (pressed(input, config_.layout, Key::Up, false, true) ||
        controlHeld(input, MusicSelectControlKey::Down)) {
      if (duration_ == 0) {
        normalBarKeyInput_ = true;
        movement = 1;
        duration_ = nowMillis + config_.scrollDurationLowMillis;
        angle_ = config_.scrollDurationLowMillis;
      }
      if (nowMillis > duration_ && normalBarKeyInput_) {
        duration_ = nowMillis + config_.scrollDurationHighMillis;
        movement = 1;
        angle_ = config_.scrollDurationHighMillis;
      }
    } else if (pressed(input, config_.layout, Key::Down, false, true) ||
        controlHeld(input, MusicSelectControlKey::Up)) {
      if (duration_ == 0) {
        normalBarKeyInput_ = true;
        movement = -1;
        duration_ = nowMillis + config_.scrollDurationLowMillis;
        angle_ = -config_.scrollDurationLowMillis;
      }
      if (nowMillis > duration_ && normalBarKeyInput_) {
        duration_ = nowMillis + config_.scrollDurationHighMillis;
        movement = -1;
        angle_ = -config_.scrollDurationHighMillis;
      }
    } else {
      normalBarKeyInput_ = false;
    }
    if (nowMillis > duration_ && !normalBarKeyInput_) {
      duration_ = 0;
    }
    while (movement > 0) {
      emit(out, MusicSelectInputActionKind::MoveNext);
      emit(out, MusicSelectInputActionKind::ScratchSound);
      --movement;
    }
    while (movement < 0) {
      emit(out, MusicSelectInputActionKind::MovePrevious);
      emit(out, MusicSelectInputActionKind::ScratchSound);
      ++movement;
    }
    if (input.currentBar == MusicSelectInputBarKind::Selectable) {
      if (pressed(input, config_.layout, Key::Play, true) ||
          control(input, MusicSelectControlKey::Right) ||
          control(input, MusicSelectControlKey::Enter)) {
        emit(out, MusicSelectInputActionKind::Play);
      } else if (pressed(input, config_.layout, Key::Practice, true)) {
        emit(out, input.eventMode ? MusicSelectInputActionKind::Play
                                  : MusicSelectInputActionKind::Practice);
      } else if (pressed(input, config_.layout, Key::Auto, true)) {
        emit(out, input.eventMode ? MusicSelectInputActionKind::Play
                                  : MusicSelectInputActionKind::Autoplay);
      } else if (pressed(input, config_.layout, Key::Replay, true)) {
        if (input.eventMode || input.selectedReplay < 0) {
          emit(out, MusicSelectInputActionKind::Play);
        } else {
          emit(out, MusicSelectInputActionKind::Replay,
               input.selectedReplay);
        }
      }
    } else if (input.currentBar == MusicSelectInputBarKind::Directory &&
               (pressed(input, config_.layout, Key::FolderOpen, true) ||
                control(input, MusicSelectControlKey::Right) ||
                control(input, MusicSelectControlKey::Enter))) {
      emit(out, MusicSelectInputActionKind::OpenFolder);
    }
    if (control(input, MusicSelectControlKey::Num7)) emitEvent(out, 79);
    if (control(input, MusicSelectControlKey::Num8))
      emit(out, MusicSelectInputActionKind::CommandSameFolder);
    if (control(input, MusicSelectControlKey::Num9)) emitEvent(out, 17);
    if (pressed(input, config_.layout, Key::FolderClose, true) ||
        control(input, MusicSelectControlKey::Left)) {
      emit(out, MusicSelectInputActionKind::CloseFolder);
    }
    if (input.commands.contains(MusicSelectCommandKey::AutoplayFolder) &&
        input.currentBar == MusicSelectInputBarKind::Directory)
      emit(out, MusicSelectInputActionKind::AutoplayFolder);
    if (input.commands.contains(MusicSelectCommandKey::OpenIr))
      emitEvent(out, 210);
    if (input.commands.contains(MusicSelectCommandKey::AddFavoriteSong))
      emitEvent(out, 89);
    if (input.commands.contains(MusicSelectCommandKey::AddFavoriteChart))
      emitEvent(out, 90);
  }

  if (input.selectedBarChanged)
    emit(out, MusicSelectInputActionKind::SelectedBarMoved);
  emit(out, MusicSelectInputActionKind::SongBarChangeTimer);
  if (input.commands.contains(MusicSelectCommandKey::UpdateFolder))
    emitEvent(out, 211);
  if (input.commands.contains(MusicSelectCommandKey::OpenExplorer))
    emitEvent(out, 212);
  if (input.commands.contains(MusicSelectCommandKey::CopyMd5))
    emit(out, MusicSelectInputActionKind::CopyMd5);
  if (input.commands.contains(MusicSelectCommandKey::CopySha256))
    emit(out, MusicSelectInputActionKind::CopySha256);
  if (control(input, MusicSelectControlKey::Escape))
    emit(out, MusicSelectInputActionKind::ExitApplication);
  return out;
}
