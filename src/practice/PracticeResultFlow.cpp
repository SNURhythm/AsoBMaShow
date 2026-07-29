#include "PracticeResultFlow.h"

namespace practice {

ResultCapturePolicy
resultCapturePolicy(const ResultCaptureContext &context) noexcept {
  const bool livePlayback = !context.replayPlayback && !context.coursePlayback;
  const bool recordReplay = !context.autoPlay && !context.replayPlayback;
  const bool persistResult = recordReplay && !context.practice;
  return {
      .recordReplay = recordReplay,
      .captureAnalytics = livePlayback,
      .persistReplay = persistResult,
      .persistScore = persistResult,
      .publishPracticeGhost = recordReplay && context.practice,
  };
}

const ReplayData *
selectResultAnalyticsSource(const ReplayData *capturedAnalytics,
                            const ReplayData *replayToSave,
                            const ReplayData *retrySource) noexcept {
  if (capturedAnalytics != nullptr) {
    return capturedAnalytics;
  }
  return replayToSave != nullptr ? replayToSave : retrySource;
}

} // namespace practice
