#pragma once

#include "GameplayBgaFrame.h"
#include "../scene/play/PlayfieldPresentationEvents.h"

class GameplayBgaMissStateTracker {
public:
  void onJudge(JudgeResult judge, int resultingCombo,
               PlayfieldJudgeEventClock clock) noexcept;
  [[nodiscard]] GameplayBgaMissState snapshot() const noexcept;
  void reset() noexcept;

private:
  GameplayBgaMissState state_;
};
