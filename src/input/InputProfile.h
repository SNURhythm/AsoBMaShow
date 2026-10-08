#pragma once

#include "GyroscopeTurntable.h"
#include "InputTypes.h"
#include "VirtualControllerConfig.h"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct InputProfile {
  static constexpr int kSchemaVersion = 9;

  int schemaVersion = kSchemaVersion;
  input::GyroscopeTurntableConfig gyroscopeTurntable;
  std::map<int, input::VirtualControllerConfig> virtualControllers = [] {
    std::map<int, input::VirtualControllerConfig> configs;
    for (const int mode : {4, -5, 5, 6, -7, 7, 8, 10, 14}) {
      configs.emplace(mode, input::VirtualControllerConfig::forKeyMode(mode));
    }
    return configs;
  }();

  const input::VirtualControllerConfig &virtualControllerForKeyMode(int keyMode) const {
    const auto found = virtualControllers.find(keyMode);
    if (found != virtualControllers.end()) return found->second;
    static const input::VirtualControllerConfig disabled;
    return disabled;
  }
  std::vector<input::InputBinding> bindings;

  void sanitize(std::vector<std::string> &diagnostics);

  std::vector<std::reference_wrapper<const input::InputBinding>>
  bindingsFor(input::InputScope scope) const;

  std::vector<input::InputBinding>
  conflictsWith(const input::InputBinding &candidate) const;

  bool hasDigitalBinding(input::InputScope scope, input::LogicalAction action,
                         std::string_view deviceId, int scancode) const;
};

namespace input_profile {

bool migrateCompactScratchlessLaneBindings(InputProfile &profile);
bool addMissingGameplayCommandBindings(InputProfile &profile);

} // namespace input_profile

InputProfile makeDefaultInputProfile();

namespace input_profile_runtime {

template <typename Save, typename Apply>
bool saveThenApplyGyroscopeConfig(const InputProfile &current,
                                  const InputProfile &candidate, Save &&save,
                                  Apply &&apply, std::string &error) {
  if (!std::forward<Save>(save)(candidate, error)) {
    return false;
  }
  if (candidate.gyroscopeTurntable != current.gyroscopeTurntable) {
    std::forward<Apply>(apply)(candidate.gyroscopeTurntable);
  }
  return true;
}

} // namespace input_profile_runtime
