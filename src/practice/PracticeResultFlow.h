#pragma once

#include "../analysis/JudgedPlaybackData.h"

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

[[nodiscard]] const JudgedPlaybackData *
selectResultAnalyticsSource(const JudgedPlaybackData *capturedAnalytics,
                            const JudgedPlaybackData *replayToSave,
                            const JudgedPlaybackData *retrySource) noexcept;

} // namespace practice
