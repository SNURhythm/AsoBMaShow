#pragma once

namespace settings_scene {

class InputSettingsRebuildGate {
public:
  [[nodiscard]] bool request() {
    requested_ = true;
    if (callbackScheduled_ || eligible_) {
      return false;
    }
    callbackScheduled_ = true;
    return true;
  }

  void markEventComplete() {
    callbackScheduled_ = false;
    eligible_ = true;
  }

  void noticeStateChange() {
    requested_ = true;
    eligible_ = true;
  }

  [[nodiscard]] bool consume(bool pointerTransactionActive) {
    if (!requested_ || !eligible_ || pointerTransactionActive) {
      return false;
    }
    requested_ = false;
    eligible_ = false;
    return true;
  }

  void reset() {
    requested_ = false;
    eligible_ = false;
    callbackScheduled_ = false;
  }

private:
  bool requested_ = false;
  bool eligible_ = false;
  bool callbackScheduled_ = false;
};

} // namespace settings_scene
