#pragma once

#include <algorithm>

namespace settings_scene {

struct InputSettingsLayout {
  bool stackSelectors = false;
  bool stackBindingEditor = false;
  int selectorGap = 12;
  int selectorWidth = 0;
  int numericControlWidth = 0;
};

constexpr InputSettingsLayout resolveInputSettingsLayout(int availableWidth,
                                                         bool compact) {
  InputSettingsLayout result;
  const int width = std::max(0, availableWidth);
  result.selectorGap = compact ? 8 : 12;
  result.stackSelectors = compact || width < 720;
  result.stackBindingEditor = compact || width < 900;
  result.selectorWidth =
      result.stackSelectors ? width
                            : std::max(0, (width - result.selectorGap * 2) / 3);
  result.numericControlWidth =
      result.stackBindingEditor
          ? width
          : std::max(0, (width - result.selectorGap * 3) / 4);
  return result;
}

} // namespace settings_scene
