#pragma once

#include "../scene/play/PlayfieldPresentationEvents.h"

#include <cstddef>
#include <cstdint>
#include <optional>

inline constexpr std::int64_t kDefaultMissLayerDurationMicros = 500'000;

struct GameplayBgaMissState {
  bool active = false;
  std::int64_t startedBgaMicros = 0;
  std::int64_t durationMicros = kDefaultMissLayerDurationMicros;
  std::uint64_t triggerSerial = 0;

  [[nodiscard]] bool isActiveAt(std::int64_t bgaTimeMicros) const noexcept;
  [[nodiscard]] std::optional<std::size_t>
  frameIndexAt(std::int64_t bgaTimeMicros,
               std::size_t frameCount) const noexcept;
};

class GameplayBgaMissStateTracker {
public:
  void onJudge(JudgeResult judge, int resultingCombo,
               PlayfieldJudgeEventClock clock) noexcept;
  [[nodiscard]] GameplayBgaMissState snapshot() const noexcept;
  void reset() noexcept;

private:
  GameplayBgaMissState state_;
};
