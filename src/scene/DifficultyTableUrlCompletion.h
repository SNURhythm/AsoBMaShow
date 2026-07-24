#pragma once

#include <string>
#include <string_view>

namespace settings_ui {

inline bool applyDifficultyTableUrlCompletion(bool finished, bool succeeded,
                                              std::string_view submittedUrl,
                                              std::string &currentUrl) {
  if (!finished || !succeeded || submittedUrl.empty() ||
      currentUrl != submittedUrl) {
    return false;
  }

  currentUrl.clear();
  return true;
}

} // namespace settings_ui
