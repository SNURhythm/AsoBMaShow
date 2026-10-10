#pragma once

#include "NativeRawTouchInput.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace input::ios {

// UIKit asks whether to admit a touch before delivering touchesBegan. Keep that
// decision for the whole contact, even if a pause or Retry changes its owner.
class TouchGestureRouter {
public:
  bool claim(std::int64_t pointerId) {
    if (find(pointerId) != nullptr) return true;
    const auto epoch = native_touch::RawTouchRegistration::activeEpoch();
    if (epoch == 0) return false;
    for (auto &contact : contacts_) {
      if (contact.epoch != 0) continue;
      contact.epoch = epoch;
      contact.last.pointerId = pointerId;
      return true;
    }
    // Do not make an over-capacity gameplay contact fall back through SDL.
    return true;
  }

  void dispatch(const native_touch::RawTouchEvent &event) {
    auto *contact = find(event.pointerId);
    if (contact == nullptr) return;
    using native_touch::TouchPhase;
    if (event.phase == TouchPhase::Down) {
      if (contact->started) return;
      contact->started = true;
    } else if (!contact->started) {
      if (event.phase == TouchPhase::Up || event.phase == TouchPhase::Cancel) {
        *contact = {};
      }
      return;
    } else if (event.phase == TouchPhase::Move &&
               event.steadyTimestampMicros <= contact->last.steadyTimestampMicros) {
      return;
    }
    contact->last = event;
    native_touch::RawTouchRegistration::dispatch(contact->epoch, event);
    if (event.phase == TouchPhase::Up || event.phase == TouchPhase::Cancel) {
      *contact = {};
    }
  }

  void dispatchBatch(std::span<native_touch::RawTouchEvent> events) {
    // UIKit supplies history per contact; the worker consumes a single FIFO.
    std::sort(events.begin(), events.end(), [](const auto &left, const auto &right) {
      if (left.steadyTimestampMicros != right.steadyTimestampMicros) {
        return left.steadyTimestampMicros < right.steadyTimestampMicros;
      }
      if (left.phase != right.phase) return left.phase < right.phase;
      return left.pointerId < right.pointerId;
    });
    for (const auto &event : events) dispatch(event);
  }

  void cancel(std::int64_t pointerId, std::int64_t timestampMicros) {
    auto *contact = find(pointerId);
    if (contact == nullptr) return;
    if (contact->started) {
      auto event = contact->last;
      event.phase = native_touch::TouchPhase::Cancel;
      event.steadyTimestampMicros = timestampMicros;
      native_touch::RawTouchRegistration::dispatch(contact->epoch, event);
    }
    *contact = {};
  }

  void cancelAll(std::int64_t timestampMicros) {
    for (auto &contact : contacts_) {
      if (contact.epoch != 0) cancel(contact.last.pointerId, timestampMicros);
    }
  }

  [[nodiscard]] std::size_t contactCount() const {
    std::size_t count = 0;
    for (const auto &contact : contacts_) count += contact.epoch != 0;
    return count;
  }

private:
  struct Contact {
    native_touch::TouchEpoch epoch = 0;
    bool started = false;
    native_touch::RawTouchEvent last;
  };
  Contact *find(std::int64_t pointerId) {
    for (auto &contact : contacts_) {
      if (contact.epoch != 0 && contact.last.pointerId == pointerId) return &contact;
    }
    return nullptr;
  }
  std::array<Contact, native_touch::kMaximumUiTouchContacts> contacts_{};
};

} // namespace input::ios
