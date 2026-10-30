#pragma once
#include "../../settings/JudgementIndicatorColorization.h"
#include <memory>

namespace judgement_indicator {
using SharedColorization = std::shared_ptr<const Colorization>;

// Frame copies share immutable ranges. Only editing the settings replaces them.
inline SharedColorization snapshotColorization(
    const Colorization &settings, const SharedColorization &previous = {}) {
  if (previous && *previous == settings) return previous;
  return std::make_shared<const Colorization>(settings);
}
inline const Colorization &colorizationValue(const SharedColorization &settings) {
  if (settings) return *settings;
  static const Colorization defaults;
  return defaults;
}
} // namespace judgement_indicator
