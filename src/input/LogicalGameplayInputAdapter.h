#pragma once

#include "../bms_parser.hpp"
#include "IRhythmControl.h"
#include "InputBindingResolver.h"
#include "InputProfile.h"
#include "InputTypes.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

std::vector<input::InputScope> makeGameplayInputScopes(int keyMode);
bool hasActiveKeyboardActionBinding(
    const InputProfile &profile,
    std::span<const input::InputScope> activeScopes, int scancode,
    input::LogicalActionKind actionKind);
InputProfile makeGameplayInputProfileWithEscapeFallback(
    const InputProfile &profile,
    std::span<const input::InputScope> activeScopes);

class LogicalGameplayInputAdapter {
public:
  using CommandCallback =
      std::function<void(const input::LogicalInputTransition &)>;

  LogicalGameplayInputAdapter(IRhythmControl &, CommandCallback);

  void apply(std::span<const input::LogicalInputTransition> transitions);
  void reset();

private:
  enum class ScratchDirection { Clockwise, CounterClockwise };
  struct ScratchLaneState {
    std::set<ScratchDirection> heldDirections;
    std::optional<ScratchDirection> activeDirection;
  };

  static int scratchLane(input::InputScope scope);
  [[nodiscard]] bool isLaneHeld(int lane) const;
  void applyLane(const input::LogicalInputTransition &transition);
  void applyScratch(const input::LogicalInputTransition &transition,
                    ScratchDirection direction, bool reversing = false,
                    bool oppositeReleasedInBatch = false);

  IRhythmControl &control_;
  CommandCallback commandCallback_;
  std::map<int, std::set<input::InputScope>> heldLaneScopes_;
  std::map<int, ScratchLaneState> scratchLaneStates_;
};

struct LogicalGameplayRegistryPolicy {
  bool acceptKeyboardFromRegistry = true;
};

class LogicalGameplayInputPipeline {
public:
  LogicalGameplayInputPipeline(
      IRhythmControl &, const InputProfile &,
      std::vector<input::InputScope> activeScopes,
      LogicalGameplayInputAdapter::CommandCallback commandCallback = {},
      LogicalGameplayRegistryPolicy registryPolicy = {});
  LogicalGameplayInputPipeline(const LogicalGameplayInputPipeline &) = delete;
  LogicalGameplayInputPipeline &
  operator=(const LogicalGameplayInputPipeline &) = delete;
  LogicalGameplayInputPipeline(LogicalGameplayInputPipeline &&) = delete;
  LogicalGameplayInputPipeline &
  operator=(LogicalGameplayInputPipeline &&) = delete;

  bool consumeRegistryEvent(const input::PhysicalInputEvent &event);
  bool consumeDirectKeyboard(int scancode, bool pressed);
  void disconnectDevice(std::string_view stableId);
  void reset();

private:
  LogicalGameplayInputAdapter adapter_;
  InputBindingResolver resolver_;
  LogicalGameplayRegistryPolicy registryPolicy_;
};
