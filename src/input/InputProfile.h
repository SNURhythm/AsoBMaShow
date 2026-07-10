#pragma once

#include "InputTypes.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct InputProfile {
  static constexpr int kSchemaVersion = 1;

  int schemaVersion = kSchemaVersion;
  std::vector<input::InputBinding> bindings;

  void sanitize(std::vector<std::string> &diagnostics);

  std::vector<std::reference_wrapper<const input::InputBinding>>
  bindingsFor(input::InputScope scope) const;

  std::vector<input::InputBinding>
  conflictsWith(const input::InputBinding &candidate) const;

  bool hasDigitalBinding(input::InputScope scope, input::LogicalAction action,
                         std::string_view deviceId, int scancode) const;
};

InputProfile makeDefaultInputProfile();
