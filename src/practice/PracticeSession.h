#pragma once

#include "../ReplayData.h"
#include "PracticeConfiguration.h"

#include <cstddef>
#include <vector>

namespace practice {

class Session {
public:
  explicit Session(Configuration configuration);

  void beginAttempt();
  void completeAttempt(ReplayData replay);
  void abandonAttempt();

  [[nodiscard]] bool shouldLoop() const;
  [[nodiscard]] int loopNumber() const;
  [[nodiscard]] const std::vector<ReplayData> &completedAttempts() const;
  [[nodiscard]] std::size_t abandonedAttemptCount() const;
  [[nodiscard]] const Configuration &configuration() const;

private:
  const Configuration configuration_;
  std::vector<ReplayData> completedAttempts_;
  std::size_t abandonedAttemptCount_ = 0;
  bool attemptActive_ = false;
};

[[nodiscard]] const ReplayData *
completedAttemptForGhost(const Session *session,
                         const ReplayData &sessionlessAttempt,
                         bool attemptCompleted) noexcept;

} // namespace practice
