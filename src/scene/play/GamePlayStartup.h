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

enum class CompletedAttemptPersistenceRoute {
  None,
  ModernChartFile,
  LegacyCourse,
};

inline CompletedAttemptPersistenceRoute
completedAttemptPersistenceRoute(bool persistResult, bool course) noexcept {
  if (!persistResult) {
    return CompletedAttemptPersistenceRoute::None;
  }
  return course ? CompletedAttemptPersistenceRoute::LegacyCourse
                : CompletedAttemptPersistenceRoute::ModernChartFile;
}

} // namespace gameplay_startup
