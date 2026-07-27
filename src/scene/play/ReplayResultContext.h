#pragma once

#include <optional>
#include <string>

// Repository identity for a replay being watched. This navigation context is
// deliberately separate from both raw replay playback and its derived judged
// analysis projection.
struct ReplayResultContext {
  int resultId = 0;
  std::optional<std::string> attemptId;
  std::string createdAt;

  bool operator==(const ReplayResultContext &) const = default;
};

struct ReplayComparisonQuery {
  std::optional<std::string> beforeCreatedAt;
  std::optional<std::string> excludeAttemptId;

  bool operator==(const ReplayComparisonQuery &) const = default;
};

[[nodiscard]] inline ReplayComparisonQuery
replayComparisonQueryFor(const ReplayResultContext *context) {
  ReplayComparisonQuery query;
  if (context == nullptr) {
    return query;
  }
  if (context->attemptId.has_value() && !context->attemptId->empty()) {
    query.excludeAttemptId = context->attemptId;
  }
  if (!context->createdAt.empty()) {
    query.beforeCreatedAt = context->createdAt;
  }
  return query;
}

[[nodiscard]] inline ReplayComparisonQuery
replayComparisonQueryFor(const ReplayResultContext &context) {
  return replayComparisonQueryFor(&context);
}
