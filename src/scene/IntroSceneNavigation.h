#pragma once

#include "../music_select/MusicSelectInputProcessor.h"

#include <cstdint>
#include <optional>
#include <utility>

enum class IntroSceneChoice : std::uint8_t {
  Start,
  Settings,
};

struct IntroSceneNavigationResult {
  bool selectionChanged = false;
  std::optional<IntroSceneChoice> activated;
};

class IntroSceneNavigation final {
public:
  explicit IntroSceneNavigation(MusicSelectKeyLayout layout)
      : processor_({.layout = layout}) {}

  void reset(MusicSelectKeyLayout layout) {
    processor_ = MusicSelectInputProcessor({.layout = layout});
    choice_ = IntroSceneChoice::Start;
    startHeld_ = false;
  }

  [[nodiscard]] IntroSceneNavigationResult
  process(MusicSelectLogicalInput input, std::int64_t nowMillis) {
    IntroSceneNavigationResult result;
    const bool startPressed = input.start && !startHeld_;
    startHeld_ = input.start;
    input.currentBar = MusicSelectInputBarKind::Selectable;
    for (const auto &action : processor_.process(std::move(input), nowMillis)) {
      IntroSceneChoice next = choice_;
      switch (action.kind) {
      case MusicSelectInputActionKind::MoveNext:
        next = IntroSceneChoice::Settings;
        break;
      case MusicSelectInputActionKind::MovePrevious:
        next = IntroSceneChoice::Start;
        break;
      case MusicSelectInputActionKind::Play:
        result.activated = choice_;
        break;
      default:
        break;
      }
      if (next != choice_) {
        choice_ = next;
        result.selectionChanged = true;
      }
    }
    if (startPressed) result.activated = choice_;
    return result;
  }

  [[nodiscard]] IntroSceneChoice choice() const noexcept { return choice_; }

private:
  MusicSelectInputProcessor processor_;
  IntroSceneChoice choice_ = IntroSceneChoice::Start;
  bool startHeld_ = false;
};
