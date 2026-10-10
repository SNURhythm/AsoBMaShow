#pragma once

#include <SDL3/SDL_events.h>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace platform {

// SDL releases temporary text/drop storage on subsequent pump iterations.
// Rebind on access so copying or moving this value never leaves dangling SSO
// string pointers. Custom user events retain their producer-owned contract.
class OwnedApplicationEvent {
public:
  OwnedApplicationEvent() = default;
  explicit OwnedApplicationEvent(const SDL_Event &event);
  const SDL_Event &event() const;

private:
  mutable SDL_Event event_{};
  std::optional<std::string> text_;
  std::optional<std::string> source_;
  std::vector<std::optional<std::string>> strings_;
  mutable std::vector<const char *> pointers_;
};

// Main produces, application worker consumes. No caller code runs under the
// lock. Pressure invalidates the buffered input stream; focus cancellation and
// the final lifecycle/quit state remain deliverable without queue allocation.
class ApplicationEventQueue {
public:
  explicit ApplicationEventQueue(std::size_t capacity = 256);
  bool push(const SDL_Event &event);
  bool poll(OwnedApplicationEvent &event);
  bool takeOverflow();

private:
  void recover(const SDL_Event &incoming);
  void preserveLifecycle(const SDL_Event &event);
  const std::size_t capacity_;
  std::mutex mutex_;
  std::deque<OwnedApplicationEvent> events_;
  std::optional<SDL_Event> lifecycle_;
  std::optional<SDL_Event> quit_;
  bool overflow_ = false;
  bool recovering_ = false;
  bool cancellationPending_ = false;
};
}
