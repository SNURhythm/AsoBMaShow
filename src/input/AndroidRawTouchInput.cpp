#include "AndroidRawTouchInput.h"

#include <mutex>

namespace input::android {
namespace {
struct Registry {
  std::mutex mutex;
  RawTouchRegistration *active = nullptr;
  TouchEpoch nextEpoch = 1;
};
Registry &registry() {
  static Registry state;
  return state;
}
} // namespace

struct RawTouchRegistration::UiState {
  struct Contact {
    bool active = false;
    bool suppressed = false;
    RawTouchEvent last;
  };
  std::array<RawTouchEvent, kUiTouchQueueCapacity> queue{};
  std::size_t head = 0, count = 0;
  bool overflow = false;
  std::array<Contact, kMaximumUiTouchContacts> physical{};
  std::array<Contact, kMaximumUiTouchContacts> delivered{};

  static Contact *find(std::array<Contact, kMaximumUiTouchContacts> &contacts,
                       int pointerId) {
    for (auto &contact : contacts) {
      if (contact.active && contact.last.pointerId == pointerId) return &contact;
    }
    return nullptr;
  }

  static Contact *unused(std::array<Contact, kMaximumUiTouchContacts> &contacts) {
    for (auto &contact : contacts) {
      if (!contact.active) return &contact;
    }
    return nullptr;
  }

  void discardAndCancel() {
    head = count = 0;
    overflow = true;
    for (auto &contact : physical) {
      if (contact.active) contact.suppressed = true;
    }
  }

  void push(const RawTouchEvent &event) {
    auto *contact = find(physical, event.pointerId);
    if (event.phase == TouchPhase::Down) {
      if (contact != nullptr) return;
      contact = unused(physical);
      if (contact == nullptr) {
        discardAndCancel();
        return;
      }
      *contact = {.active = true, .suppressed = overflow, .last = event};
    } else if (contact == nullptr) {
      // A session cannot acquire the latter half of somebody else's gesture.
      return;
    }
    contact->last = event;
    if (!contact->suppressed && !overflow) {
      if (count == queue.size()) {
        discardAndCancel();
      } else {
        queue[(head + count) % queue.size()] = event;
        ++count;
      }
    }
    if (event.phase == TouchPhase::Up || event.phase == TouchPhase::Cancel) {
      contact->active = false;
    }
  }

  bool popCancellation(RawTouchEvent &event) {
    for (auto &contact : delivered) {
      if (!contact.active) continue;
      event = contact.last;
      event.phase = TouchPhase::Cancel;
      contact.active = false;
      return true;
    }
    return false;
  }

  bool pop(RawTouchEvent &event) {
    if (overflow) {
      if (popCancellation(event)) return true;
      overflow = false;
    }
    if (count == 0) return false;
    event = queue[head];
    head = (head + 1) % queue.size();
    --count;
    auto *contact = find(delivered, event.pointerId);
    if (event.phase == TouchPhase::Down) contact = unused(delivered);
    if (contact != nullptr) {
      *contact = {.active = event.phase != TouchPhase::Up &&
                           event.phase != TouchPhase::Cancel,
                  .last = event};
    }
    return true;
  }
};

RawTouchRegistration::RawTouchRegistration(Callback callback, void *context)
    : ui_(std::make_unique<UiState>()), callback_(callback), context_(context) {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  do { epoch_ = state.nextEpoch++; } while (epoch_ == 0);
  state.active = this;
}

RawTouchRegistration::~RawTouchRegistration() {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  if (state.active == this) state.active = nullptr;
}

void RawTouchRegistration::dispatch(const RawTouchEvent &event) {
  dispatch(activeEpoch(), event);
}

TouchEpoch RawTouchRegistration::activeEpoch() {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  return state.active != nullptr ? state.active->epoch_ : 0;
}

bool RawTouchRegistration::isCurrentEpoch(TouchEpoch epoch) {
  return epoch != 0 && activeEpoch() == epoch;
}

void RawTouchRegistration::dispatch(TouchEpoch epoch, const RawTouchEvent &event) {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  if (state.active != nullptr && state.active->epoch_ == epoch) {
    // UI admission is independent of the gameplay pause/recovery gates. A
    // saturated UI queue must never make the producer forward through SDL.
    state.active->ui_->push(event);
    if (state.active->callback_ != nullptr) {
      state.active->callback_(event, state.active->context_);
    }
  }
}

bool RawTouchRegistration::pollUiEvent(UiTouchEvent &event) {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  if (state.active == nullptr || !state.active->ui_->pop(event.touch)) return false;
  event.epoch = state.active->epoch_;
  return true;
}

UiCancellationBatch RawTouchRegistration::cancelUiTouches() {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  ui_->discardAndCancel();
  UiCancellationBatch result;
  while (result.size < result.events.size() &&
         ui_->popCancellation(result.events[result.size])) {
    ++result.size;
  }
  ui_->overflow = false;
  return result;
}

} // namespace input::android
