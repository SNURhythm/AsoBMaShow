#pragma once

#include <string>
#include <utility>

namespace gameplay_startup {

struct PlaybackInitializationResult {
  bool mayStartAttempt = false;
  std::string visibleStatus;
};

inline PlaybackInitializationResult
playbackInitializationResult(bool playbackReady, std::string errorMessage) {
  if (playbackReady) {
    return {.mayStartAttempt = true};
  }
  if (errorMessage.empty()) {
    errorMessage = "Playback initialization failed";
  }
  return {
      .mayStartAttempt = false,
      .visibleStatus = std::move(errorMessage),
  };
}

enum class FailureReturnTarget { RequestedScene, MainMenu };

inline FailureReturnTarget failureReturnTarget(bool requestedSceneIsLive) {
  return requestedSceneIsLive ? FailureReturnTarget::RequestedScene
                              : FailureReturnTarget::MainMenu;
}

} // namespace gameplay_startup
