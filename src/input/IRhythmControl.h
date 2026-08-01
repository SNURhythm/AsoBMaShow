#pragma once

class IRhythmControl {
public:
  virtual ~IRhythmControl() = default;
  virtual bms_parser::Note *pressLane(int mainLane, int compensateLane,
                                      double inputDelay = 0) = 0;
  virtual bms_parser::Note *pressLane(int lane, double inputDelay = 0) = 0;
  virtual bms_parser::Note *releaseLane(int lane, double inputDelay = 0,
                                        bool isBackSpin = false) = 0;
};
