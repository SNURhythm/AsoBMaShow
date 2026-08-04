#include "GameplayBgaMissStateTracker.h"

bool GameplayBgaMissState::isActiveAt(
    std::int64_t bgaTimeMicros) const noexcept {
  if (!active || startedBgaMicros == 0 || durationMicros <= 0 ||
      bgaTimeMicros < startedBgaMicros) {
    return false;
  }
  return bgaTimeMicros - startedBgaMicros < durationMicros;
}

std::optional<std::size_t> GameplayBgaMissState::frameIndexAt(
    std::int64_t bgaTimeMicros, std::size_t frameCount) const noexcept {
  if (frameCount == 0 || !isActiveAt(bgaTimeMicros)) {
    return std::nullopt;
  }
  if (frameCount == 1) {
    return 0;
  }
  const auto elapsedMicros =
      static_cast<std::uint64_t>(bgaTimeMicros - startedBgaMicros);
  const auto duration = static_cast<std::uint64_t>(durationMicros);
  return ((frameCount - 1) * elapsedMicros) / duration;
}

void GameplayBgaMissStateTracker::onJudge(
    JudgeResult judge, int resultingCombo,
    PlayfieldJudgeEventClock clock) noexcept {
  if (judge.judgement == None || resultingCombo != 0) {
    return;
  }
  state_.active = true;
  state_.startedBgaMicros = clock.bgaTimeMicros;
  state_.durationMicros = kDefaultMissLayerDurationMicros;
  ++state_.triggerSerial;
}

GameplayBgaMissState GameplayBgaMissStateTracker::snapshot() const noexcept {
  return state_;
}

void GameplayBgaMissStateTracker::reset() noexcept {
  state_ = GameplayBgaMissState{};
}
