#pragma once

#include "InputProfile.h"

#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <vector>

class InputBindingResolver {
public:
  enum class Mode { Gameplay, Capture };

  struct Callbacks {
    std::function<void(std::span<const input::LogicalInputTransition>)>
        onTransitions;
    std::function<void(const input::PhysicalInputEvent &)> onMonitorSample;
    std::function<void(const input::PhysicalInputEvent &)> onCaptureCandidate;
  };

  InputBindingResolver(const InputProfile &, std::vector<input::InputScope>,
                       Callbacks);

  void setMode(Mode);
  void consume(const input::PhysicalInputEvent &);
  void disconnectDevice(std::string_view stableId);
  void reset();
  std::set<input::DeviceClass> activeDeviceClasses() const;

private:
  struct PhysicalBindingState {
    bool active = false;
    std::set<input::ControlDirection> activeHatDirections;
  };

  struct LogicalStateKey {
    input::InputScope scope;
    input::LogicalAction action;
    auto operator<=>(const LogicalStateKey &) const = default;
  };

  struct BindingEvaluation {
    std::size_t bindingIndex = 0;
    bool active = false;
    float value = 0.0f;
  };

  bool scopeIsActive(input::InputScope scope) const;
  void applyEvaluations(std::span<const BindingEvaluation> evaluations);

  InputProfile profile_;
  std::vector<input::InputScope> activeScopes_;
  Callbacks callbacks_;
  Mode mode_ = Mode::Gameplay;
  std::vector<PhysicalBindingState> physicalStates_;
  std::map<LogicalStateKey, std::size_t> logicalReferenceCounts_;
};
