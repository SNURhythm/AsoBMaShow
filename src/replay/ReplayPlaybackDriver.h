#pragma once

#include "ReplayPlaybackData.h"
#include "../bms_parser.hpp"
#include "../input/IRhythmControl.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>

namespace replay {

class ReplayPlaybackDriver {
public:
  using CommandCallback =
      std::function<void(const LogicalControl &, bool pressed)>;

  ReplayPlaybackDriver(const ReplayPlaybackData &, IRhythmControl &,
                       CommandCallback = {});

  void advanceTo(std::int64_t songTimeMicros);
  void reset();
  [[nodiscard]] bool finished() const noexcept;

private:
  [[nodiscard]] int physicalLane(const LogicalControl &) const noexcept;
  [[nodiscard]] bool isScratch(const LogicalControl &) const noexcept;
  [[nodiscard]] bool reversesScratchAt(std::size_t index) const noexcept;
  void apply(std::size_t index);

  const ReplayPlaybackData &replay_;
  IRhythmControl &control_;
  CommandCallback commandCallback_;
  std::size_t next_ = 0;
  std::set<int> heldLanes_;
  std::map<int, LogicalControlKind> activeScratchDirections_;
  std::int64_t advanceTimeMicros_ = 0;
};

} // namespace replay
