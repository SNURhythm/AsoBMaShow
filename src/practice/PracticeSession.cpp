#include "PracticeSession.h"

#include <utility>

namespace practice {

Session::Session(Configuration configuration)
    : configuration_(std::move(configuration)) {}

void Session::beginAttempt() { attemptActive_ = true; }

void Session::completeAttempt(ReplayData replay) {
  if (!attemptActive_) {
    return;
  }
  completedAttempts_.push_back(std::move(replay));
  attemptActive_ = false;
}

void Session::abandonAttempt() {
  if (!attemptActive_) {
    return;
  }
  ++abandonedAttemptCount_;
  attemptActive_ = false;
}

bool Session::shouldLoop() const { return configuration_.loop; }

int Session::loopNumber() const {
  return static_cast<int>(completedAttempts_.size()) + 1;
}

const std::vector<ReplayData> &Session::completedAttempts() const {
  return completedAttempts_;
}

std::size_t Session::abandonedAttemptCount() const {
  return abandonedAttemptCount_;
}

const Configuration &Session::configuration() const { return configuration_; }

void Session::setSkinItemScrollPosition(float position) noexcept {
  skinItemScrollPosition_ = position;
}

SkinMenuState Session::skinMenuState(const SkinMenuInputs &inputs) const {
  return buildSkinMenuState(configuration_, inputs, skinItemScrollPosition_);
}

const ReplayData *
completedAttemptForGhost(const Session *session,
                         const ReplayData &sessionlessAttempt,
                         bool attemptCompleted) noexcept {
  if (!attemptCompleted) {
    return nullptr;
  }
  if (session != nullptr) {
    return session->completedAttempts().empty()
               ? nullptr
               : &session->completedAttempts().back();
  }
  return sessionlessAttempt.events.empty() ? nullptr : &sessionlessAttempt;
}

} // namespace practice
