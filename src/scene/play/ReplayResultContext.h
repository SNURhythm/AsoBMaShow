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
