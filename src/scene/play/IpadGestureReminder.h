#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>

namespace gameplay {
// SDL touch coordinates are normalized to the current window. A check only
// succeeds after every finger lifts, so none of its touches enter gameplay.
class IpadGestureReminder {
public:
  static bool required(bool enabled, bool ipad, bool replay, bool autoPlay,
                       std::size_t courseTrackIndex) {
    return enabled && ipad && !replay && !autoPlay && courseTrackIndex == 0;
  }

  void reset() {
    fingers_.clear();
    releasing_ = false;
    valid_ = false;
    completed_ = false;
  }
  void down(std::int64_t id, float x, float y) {
    completed_ = false;
    if (releasing_) valid_ = false;
    fingers_.try_emplace(id, Finger{x, y, x, y});
  }
  void move(std::int64_t id, float x, float y) {
    if (auto it = fingers_.find(id); it != fingers_.end()) {
      it->second.x = x;
      it->second.y = y;
    }
  }
  void up(std::int64_t id) {
    if (!fingers_.contains(id)) return;
    if (!releasing_) {
      releasing_ = true;
      valid_ = fingers_.size() >= 4;
      for (const auto &[fingerId, finger] : fingers_) {
        const float upward = finger.startY - finger.y;
        valid_ = valid_ && upward >= 0.12F &&
                 std::abs(finger.x - finger.startX) < upward;
      }
    }
    fingers_.erase(id);
    if (fingers_.empty()) {
      completed_ = valid_;
      releasing_ = false;
      valid_ = false;
    }
  }
  [[nodiscard]] bool completed() const { return completed_; }

private:
  struct Finger {
    float startX, startY, x, y;
  };
  std::map<std::int64_t, Finger> fingers_;
  bool releasing_ = false;
  bool valid_ = false;
  bool completed_ = false;
};
} // namespace gameplay
