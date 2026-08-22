#pragma once

#include "../../input/InputLifecycle.h"

namespace skin_text_input_lifecycle {

enum class CommitResult {
  NotRequested,
  Committed,
  Retained,
};

[[nodiscard]] inline bool shouldCommit(const SDL_Event &event,
                                       bool focused) noexcept {
  return focused && input::isBackgroundLifecycleEvent(event);
}

template <typename Commit>
[[nodiscard]] CommitResult route(const SDL_Event &event, bool focused,
                                 Commit commit) {
  if (!shouldCommit(event, focused)) {
    return CommitResult::NotRequested;
  }
  return commit() ? CommitResult::Committed : CommitResult::Retained;
}

} // namespace skin_text_input_lifecycle
