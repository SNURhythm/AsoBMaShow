#pragma once

#include "Judge.h"

struct PlayfieldJudgeEventClock {
  long long songTimeMicros = 0;
  long long visualTimeMicros = 0;
  long long bgaTimeMicros = 0;

  bool operator==(const PlayfieldJudgeEventClock &) const = default;
};

[[nodiscard]] inline long long
playfieldVisualEventTimeMicros(long long songTimeMicros,
                               long long visualOffsetMicros) noexcept {
  return songTimeMicros - visualOffsetMicros;
}

[[nodiscard]] inline PlayfieldJudgeEventClock
makePlayfieldJudgeEventClock(long long songTimeMicros,
                             long long visualOffsetMicros,
                             long long bgaOffsetMicros = 0) noexcept {
  return {
      .songTimeMicros = songTimeMicros,
      .visualTimeMicros =
          playfieldVisualEventTimeMicros(songTimeMicros, visualOffsetMicros),
      .bgaTimeMicros = songTimeMicros - bgaOffsetMicros,
  };
}

class IPlayfieldPresentationEvents {
public:
  virtual ~IPlayfieldPresentationEvents() = default;

  virtual void onLanePressed(int lane, JudgeResult judge,
                             long long eventMicros) = 0;
  virtual void onLaneReleased(int lane, long long eventMicros) = 0;
  virtual void onJudge(JudgeResult judge, int combo, int score,
                       PlayfieldJudgeEventClock clock,
                       bool recordTimingSample) = 0;
  // Legacy renderers consume the global HUD; custom skins also need the lane.
  virtual void onLaneJudge(int, JudgeResult judge, int combo, int score,
                           PlayfieldJudgeEventClock clock,
                           bool recordTimingSample) {
    onJudge(judge, combo, score, clock, recordTimingSample);
  }

};
