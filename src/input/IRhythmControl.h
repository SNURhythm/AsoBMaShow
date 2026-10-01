#pragma once

#include <cstdint>

class IRhythmControl {
public:
  virtual ~IRhythmControl() = default;
  virtual bms_parser::Note *pressLaneAt(int lane, std::int64_t) {
    return pressLane(lane, 0.0);
  }
  virtual bms_parser::Note *releaseLaneAt(int lane, std::int64_t,
                                         bool backSpin = false) {
    return releaseLane(lane, 0.0, backSpin);
  }
  virtual bms_parser::Note *pressLane(int mainLane, int compensateLane,
                                      double inputDelay = 0) = 0;
  virtual bms_parser::Note *pressLane(int lane, double inputDelay = 0) = 0;
  virtual bms_parser::Note *releaseLane(int lane, double inputDelay = 0,
                                        bool isBackSpin = false) = 0;
};
