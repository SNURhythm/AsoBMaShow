#pragma once

#include "../bms_parser.hpp"
#include "IRhythmControl.h"
#include "InputBindingResolver.h"
#include "InputProfile.h"
#include "InputTypes.h"
#include "../replay/ReplayPlayback.h"

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
  [[nodiscard]] bms_parser::Note *
  applyTouch(const input::LogicalInputTransition &transition);
  void reset();

private:
  enum class OwnerKind { Logical, Touch };
  enum class ScratchDirection { Clockwise, CounterClockwise };
  struct LaneOwner {
    input::InputScope scope;
    OwnerKind kind = OwnerKind::Logical;
    auto operator<=>(const LaneOwner &) const = default;
  };
  struct ScratchOwner {
    ScratchDirection direction = ScratchDirection::Clockwise;
    input::InputScope scope;
    OwnerKind kind = OwnerKind::Logical;
    auto operator<=>(const ScratchOwner &) const = default;
  };
  struct ScratchLaneState {
    std::set<ScratchOwner> heldOwners;
    std::optional<ScratchDirection> activeDirection;
  };

  static int scratchLane(input::InputScope scope);
  [[nodiscard]] bool isLaneHeld(int lane) const;
  bms_parser::Note *pressPhysicalLane(int lane);
  void releasePhysicalLane(int lane, bool backSpin);
  void applyOwned(std::span<const input::LogicalInputTransition> transitions,
                  OwnerKind ownerKind);
  void applyLane(const input::LogicalInputTransition &transition,
                 OwnerKind ownerKind);
  void applyScratch(const input::LogicalInputTransition &transition,
                    ScratchDirection direction, OwnerKind ownerKind,
                    bool reversing = false,
                    bool oppositeReleasedInBatch = false);
  [[nodiscard]] static replay::LogicalControl
  replayLaneControl(const input::LogicalInputTransition &transition);
  [[nodiscard]] std::optional<replay::LogicalControl>
  effectiveScratchReplayControl(int lane) const;
  void synchronizeScratchReplayControl(
      const input::LogicalInputTransition &transition, int lane);
  void notifyApplied(const input::LogicalInputTransition &,
                     replay::LogicalControl, bool pressed);
  void notifyCommandApplied(const input::LogicalInputTransition &);

  IRhythmControl &control_;
  CommandCallback commandCallback_;
  AppliedTransitionCallback appliedTransitionCallback_;
  std::map<int, std::set<LaneOwner>> heldLaneOwners_;
  std::map<int, ScratchLaneState> scratchLaneStates_;
  std::map<int, replay::LogicalControl> recordedScratchControls_;
  std::map<int, std::size_t> pendingPhysicalEdges_;
  bms_parser::Note *latestPressedNote_ = nullptr;
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
  bool consumeDirectKeyboard(int scancode, bool pressed);
  [[nodiscard]] bms_parser::Note *consumeTouchTransition(
      const input::LogicalInputTransition &transition);
  void disconnectDevice(std::string_view stableId);
  void reset();

private:
  LogicalGameplayInputAdapter adapter_;
  InputBindingResolver resolver_;
  LogicalGameplayRegistryPolicy registryPolicy_;
};
