#pragma once

#include "../bms_parser.hpp"
#include "IRhythmControl.h"
#include "InputProfile.h"
#include "InputTypes.h"

#include <functional>
#include <map>
#include <set>
#include <span>
#include <vector>

std::vector<input::InputScope> makeGameplayInputScopes(int keyMode);
bool hasActiveKeyboardActionBinding(
    const InputProfile &profile,
    std::span<const input::InputScope> activeScopes, int scancode,
    input::LogicalActionKind actionKind);

class LogicalGameplayInputAdapter {
public:
  using CommandCallback =
      std::function<void(const input::LogicalInputTransition &)>;

  LogicalGameplayInputAdapter(IRhythmControl &, CommandCallback);

  void apply(std::span<const input::LogicalInputTransition> transitions);
  void reset();

private:
  enum class ScratchDirection { Clockwise, CounterClockwise };

  static int scratchLane(input::InputScope scope);
  [[nodiscard]] bool isLaneHeld(int lane) const;
  void applyLane(const input::LogicalInputTransition &transition);
  void applyScratch(const input::LogicalInputTransition &transition,
                    ScratchDirection direction, bool reversing = false);

  IRhythmControl &control_;
  CommandCallback commandCallback_;
  std::map<int, std::set<input::InputScope>> heldLaneScopes_;
  std::map<int, ScratchDirection> heldScratchDirections_;
};
