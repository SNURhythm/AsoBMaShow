#pragma once

#include "../analysis/JudgedPlaybackData.h"
#include "PracticeConfiguration.h"

#include <cstddef>
#include <vector>

namespace practice {

class Session {
public:
  explicit Session(Configuration configuration);

  void beginAttempt();
  void completeAttempt(JudgedPlaybackData replay);
  void abandonAttempt();

  [[nodiscard]] bool shouldLoop() const;
  [[nodiscard]] int loopNumber() const;
  [[nodiscard]] const std::vector<JudgedPlaybackData> &completedAttempts() const;
  [[nodiscard]] std::size_t abandonedAttemptCount() const;
  [[nodiscard]] const Configuration &configuration() const;

private:
  const Configuration configuration_;
  std::vector<JudgedPlaybackData> completedAttempts_;
  std::size_t abandonedAttemptCount_ = 0;
  bool attemptActive_ = false;
};

[[nodiscard]] const JudgedPlaybackData *
completedAttemptForGhost(const Session *session,
                         const JudgedPlaybackData &sessionlessAttempt,
                         bool attemptCompleted) noexcept;

} // namespace practice
