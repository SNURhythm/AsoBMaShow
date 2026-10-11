#include "ApplicationEventQueue.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace platform {
namespace {
constexpr std::size_t maximumPayloadBytes = 64 * 1024;
bool retainedDuringExport(const SDL_Event &event) {
  if ((event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) ||
      (event.type >= SDL_EVENT_DISPLAY_FIRST && event.type <= SDL_EVENT_DISPLAY_LAST)) return true;
  switch (event.type) {
  case SDL_EVENT_LOW_MEMORY:
  case SDL_EVENT_QUIT:
  case SDL_EVENT_TERMINATING:
  case SDL_EVENT_WILL_ENTER_BACKGROUND:
  case SDL_EVENT_DID_ENTER_BACKGROUND:
  case SDL_EVENT_WILL_ENTER_FOREGROUND:
  case SDL_EVENT_DID_ENTER_FOREGROUND:
  case SDL_EVENT_JOYSTICK_ADDED:
  case SDL_EVENT_JOYSTICK_REMOVED:
  case SDL_EVENT_GAMEPAD_ADDED:
  case SDL_EVENT_GAMEPAD_REMOVED:
  case SDL_EVENT_GAMEPAD_REMAPPED:
  case SDL_EVENT_AUDIO_DEVICE_ADDED:
  case SDL_EVENT_AUDIO_DEVICE_REMOVED:
  case SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED:
    return true;
  default: return false;
  }
}
std::optional<std::string> copyText(const char *text, std::size_t &remaining) {
  if (text == nullptr) return std::nullopt;
  std::size_t length = 0;
  while (length < remaining && text[length] != '\0') ++length;
  if (length == remaining) throw std::length_error("SDL event payload exceeds handoff budget");
  remaining -= length + 1;
  return std::string(text, length);
}
}

OwnedApplicationEvent::OwnedApplicationEvent(const SDL_Event &event) : event_(event) {
  std::size_t remaining = maximumPayloadBytes;
  const char *const *strings = nullptr;
  int count = 0;
  switch (event.type) {
  case SDL_EVENT_TEXT_INPUT:
    text_ = copyText(event.text.text, remaining);
    break;
  case SDL_EVENT_TEXT_EDITING:
    text_ = copyText(event.edit.text, remaining);
    break;
  case SDL_EVENT_DROP_BEGIN:
  case SDL_EVENT_DROP_FILE:
  case SDL_EVENT_DROP_TEXT:
  case SDL_EVENT_DROP_COMPLETE:
  case SDL_EVENT_DROP_POSITION:
    text_ = copyText(event.drop.data, remaining);
    source_ = copyText(event.drop.source, remaining);
    break;
  case SDL_EVENT_TEXT_EDITING_CANDIDATES:
    strings = event.edit_candidates.candidates;
    count = event.edit_candidates.num_candidates;
    break;
  case SDL_EVENT_CLIPBOARD_UPDATE:
    strings = event.clipboard.mime_types;
    count = event.clipboard.num_mime_types;
    break;
  default:
    break;
  }
  if (count < 0 || count > 1024 || (count != 0 && strings == nullptr)) {
    throw std::length_error("Invalid SDL event string array");
  }
  strings_.reserve(count);
  pointers_.resize(count);
  for (int i = 0; i < count; ++i) strings_.push_back(copyText(strings[i], remaining));
}

const SDL_Event &OwnedApplicationEvent::event() const {
  const auto text = text_ ? text_->c_str() : nullptr;
  switch (event_.type) {
  case SDL_EVENT_TEXT_INPUT: event_.text.text = text; break;
  case SDL_EVENT_TEXT_EDITING: event_.edit.text = text; break;
  case SDL_EVENT_DROP_BEGIN:
  case SDL_EVENT_DROP_FILE:
  case SDL_EVENT_DROP_TEXT:
  case SDL_EVENT_DROP_COMPLETE:
  case SDL_EVENT_DROP_POSITION:
    event_.drop.data = text;
    event_.drop.source = source_ ? source_->c_str() : nullptr;
    break;
  case SDL_EVENT_TEXT_EDITING_CANDIDATES:
  case SDL_EVENT_CLIPBOARD_UPDATE:
    for (std::size_t i = 0; i < strings_.size(); ++i) {
      pointers_[i] = strings_[i] ? strings_[i]->c_str() : nullptr;
    }
    if (event_.type == SDL_EVENT_TEXT_EDITING_CANDIDATES) {
      event_.edit_candidates.candidates = pointers_.empty() ? nullptr : pointers_.data();
    } else {
      event_.clipboard.mime_types = pointers_.empty() ? nullptr : pointers_.data();
    }
    break;
  default: break;
  }
  return event_;
}

ApplicationEventQueue::ApplicationEventQueue(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)) {}

bool ApplicationEventQueue::coalesceMotion(const SDL_Event &event) {
  if (event.type != SDL_EVENT_MOUSE_MOTION && event.type != SDL_EVENT_FINGER_MOTION) {
    return false;
  }
  // Search only the uninterrupted motion suffix: key/button/contact edges and
  // lifecycle events are ordering barriers. Distinct contacts may interleave.
  auto position = events_.end();
  while (position != events_.begin()) {
    --position;
    const auto &previous = position->event();
    if (previous.type != SDL_EVENT_MOUSE_MOTION && previous.type != SDL_EVENT_FINGER_MOTION) {
      break;
    }
    if (previous.type != event.type) continue;
    SDL_Event merged = event;
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
      if (previous.motion.windowID != event.motion.windowID ||
          previous.motion.which != event.motion.which) continue;
      if (previous.motion.state != event.motion.state) return false;
      merged.motion.xrel += previous.motion.xrel;
      merged.motion.yrel += previous.motion.yrel;
    } else {
      if (previous.tfinger.windowID != event.tfinger.windowID ||
          previous.tfinger.touchID != event.tfinger.touchID ||
          previous.tfinger.fingerID != event.tfinger.fingerID) continue;
      merged.tfinger.dx += previous.tfinger.dx;
      merged.tfinger.dy += previous.tfinger.dy;
    }
    // Keep the latest position, pressure, and timestamp, at its arrival position.
    events_.erase(position);
    events_.emplace_back(merged);
    return true;
  }
  return false;
}

void ApplicationEventQueue::preserveLifecycle(const SDL_Event &event) {
  switch (event.type) {
  case SDL_EVENT_LOW_MEMORY:
    lowMemory_ = event;
    break;
  case SDL_EVENT_WINDOW_MINIMIZED:
  case SDL_EVENT_WINDOW_HIDDEN:
  case SDL_EVENT_WINDOW_FOCUS_LOST:
  case SDL_EVENT_WINDOW_RESTORED:
  case SDL_EVENT_WINDOW_SHOWN:
  case SDL_EVENT_WINDOW_FOCUS_GAINED:
  case SDL_EVENT_WILL_ENTER_BACKGROUND:
  case SDL_EVENT_DID_ENTER_BACKGROUND:
  case SDL_EVENT_WILL_ENTER_FOREGROUND:
  case SDL_EVENT_DID_ENTER_FOREGROUND:
    lifecycle_ = event;
    break;
  case SDL_EVENT_QUIT:
  case SDL_EVENT_TERMINATING:
    quit_ = event;
    break;
  default: break;
  }
}

void ApplicationEventQueue::recover(
    const SDL_Event &incoming,
    std::optional<std::deque<OwnedApplicationEvent>> &discarded) {
  for (const auto &queued : events_) preserveLifecycle(queued.event());
  preserveLifecycle(incoming);
  discarded.emplace(std::move(events_));
  events_.clear();
  overflow_ = true;
  recovering_ = true;
  recoveryDrained_ = false;
}

bool ApplicationEventQueue::push(const SDL_Event &event, bool stateOnly,
                                 const DiscardCallback &onDiscard) {
  if (stateOnly && !retainedDuringExport(event)) {
    if (onDiscard) onDiscard(event);
    return true;
  }
  // Own SDL's temporary bytes before they expire, outside the shared lock.
  std::optional<OwnedApplicationEvent> owned;
  try {
    owned.emplace(event);
  } catch (const std::length_error &) {
    // Invalid/oversized payloads use the same bounded recovery as queue pressure.
  }
  std::optional<std::deque<OwnedApplicationEvent>> discarded;
  {
    const std::lock_guard lock(mutex_);
    if (!owned) {
      recover(event, discarded);
    } else if (recovering_) {
      preserveLifecycle(event);
    } else if (coalesceMotion(event)) {
      return true;
    } else if (events_.size() == capacity_) {
      recover(event, discarded);
    } else {
      events_.push_back(std::move(*owned));
      return true;
    }
  }
  if (onDiscard) {
    if (discarded) {
      for (const auto &queued : *discarded) onDiscard(queued.event());
    }
    onDiscard(event);
  }
  return false;
}

bool ApplicationEventQueue::poll(OwnedApplicationEvent &event) {
  OwnedApplicationEvent next;
  {
    const std::lock_guard lock(mutex_);
    if (recovering_) {
      SDL_Event recovery{};
      if (lowMemory_) {
        recovery = *std::exchange(lowMemory_, std::nullopt);
      } else if (lifecycle_) {
        recovery = *std::exchange(lifecycle_, std::nullopt);
      } else if (quit_) {
        recovery = *std::exchange(quit_, std::nullopt);
      } else {
        recoveryDrained_ = true;
        finishRecovery();
        return false;
      }
      next = OwnedApplicationEvent(recovery);
    } else {
      if (events_.empty()) return false;
      next = std::move(events_.front());
      events_.pop_front();
    }
  }
  event = std::move(next);
  return true;
}

bool ApplicationEventQueue::takeOverflow() {
  const std::lock_guard lock(mutex_);
  const bool overflow = std::exchange(overflow_, false);
  finishRecovery();
  return overflow;
}

void ApplicationEventQueue::finishRecovery() {
  // Both sides must cross the invalidated batch. Owner polling alone cannot
  // reopen admission while main is still forwarding its stale SDL backlog.
  if (recoveryDrained_ && !producerBatchActive_ && !overflow_ &&
      !lowMemory_ && !lifecycle_ && !quit_) recovering_ = false;
}

void ApplicationEventQueue::beginProducerBatch() {
  const std::lock_guard lock(mutex_);
  producerBatchActive_ = true;
}

void ApplicationEventQueue::endProducerBatch() {
  const std::lock_guard lock(mutex_);
  producerBatchActive_ = false;
  finishRecovery();
}

void ApplicationEventQueue::discardUserInput(const DiscardCallback &onDiscard) {
  std::deque<OwnedApplicationEvent> discarded;
  {
    const std::lock_guard lock(mutex_);
    for (auto position = events_.begin(); position != events_.end();) {
      if (retainedDuringExport(position->event())) {
        ++position;
      } else {
        discarded.push_back(std::move(*position));
        position = events_.erase(position);
      }
    }
  }
  if (onDiscard) {
    for (const auto &event : discarded) onDiscard(event.event());
  }
}
}
