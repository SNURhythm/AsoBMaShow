#pragma once

#include "InputBindingResolver.h"
#include "InputDeviceRegistry.h"
#include "InputProfile.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class InputCaptureController {
public:
  enum class State { Idle, Listening, AwaitingConflictConfirmation };
  struct BindingEdit {
    std::optional<float> deadZone;
    std::optional<float> activationThreshold;
    std::optional<float> releaseThreshold;
    std::optional<bool> inverted;
  };
  using SaveCallback =
      std::function<bool(const InputProfile &, std::string &errorMessage)>;

  InputCaptureController(InputDeviceRegistry &, InputProfile &, SaveCallback);
  ~InputCaptureController();

  InputCaptureController(const InputCaptureController &) = delete;
  InputCaptureController &operator=(const InputCaptureController &) = delete;

  void begin(input::InputScope, input::LogicalAction);
  void cancel();
  void confirmReplace();
  void rejectReplace();
  void updateBinding(std::string_view bindingId, const BindingEdit &edit);
  void toggleBindingInversion(std::string_view bindingId);
  void resetScopeToDefaults(input::InputScope);

  [[nodiscard]] State state() const;
  [[nodiscard]] std::optional<input::PhysicalInputEvent> monitorSample() const;
  [[nodiscard]] std::span<const input::InputBinding> pendingConflicts() const;
  [[nodiscard]] bool isBindingDeviceMissing(std::string_view bindingId) const;
  [[nodiscard]] std::string_view lastError() const;

private:
  static constexpr float kCaptureActivationThreshold = 0.5F;

  void observeDevice(const input::InputDeviceSnapshot &device);
  void observeSample(const input::PhysicalInputEvent &event);
  void considerCandidate(const input::PhysicalInputEvent &event);
  void considerControlActivation(const input::PhysicalInputEvent &event,
                                 input::PhysicalControl control, float value);
  void stageCandidate(input::PhysicalControl control);
  bool persist(InputProfile nextProfile);
  void clearPending();
  [[nodiscard]] std::string makeBindingId();

  InputDeviceRegistry &registry_;
  InputProfile &profile_;
  SaveCallback save_;
  InputBindingResolver resolver_;
  std::uint64_t inputSubscription_ = 0;
  std::uint64_t deviceSubscription_ = 0;
  std::uint64_t nextBindingId_ = 1;
  State state_ = State::Idle;
  input::InputScope targetScope_;
  input::LogicalAction targetAction_;
  std::optional<input::PhysicalInputEvent> monitorSample_;
  std::optional<input::InputBinding> pendingBinding_;
  std::vector<input::InputBinding> pendingConflicts_;
  std::map<input::PhysicalControl, bool> activationStates_;
  std::string lastError_;
};
