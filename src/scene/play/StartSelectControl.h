#pragma once

#include "../../replay/ReplayPlayback.h"

#include <array>
#include <cstdint>
#include <vector>

namespace gameplay {

// This is a direct state-machine port of the Start/Select branches in
// Beatoraja's ControlInputProcessor.  It deliberately consumes the canonical
// BRD control stream, so physical and virtual controllers use the same
// gesture semantics.
enum class StartSelectControlActionKind : std::uint8_t {
  AdjustHispeed,
  AdjustDuration,
  AdjustLaneCover,
  ToggleLaneCover,
  ToggleLiftHiddenTarget,
  Exit,
};

struct StartSelectControlAction {
  StartSelectControlActionKind kind =
      StartSelectControlActionKind::AdjustHispeed;
  int delta = 0;

  bool operator==(const StartSelectControlAction &) const = default;
};

struct StartSelectControlFrameContext {
  bool noteEnd = false;
};

struct StartSelectControlInput {
  replay::LogicalControl control;
  bool pressed = false;
  std::int64_t timestampMicros = 0;
};

class StartSelectControl {
public:
  struct Configuration {
    int keyMode = 7;
    std::int64_t doubleStartWindowMicros = 500'000;
    std::int64_t exitHoldDurationMicros = 1'000'000;
    std::int64_t scratchRepeatMicros = 50'000;
    std::int64_t scratchFastRepeatAfterMicros = 500'000;
  };

  explicit StartSelectControl(Configuration configuration)
      : configuration_(configuration) {}

  [[nodiscard]] std::vector<StartSelectControlAction>
  apply(const replay::LogicalControl &control, bool pressed,
        std::int64_t timestampMicros,
        StartSelectControlFrameContext context = {}) {
    std::vector<StartSelectControlAction> actions;
    switch (control.kind) {
    case replay::LogicalControlKind::Start:
      applyStart(pressed, timestampMicros, actions);
      break;
    case replay::LogicalControlKind::Select:
      applySelect(pressed, timestampMicros, actions);
      break;
    case replay::LogicalControlKind::Lane:
      applyLane(control, pressed, timestampMicros, actions);
      break;
    case replay::LogicalControlKind::ScratchClockwise:
      applyScratch(control.player, 1, pressed, timestampMicros);
      break;
    case replay::LogicalControlKind::ScratchCounterClockwise:
      applyScratch(control.player, -1, pressed, timestampMicros);
      break;
    }
    appendConjunctionActions(timestampMicros, context, actions);
    return actions;
  }

  [[nodiscard]] std::vector<StartSelectControlAction>
  tick(std::int64_t timestampMicros,
       StartSelectControlFrameContext context = {}) {
    std::vector<StartSelectControlAction> actions;
    appendScratchActions(timestampMicros, actions);
    appendConjunctionActions(timestampMicros, context, actions);
    return actions;
  }

  void reset() noexcept {
    startHeld_ = false;
    selectHeld_ = false;
    conjunctionHeld_ = false;
    exitIssued_ = false;
    for (auto &scratch : scratch_) {
      scratch = {};
    }
  }

private:
  struct ScratchState {
    int direction = 0;
    std::int64_t heldSinceMicros = 0;
    std::int64_t lastAppliedMicros = 0;
  };

  [[nodiscard]] int laneBinding(int lane) const noexcept {
    switch (configuration_.keyMode) {
    case 5:
    case 10: {
      constexpr std::array bindings{-1, 1, -1, 1, -1};
      return lane >= 0 && lane < static_cast<int>(bindings.size())
                 ? bindings[static_cast<std::size_t>(lane)]
                 : 0;
    }
    case 7:
    case 14: {
      constexpr std::array bindings{-1, 1, -1, 1, -1, 1, -1};
      return lane >= 0 && lane < static_cast<int>(bindings.size())
                 ? bindings[static_cast<std::size_t>(lane)]
                 : 0;
    }
    case 9: {
      constexpr std::array bindings{-1, 1, -1, 1, -1, 1, -1, 2, -2};
      return lane >= 0 && lane < static_cast<int>(bindings.size())
                 ? bindings[static_cast<std::size_t>(lane)]
                 : 0;
    }
    case 24:
    case 48: {
      constexpr std::array bindings{
          -1, 1, -1, 1, -1, -1, 1, -1, 1, -1, 1, -1,
          -1, 1, -1, 1, -1, -1, 1, -1, 1, -1, 1, -1,
          -2, 2,
      };
      return lane >= 0 && lane < static_cast<int>(bindings.size())
                 ? bindings[static_cast<std::size_t>(lane)]
                 : 0;
    }
    default:
      return 0;
    }
  }

  [[nodiscard]] static std::size_t playerIndex(int player) noexcept {
    return player == 2 ? 1U : 0U;
  }

  void applyStart(bool pressed, std::int64_t timestampMicros,
                  std::vector<StartSelectControlAction> &actions) {
    if (pressed == startHeld_) {
      return;
    }
    if (pressed && !selectHeld_) {
      if (lastStartPressedMicros_ != 0 &&
          timestampMicros - lastStartPressedMicros_ <
              configuration_.doubleStartWindowMicros) {
        actions.push_back({.kind = StartSelectControlActionKind::ToggleLaneCover});
        lastStartPressedMicros_ = 0;
      } else {
        lastStartPressedMicros_ = timestampMicros;
      }
    }
    startHeld_ = pressed;
  }

  void applySelect(bool pressed, std::int64_t,
                   std::vector<StartSelectControlAction> &) {
    if (pressed == selectHeld_) {
      return;
    }
    selectHeld_ = pressed;
  }

  void applyLane(const replay::LogicalControl &control, bool pressed,
                 std::int64_t timestampMicros,
                 std::vector<StartSelectControlAction> &actions) {
    const int binding = laneBinding(control.lane);
    // Beatoraja's +/-2 bindings are held digital controls (the final two
    // Pop'n/keyboard inputs), not two-step one-shot changes. Route them
    // through the same repeat state as a scratch control.
    if (binding == 2 || binding == -2) {
      applyScratch(control.player, binding > 0 ? 1 : -1, pressed,
                   timestampMicros);
      return;
    }
    if (!pressed || startHeld_ == selectHeld_) {
      return;
    }
    if (binding == 0) {
      return;
    }
    actions.push_back(
        {.kind = startHeld_ ? StartSelectControlActionKind::AdjustHispeed
                            : StartSelectControlActionKind::AdjustDuration,
         .delta = binding});
  }

  void applyScratch(int player, int direction, bool pressed,
                    std::int64_t timestampMicros) noexcept {
    auto &scratch = scratch_[playerIndex(player)];
    if (pressed) {
      if (scratch.direction == direction) {
        return;
      }
      scratch = {.direction = direction, .heldSinceMicros = timestampMicros};
      return;
    }
    if (scratch.direction == direction) {
      scratch = {};
    }
  }

  void appendScratchActions(
      std::int64_t timestampMicros,
      std::vector<StartSelectControlAction> &actions) {
    if (startHeld_ == selectHeld_) {
      return;
    }
    for (auto &scratch : scratch_) {
      if (scratch.direction == 0 ||
          timestampMicros - scratch.lastAppliedMicros <=
              configuration_.scratchRepeatMicros) {
        continue;
      }
      scratch.lastAppliedMicros = timestampMicros;
      if (startHeld_) {
        const bool fast = timestampMicros - scratch.heldSinceMicros >
                          configuration_.scratchFastRepeatAfterMicros;
        actions.push_back(
            {.kind = StartSelectControlActionKind::AdjustLaneCover,
             .delta = scratch.direction * (fast ? 10 : 1)});
      } else {
        actions.push_back(
            {.kind = StartSelectControlActionKind::AdjustDuration,
             .delta = scratch.direction});
      }
    }
  }

  void appendConjunctionActions(
      std::int64_t timestampMicros, StartSelectControlFrameContext context,
      std::vector<StartSelectControlAction> &actions) {
    const bool conjunction = startHeld_ && selectHeld_;
    if (conjunction && !conjunctionHeld_) {
      conjunctionHeldSinceMicros_ = timestampMicros;
      exitIssued_ = false;
      actions.push_back(
          {.kind = StartSelectControlActionKind::ToggleLiftHiddenTarget});
    }
    conjunctionHeld_ = conjunction;
    if (context.noteEnd && (startHeld_ || selectHeld_) && !exitIssued_) {
      actions.push_back({.kind = StartSelectControlActionKind::Exit});
      exitIssued_ = true;
      return;
    }
    if (conjunction && !exitIssued_ &&
        timestampMicros - conjunctionHeldSinceMicros_ >
            configuration_.exitHoldDurationMicros) {
      actions.push_back({.kind = StartSelectControlActionKind::Exit});
      exitIssued_ = true;
    }
    if (!conjunction) {
      exitIssued_ = false;
    }
  }

  Configuration configuration_;
  bool startHeld_ = false;
  bool selectHeld_ = false;
  bool conjunctionHeld_ = false;
  bool exitIssued_ = false;
  std::int64_t lastStartPressedMicros_ = 0;
  std::int64_t conjunctionHeldSinceMicros_ = 0;
  std::array<ScratchState, 2> scratch_{};
};

} // namespace gameplay
