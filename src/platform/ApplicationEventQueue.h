#pragma once

#include <SDL3/SDL_events.h>
#include <cstddef>
#include <deque>
#include <functional>
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
// lock. Compatible motion is coalesced between input edges. Pressure invalidates
// buffered input, without synthesizing focus loss. Low-memory warnings and final
// lifecycle/quit state remain deliverable without queue allocation.
class ApplicationEventQueue {
public:
  using DiscardCallback = std::function<void(const SDL_Event &)>;
  explicit ApplicationEventQueue(std::size_t capacity = 256);
  // Export suppresses user input but defers system/window/device state for its owner.
  // onDiscard runs synchronously outside the queue lock for removed, rejected,
  // or suppressed events. The caller keeps the callback alive until return.
  bool push(const SDL_Event &event, bool stateOnly = false,
            const DiscardCallback &onDiscard = {});
  bool poll(OwnedApplicationEvent &event);
  // The owner acknowledges after draining retained events, before processing
  // fresh input. Recovery also waits for the producer's current batch to end.
  bool takeOverflow();
  // The single producer brackets each complete SDL poll batch, including its
  // viewport publication. Never end a batch just because the consumer caught up.
  void beginProducerBatch();
  void endProducerBatch();
  void discardUserInput(const DiscardCallback &onDiscard = {});

private:
  bool coalesceMotion(const SDL_Event &event);
  void recover(const SDL_Event &incoming,
               std::optional<std::deque<OwnedApplicationEvent>> &discarded);
  void preserveLifecycle(const SDL_Event &event);
  void finishRecovery();
  const std::size_t capacity_;
  std::mutex mutex_;
  std::deque<OwnedApplicationEvent> events_;
  std::optional<SDL_Event> lifecycle_;
  std::optional<SDL_Event> lowMemory_;
  std::optional<SDL_Event> quit_;
  bool overflow_ = false;
  bool recovering_ = false;
  bool producerBatchActive_ = false;
  bool recoveryDrained_ = false;
};
}
