#pragma once

#include "IrDriver.h"
#include "IrProfileSettings.h"

#include <map>
#include <optional>
#include <string>

namespace ir {

[[nodiscard]] inline std::optional<std::string> firstEnabledRankingProvider(
    const std::map<std::string, IrProviderSettings> &providers,
    const IrDriverRegistry &drivers) {
  for (const auto &[id, settings] : providers) {
    if (!settings.enabled) continue;
    const auto driver = drivers.find(id);
    if (driver && driver->capabilities().chartRankings) return id;
  }
  return std::nullopt;
}

} // namespace ir
