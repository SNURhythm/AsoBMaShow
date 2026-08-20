#pragma once

#include "../ReplayData.h"
#include "PracticeConfiguration.h"

#include <cstddef>
#include <optional>
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
  void configureSkinMenu(SkinMenuInputs);
  [[nodiscard]] std::optional<SkinMenuAttemptPlan>
  skinMenuAttemptPlan() const noexcept;
  [[nodiscard]] std::optional<SkinMenuAttemptPlan> beginSkinMenuAttempt();
  void beginSkinMenuAttempt(const SkinMenuAttemptPlan &);
  void setSkinItemScrollPosition(float position) noexcept;
  [[nodiscard]] bool changeSkinMenuVisibleItem(std::size_t index,
                                               bool increment);
  [[nodiscard]] SkinMenuState skinMenuState() const;
  [[nodiscard]] SkinMenuState
  skinMenuState(const SkinMenuInputs &inputs) const;
  [[nodiscard]] Session freshForRetry() const;

private:
  Configuration configuration_;
  std::optional<SkinMenuController> skinMenu_;
  std::optional<Configuration> skinMenuAttemptConfiguration_;
  std::vector<ReplayData> completedAttempts_;
  std::size_t abandonedAttemptCount_ = 0;
  bool attemptActive_ = false;
  float skinItemScrollPosition_ = 0.0F;
};

[[nodiscard]] const ReplayData *
completedAttemptForGhost(const Session *session,
                         const ReplayData &sessionlessAttempt,
                         bool attemptCompleted) noexcept;

} // namespace practice
