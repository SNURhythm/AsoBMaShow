#include "ApplicationEventQueue.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace platform {
namespace {
constexpr std::size_t maximumPayloadBytes = 64 * 1024;
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

void ApplicationEventQueue::preserveLifecycle(const SDL_Event &event) {
  switch (event.type) {
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

void ApplicationEventQueue::recover(const SDL_Event &incoming) {
  for (const auto &queued : events_) preserveLifecycle(queued.event());
  preserveLifecycle(incoming);
  events_.clear();
  overflow_ = true;
  recovering_ = true;
  cancellationPending_ = true;
}

bool ApplicationEventQueue::push(const SDL_Event &event) {
  // Own SDL's temporary bytes before they expire, outside the shared lock.
  std::optional<OwnedApplicationEvent> owned;
  try {
    owned.emplace(event);
  } catch (const std::length_error &) {
    const std::lock_guard lock(mutex_);
    recover(event);
    return false;
  }
  const std::lock_guard lock(mutex_);
  if (recovering_) {
    preserveLifecycle(event);
    return false;
  }
  if (events_.size() == capacity_) {
    recover(event);
    return false;
  }
  events_.push_back(std::move(*owned));
  return true;
}

bool ApplicationEventQueue::poll(OwnedApplicationEvent &event) {
  OwnedApplicationEvent next;
  {
    const std::lock_guard lock(mutex_);
    if (recovering_) {
      SDL_Event recovery{};
      if (cancellationPending_) {
        recovery.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        cancellationPending_ = false;
      } else if (lifecycle_) {
        recovery = *std::exchange(lifecycle_, std::nullopt);
      } else if (quit_) {
        recovery = *std::exchange(quit_, std::nullopt);
      } else {
        recovering_ = false;
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
  return std::exchange(overflow_, false);
}
}
