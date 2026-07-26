#pragma once

#include "../bms_parser.hpp"
#include "IRhythmControl.h"
#include "InputBindingResolver.h"
#include "InputProfile.h"
#include "InputTypes.h"
#include "../replay/ReplayPlaybackData.h"

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
  struct AppliedTransition {
    input::LogicalInputTransition source;
    replay::LogicalControl control;
    bool pressed = false;
    bool replayOnly = false;
  };
  using AppliedTransitionCallback =
      std::function<void(const AppliedTransition &)>;

  LogicalGameplayInputAdapter(IRhythmControl &, CommandCallback,
                              AppliedTransitionCallback = {});

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
  void pressPhysicalLane(int lane);
  void releasePhysicalLane(int lane, bool backSpin);
  void applyLane(const input::LogicalInputTransition &transition);
  void applyScratch(const input::LogicalInputTransition &transition,
                    ScratchDirection direction, bool reversing = false,
                    bool oppositeReleasedInBatch = false);
  [[nodiscard]] static replay::LogicalControl
  replayLaneControl(const input::LogicalInputTransition &transition);
  [[nodiscard]] static replay::LogicalControl
  replayScratchControl(const input::LogicalInputTransition &transition,
                       ScratchDirection direction);
  [[nodiscard]] std::optional<replay::LogicalControl>
  effectiveScratchReplayControl(int lane) const;
  void synchronizeScratchReplayControl(
      const input::LogicalInputTransition &transition, int lane);
  void notifyApplied(const input::LogicalInputTransition &,
                     replay::LogicalControl, bool pressed);

  IRhythmControl &control_;
  CommandCallback commandCallback_;
  AppliedTransitionCallback appliedTransitionCallback_;
  std::map<int, std::set<input::InputScope>> heldLaneScopes_;
  std::map<int, ScratchLaneState> scratchLaneStates_;
  std::map<int, replay::LogicalControl> recordedScratchControls_;
  std::map<int, std::size_t> pendingPhysicalEdges_;
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
      LogicalGameplayRegistryPolicy registryPolicy = {},
      LogicalGameplayInputAdapter::AppliedTransitionCallback
          appliedTransitionCallback = {});
  LogicalGameplayInputPipeline(const LogicalGameplayInputPipeline &) = delete;
  LogicalGameplayInputPipeline &
  operator=(const LogicalGameplayInputPipeline &) = delete;
  LogicalGameplayInputPipeline(LogicalGameplayInputPipeline &&) = delete;
  LogicalGameplayInputPipeline &
  operator=(LogicalGameplayInputPipeline &&) = delete;

  bool consumeRegistryEvent(const input::PhysicalInputEvent &event);
  bool consumeDirectKeyboard(int scancode, bool pressed,
                             std::int64_t steadyTimestampMicros = 0);
  void disconnectDevice(std::string_view stableId,
                        std::int64_t steadyTimestampMicros = 0);
  void reset(std::int64_t steadyTimestampMicros = 0);

private:
  LogicalGameplayInputAdapter adapter_;
  InputBindingResolver resolver_;
  LogicalGameplayRegistryPolicy registryPolicy_;
};
