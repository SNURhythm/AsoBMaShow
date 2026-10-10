#include "platform/ApplicationEventQueue.h"
#include "platform/GenerationMailbox.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <thread>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testPayloadSurvivesSourceAndQueueMoves() {
  platform::ApplicationEventQueue queue(16);
  {
    std::string text = "한글";
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = text.c_str();
    queue.push(event);
    event.type = SDL_EVENT_TEXT_EDITING;
    event.edit.text = text.c_str();
    event.edit.start = 1;
    queue.push(event);
    event = {};
    event.type = SDL_EVENT_DROP_FILE;
    event.drop.data = text.c_str();
    event.drop.source = "Files";
    queue.push(event);
    const char *candidates[] = {text.c_str(), "候補"};
    event = {};
    event.type = SDL_EVENT_TEXT_EDITING_CANDIDATES;
    event.edit_candidates.candidates = candidates;
    event.edit_candidates.num_candidates = 2;
    event.edit_candidates.selected_candidate = 1;
    queue.push(event);
    event = {};
    event.type = SDL_EVENT_CLIPBOARD_UPDATE;
    event.clipboard.mime_types = candidates;
    event.clipboard.num_mime_types = 2;
    queue.push(event);
    text.assign(100, 'x');
  }
  platform::OwnedApplicationEvent owned;
  require(queue.poll(owned), "text event missing");
  auto copy = owned;
  owned = {};
  require(std::string(copy.event().text.text) == "한글", "text payload was borrowed");
  require(queue.poll(owned) && std::string(owned.event().edit.text) == "한글" &&
          owned.event().edit.start == 1, "editing payload/selection changed");
  require(queue.poll(owned) && std::string(owned.event().drop.data) == "한글" &&
          std::string(owned.event().drop.source) == "Files", "drop payload was borrowed");
  require(queue.poll(owned) && owned.event().edit_candidates.num_candidates == 2 &&
          std::string(owned.event().edit_candidates.candidates[0]) == "한글" &&
          std::string(owned.event().edit_candidates.candidates[1]) == "候補" &&
          owned.event().edit_candidates.selected_candidate == 1, "candidate payload was borrowed");
  require(queue.poll(owned) && owned.event().clipboard.num_mime_types == 2 &&
          std::string(owned.event().clipboard.mime_types[0]) == "한글", "clipboard payload was borrowed");
}

void testEdgesRemainOrderedAndOverflowCancelsBeforeRecovery() {
  platform::ApplicationEventQueue queue(3);
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.scancode = SDL_SCANCODE_A;
  queue.push(event);
  event.type = SDL_EVENT_KEY_UP;
  queue.push(event);
  platform::OwnedApplicationEvent owned;
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_KEY_DOWN,
          "key down order changed");
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_KEY_UP,
          "key release was lost");
  event.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
  queue.push(event);
  event.type = SDL_EVENT_KEY_DOWN;
  queue.push(event);
  queue.push(event);
  require(!queue.push(event), "overflow must be reported to the producer");
  require(queue.takeOverflow() && !queue.takeOverflow(), "overflow latch is not edge-triggered");
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_WINDOW_FOCUS_LOST,
          "pressure must cancel input before replaying events");
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_WILL_ENTER_BACKGROUND,
          "pressure lost pending background state");
  require(!queue.poll(owned), "stale input survived pressure recovery");
  event.type = SDL_EVENT_DID_ENTER_FOREGROUND;
  queue.push(event);
  event.type = SDL_EVENT_QUIT;
  queue.push(event);
  event.type = SDL_EVENT_KEY_DOWN;
  for (int i = 0; i < 20; ++i) queue.push(event);
  bool foreground = false, quit = false;
  while (queue.poll(owned)) {
    foreground |= owned.event().type == SDL_EVENT_DID_ENTER_FOREGROUND;
    quit |= owned.event().type == SDL_EVENT_QUIT;
  }
  require(foreground && quit, "pressure lost foreground or quit");
}

void testStaleNativeOwnerCannotReceiveOrInvalidateReplacement() {
  platform::GenerationMailbox<std::string> mailbox;
  const auto old = mailbox.open();
  mailbox.post(old, "old callback");
  mailbox.close(old);
  const auto current = mailbox.open();
  mailbox.post(old, "late callback");
  mailbox.post(current, "current callback");
  mailbox.close(old);
  require(mailbox.take(old).empty(), "destroyed owner received callback");
  auto events = mailbox.take(current);
  require(events.size() == 1 && events[0] == "current callback",
          "stale owner affected replacement callback");
}

void testProducerCompletesWhileConsumerIsStalled() {
  using namespace std::chrono_literals;
  platform::ApplicationEventQueue queue(256);
  std::promise<void> releaseConsumer, consumerEntered;
  auto release = releaseConsumer.get_future();
  std::thread consumer([&] {
    platform::OwnedApplicationEvent owned;
    consumerEntered.set_value();
    release.wait(); // Models a Metal/GPU stall outside the queue operation.
    queue.poll(owned);
  });
  consumerEntered.get_future().wait();
  auto producer = std::async(std::launch::async, [&] {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_UP;
    for (int i = 0; i < 200; ++i) require(queue.push(event), "queue filled too early");
  });
  const bool progressed = producer.wait_for(2s) == std::future_status::ready;
  releaseConsumer.set_value();
  consumer.join();
  producer.get();
  require(progressed, "producer waited for the stalled render consumer");
}
}

int main() {
  testPayloadSurvivesSourceAndQueueMoves();
  testEdgesRemainOrderedAndOverflowCancelsBeforeRecovery();
  testStaleNativeOwnerCannotReceiveOrInvalidateReplacement();
  testProducerCompletesWhileConsumerIsStalled();
  std::cout << "application event handoff tests passed\n";
}
