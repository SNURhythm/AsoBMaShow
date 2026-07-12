#pragma once

#include "../ReplayData.h"

namespace practice {

struct ResultCaptureContext {
  bool autoPlay = false;
  bool practice = false;
  bool replayPlayback = false;
  bool coursePlayback = false;
};

struct ResultCapturePolicy {
  bool recordReplay = false;
  bool captureAnalytics = false;
  bool persistReplay = false;
  bool persistScore = false;
  bool publishPracticeGhost = false;
};

[[nodiscard]] ResultCapturePolicy
resultCapturePolicy(const ResultCaptureContext &context) noexcept;

[[nodiscard]] const ReplayData *
selectResultAnalyticsSource(const ReplayData *capturedAnalytics,
                            const ReplayData *replayToSave,
                            const ReplayData *retrySource) noexcept;

} // namespace practice
