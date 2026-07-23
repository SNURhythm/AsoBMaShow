#pragma once

#include <string>

namespace settings_ui {

inline bool applyDifficultyTableUrlCompletion(bool finished, bool succeeded,
                                              std::string &url) {
  if (!finished || !succeeded) {
    return false;
  }

  url.clear();
  return true;
}

} // namespace settings_ui
