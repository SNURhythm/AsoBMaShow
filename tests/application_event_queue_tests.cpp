#include "platform/ApplicationEventQueue.h"
#include "platform/GenerationMailbox.h"
#include "platform/ApplicationThreadHost.h"

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

void testExportDefersViewportAndDeviceStateButDropsInput() {
  platform::ApplicationEventQueue queue(8);
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  require(queue.push(event, true), "suppressed export input is not queue pressure");
  for (const auto type : {SDL_EVENT_WINDOW_RESIZED, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED,
                         SDL_EVENT_JOYSTICK_ADDED, SDL_EVENT_JOYSTICK_REMOVED,
                         SDL_EVENT_DID_ENTER_FOREGROUND}) {
    event.type = type;
    require(queue.push(event, true), "export lost state event");
  }
  platform::OwnedApplicationEvent owned;
  for (const auto type : {SDL_EVENT_WINDOW_RESIZED, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED,
                         SDL_EVENT_JOYSTICK_ADDED, SDL_EVENT_JOYSTICK_REMOVED,
                         SDL_EVENT_DID_ENTER_FOREGROUND}) {
    require(queue.poll(owned) && owned.event().type == type,
            "export completion/cancellation must deliver deferred state in order");
  }
  require(!queue.poll(owned), "export replayed suppressed input");
}

void testExportRetainsLowMemoryAcrossOverflowRecovery() {
  platform::ApplicationEventQueue queue(2);
  SDL_Event event{};
  event.type = SDL_EVENT_LOW_MEMORY;
  event.common.timestamp = 17;
  require(queue.push(event, true), "export rejected low-memory warning");
  platform::OwnedApplicationEvent owned;
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_LOW_MEMORY &&
          owned.event().common.timestamp == 17, "export lost low-memory warning");
  for (int position = 0; position < 4; ++position) {
    for (int i = 0; i < 4; ++i) {
      event.type = i == position ? SDL_EVENT_LOW_MEMORY : SDL_EVENT_WINDOW_RESIZED;
      event.common.timestamp = 23;
      queue.push(event, true);
    }
    event.type = SDL_EVENT_DID_ENTER_FOREGROUND;
    queue.push(event, true);
    event.type = SDL_EVENT_QUIT;
    queue.push(event, true);
    require(queue.takeOverflow(), "state flood did not exercise recovery");
    bool lowMemory = false, foreground = false, quit = false;
    while (queue.poll(owned)) {
      require(owned.event().type != SDL_EVENT_WINDOW_FOCUS_LOST,
              "queue pressure must not synthesize OS focus loss");
      if (owned.event().type == SDL_EVENT_LOW_MEMORY) {
        lowMemory = owned.event().common.timestamp == 23;
      }
      foreground |= owned.event().type == SDL_EVENT_DID_ENTER_FOREGROUND;
      quit |= owned.event().type == SDL_EVENT_QUIT;
    }
    require(lowMemory && foreground && quit,
            "overflow lost low-memory, foreground, or quit state");
  }
}

void testMouseMotionFloodPreservesEdgesAndRelativeTravel() {
  platform::ApplicationEventQueue queue;
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.scancode = SDL_SCANCODE_A;
  queue.push(event);
  event = {};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_LEFT;
  queue.push(event);
  event = {};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.windowID = 3;
  event.motion.which = 7;
  event.motion.state = SDL_BUTTON_LMASK;
  event.motion.xrel = 0.5f;
  event.motion.yrel = -0.25f;
  for (int i = 0; i < 512; ++i) {
    event.motion.timestamp = i + 1;
    event.motion.x = i;
    event.motion.y = i + 10;
    require(queue.push(event), "mouse motion flood spuriously cancelled gameplay");
  }
  event.type = SDL_EVENT_KEY_UP;
  event.key.scancode = SDL_SCANCODE_A;
  require(queue.push(event), "motion flood lost key release");
  event = {};
  event.type = SDL_EVENT_MOUSE_BUTTON_UP;
  event.button.button = SDL_BUTTON_LEFT;
  require(queue.push(event), "motion flood lost button release");
  platform::OwnedApplicationEvent owned;
  require(!queue.takeOverflow(), "coalescible mouse motion reported overflow");
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_KEY_DOWN &&
          owned.event().key.scancode == SDL_SCANCODE_A, "key press order changed");
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_MOUSE_BUTTON_DOWN,
          "mouse press order changed");
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_MOUSE_MOTION &&
          owned.event().motion.timestamp == 512 && owned.event().motion.x == 511 &&
          owned.event().motion.y == 521 && owned.event().motion.xrel == 256 &&
          owned.event().motion.yrel == -128 && owned.event().motion.windowID == 3 &&
          owned.event().motion.which == 7 && owned.event().motion.state == SDL_BUTTON_LMASK,
          "coalescing lost final mouse position or accumulated relative travel");
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_KEY_UP &&
          owned.event().key.scancode == SDL_SCANCODE_A, "key release order changed");
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_MOUSE_BUTTON_UP,
          "mouse release order changed");
  require(!queue.poll(owned), "motion flood synthesized focus loss");
}

void testInterleavedTouchMotionPreservesContactsAndRelativeTravel() {
  platform::ApplicationEventQueue queue;
  SDL_Event event{};
  event.type = SDL_EVENT_FINGER_DOWN;
  event.tfinger.touchID = 1;
  event.tfinger.fingerID = 2;
  event.tfinger.windowID = 3;
  queue.push(event);
  event.type = SDL_EVENT_FINGER_MOTION;
  event.tfinger.dx = 0.125f;
  event.tfinger.dy = -0.0625f;
  for (int i = 0; i < 128; ++i) {
    for (int contact = 0; contact < 4; ++contact) {
      event.tfinger.touchID = contact == 2 ? 4 : 1;
      event.tfinger.fingerID = contact == 1 ? 5 : 2;
      event.tfinger.windowID = contact == 3 ? 6 : 3;
      event.tfinger.timestamp = i * 4 + contact;
      event.tfinger.x = i / 128.0f;
      event.tfinger.y = contact / 4.0f;
      event.tfinger.pressure = 0.75f;
      require(queue.push(event), "interleaved touch motion spuriously cancelled gameplay");
    }
  }
  event = {};
  event.type = SDL_EVENT_FINGER_UP;
  event.tfinger.touchID = 1;
  event.tfinger.fingerID = 2;
  event.tfinger.windowID = 3;
  queue.push(event);
  event.type = SDL_EVENT_FINGER_DOWN;
  queue.push(event);
  event.type = SDL_EVENT_FINGER_MOTION;
  event.tfinger.dx = 0.25f;
  queue.push(event);
  platform::OwnedApplicationEvent owned;
  require(!queue.takeOverflow(), "coalescible touch motion reported overflow");
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_FINGER_DOWN,
          "motion crossed contact start");
  for (int contact = 0; contact < 4; ++contact) {
    require(queue.poll(owned) && owned.event().type == SDL_EVENT_FINGER_MOTION,
            "touch motion missing");
    const auto &finger = owned.event().tfinger;
    require(finger.touchID == (contact == 2 ? 4 : 1) &&
            finger.fingerID == (contact == 1 ? 5 : 2) &&
            finger.windowID == (contact == 3 ? 6 : 3) &&
            finger.timestamp == Uint64(508 + contact) && finger.x == 127 / 128.0f &&
            finger.y == contact / 4.0f && finger.pressure == 0.75f &&
            finger.dx == 16 && finger.dy == -8,
            "coalescing mixed contacts or lost touch position/pressure/relative travel");
  }
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_FINGER_UP,
          "motion crossed contact end");
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_FINGER_DOWN,
          "motion crossed reused contact ID");
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_FINGER_MOTION &&
          owned.event().tfinger.dx == 0.25f, "coalescing combined separate contacts");
  require(!queue.poll(owned), "touch flood synthesized focus loss");
}

void testMouseMotionDoesNotCrossButtonStateOrKeyEdges() {
  platform::ApplicationEventQueue queue;
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.xrel = 1;
  queue.push(event);
  event.motion.state = SDL_BUTTON_LMASK;
  queue.push(event);
  event.motion.state = 0;
  queue.push(event);
  event.type = SDL_EVENT_KEY_DOWN;
  queue.push(event);
  event = {};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.xrel = 2;
  queue.push(event);
  platform::OwnedApplicationEvent owned;
  for (const auto state : {0u, SDL_BUTTON_LMASK, 0u}) {
    require(queue.poll(owned) && owned.event().type == SDL_EVENT_MOUSE_MOTION &&
            owned.event().motion.state == state && owned.event().motion.xrel == 1,
            "coalescing crossed a mouse button-state boundary");
  }
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_KEY_DOWN,
          "coalescing reordered key edge");
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_MOUSE_MOTION &&
          owned.event().motion.xrel == 2, "coalescing crossed key edge");
  require(!queue.poll(owned), "unexpected events after mouse boundaries");
}

void testMouseMotionKeepsDevicesAndWindowsSeparateAtCapacity() {
  platform::ApplicationEventQueue queue(3);
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_MOTION;
  for (int i = 0; i < 2; ++i) {
    for (int mouse = 0; mouse < 3; ++mouse) {
      event.motion.windowID = mouse == 2 ? 5 : 1;
      event.motion.which = mouse == 1 ? 7 : 2;
      event.motion.x = i * 10 + mouse;
      event.motion.xrel = mouse + 1;
      require(queue.push(event), "compatible motion overflowed a full queue");
    }
  }
  platform::OwnedApplicationEvent owned;
  for (int mouse = 0; mouse < 3; ++mouse) {
    require(queue.poll(owned) && owned.event().type == SDL_EVENT_MOUSE_MOTION &&
            owned.event().motion.windowID == (mouse == 2 ? 5 : 1) &&
            owned.event().motion.which == (mouse == 1 ? 7 : 2) &&
            owned.event().motion.x == 10 + mouse &&
            owned.event().motion.xrel == 2 * (mouse + 1),
            "coalescing mixed mouse devices or windows");
  }
  require(!queue.poll(owned) && !queue.takeOverflow(), "full motion queue lost focus");
}

void testHostServicesCleanupBeforeJoinAndPropagatesStartupFailure() {
  std::atomic_bool needsMain = false;
  std::atomic_bool serviced = false;
  bool reported = false;
  const auto mainThread = std::this_thread::get_id();
  auto application = [&]() -> int {
    require(std::this_thread::get_id() != mainThread, "application stayed on main");
    struct Cleanup {
      std::atomic_bool &request, &done;
      ~Cleanup() {
        request = true;
        while (!done.load()) std::this_thread::yield();
      }
    } cleanup{needsMain, serviced};
    throw std::runtime_error("Startup failure");
  };
  auto service = [&] {
    require(std::this_thread::get_id() == mainThread, "native work left main");
    if (needsMain.load()) serviced = true;
    std::this_thread::yield();
  };
  const int result = platform::runApplicationThread(application, service,
      [&](std::exception_ptr failure) {
        try { std::rethrow_exception(failure); }
        catch (const std::runtime_error &) { reported = true; }
      });
  require(result == EXIT_FAILURE && reported && serviced,
          "worker failure must still service pending main cleanup before join");
  bool started = false, pumped = false;
  bool launchReported = false;
  const int launchResult = platform::runApplicationThread(
      [&] { started = true; return 0; }, [&] { pumped = true; },
      [&](auto) { launchReported = true; },
      [](auto) -> std::thread { throw std::runtime_error("No thread"); });
  require(launchResult == EXIT_FAILURE && launchReported,
          "thread creation failure must return through normal native cleanup");
  require(!started && !pumped, "failed startup must not run either loop");
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

void testEdgesRemainOrderedAndOverflowPreservesLifecycleWithoutFocusLoss() {
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
  event.type = SDL_EVENT_WINDOW_HIDDEN;
  queue.push(event);
  event.type = SDL_EVENT_KEY_DOWN;
  for (int i = 0; i < 5; ++i) queue.push(event);
  bool hidden = false;
  while (queue.poll(owned)) hidden |= owned.event().type == SDL_EVENT_WINDOW_HIDDEN;
  require(hidden, "pressure lost native window lifecycle state");
}

void testDiscardCallbacksRunUnlockedForEverySuppressedOrLostEvent() {
  platform::ApplicationEventQueue queue(2);
  std::vector<Uint64> discarded;
  const auto onDiscard = [&](const SDL_Event &event) {
    (void)queue.takeOverflow(); // A callback must be allowed to reenter the queue.
    discarded.push_back(event.common.timestamp);
  };
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  for (Uint64 timestamp = 1; timestamp <= 4; ++timestamp) {
    event.common.timestamp = timestamp;
    queue.push(event, false, onDiscard);
  }
  event.common.timestamp = 5;
  queue.push(event, true, onDiscard);
  platform::OwnedApplicationEvent owned;
  require(!queue.poll(owned), "discarded input must not manufacture lifecycle events");
  std::string tooLarge(64 * 1024, 'x');
  event = {};
  event.type = SDL_EVENT_TEXT_INPUT;
  event.text.timestamp = 6;
  event.text.text = tooLarge.c_str();
  require(!queue.push(event, false, onDiscard), "oversized payload must enter recovery");
  require(discarded == std::vector<Uint64>({1, 2, 3, 4, 5, 6}),
          "discard notification lost a queued, rejected, suppressed, or oversized event");
}

void testRealFocusLossRemainsDeliverableDuringRecovery() {
  platform::ApplicationEventQueue queue(1);
  SDL_Event event{};
  event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  event.window.timestamp = 77;
  event.window.windowID = 3;
  queue.push(event);
  event.type = SDL_EVENT_KEY_UP;
  require(!queue.push(event), "fixture overflows after native focus loss");
  platform::OwnedApplicationEvent owned;
  require(queue.poll(owned) && owned.event().type == SDL_EVENT_WINDOW_FOCUS_LOST &&
          owned.event().window.timestamp == 77 && owned.event().window.windowID == 3,
          "recovery must retain actual OS focus loss");
  require(!queue.poll(owned), "recovery manufactured another focus event");
}

void testExportStartDiscardsBufferedInputAndRetainsSystemState() {
  platform::ApplicationEventQueue queue(16);
  SDL_Event event{};
  const std::vector<Uint32> types{
      SDL_EVENT_KEY_DOWN, SDL_EVENT_WINDOW_RESIZED, SDL_EVENT_FINGER_UP,
      SDL_EVENT_JOYSTICK_ADDED, SDL_EVENT_LOW_MEMORY, SDL_EVENT_MOUSE_BUTTON_DOWN,
      SDL_EVENT_QUIT};
  for (const auto type : types) {
    event.type = type;
    queue.push(event);
  }
  std::vector<Uint32> discarded;
  queue.discardUserInput([&](const SDL_Event &lost) {
    (void)queue.takeOverflow(); // Callback must run outside the queue mutex.
    discarded.push_back(lost.type);
  });
  require(discarded == std::vector<Uint32>{SDL_EVENT_KEY_DOWN, SDL_EVENT_FINGER_UP,
                                          SDL_EVENT_MOUSE_BUTTON_DOWN},
          "export start did not retire every buffered user-input acknowledgement");
  platform::OwnedApplicationEvent owned;
  for (const auto expected : {SDL_EVENT_WINDOW_RESIZED, SDL_EVENT_JOYSTICK_ADDED,
                              SDL_EVENT_LOW_MEMORY, SDL_EVENT_QUIT}) {
    require(queue.poll(owned) && owned.event().type == expected,
            "export start lost or reordered buffered system state");
  }
  require(!queue.poll(owned), "export start retained buffered user input");
}

void testRecoveryWaitsForProducerBatchAndOwnerAcknowledgement() {
  platform::ApplicationEventQueue queue(1);
  platform::OwnedApplicationEvent owned;
  SDL_Event key{};
  key.type = SDL_EVENT_KEY_DOWN;
  std::promise<void> overflowed, ownerRecovered;
  auto recovered = ownerRecovered.get_future();
  std::thread producer([&] {
    queue.beginProducerBatch();
    queue.push(key);
    require(!queue.push(key), "fixture did not overflow");
    overflowed.set_value();
    recovered.wait();
    require(!queue.push(key), "owner recovery admitted stale producer-batch input");
    queue.endProducerBatch();
  });
  overflowed.get_future().wait();
  require(!queue.poll(owned), "overflow retained ordinary input");
  require(queue.takeOverflow(), "owner lost overflow acknowledgement");
  ownerRecovered.set_value();
  producer.join();
  require(!queue.poll(owned), "stale producer tail was replayed");
  queue.beginProducerBatch();
  require(queue.push(key), "fresh producer batch remained suppressed");
  queue.endProducerBatch();
  require(queue.poll(owned), "fresh input was lost after recovery");

  queue.beginProducerBatch();
  queue.push(key);
  require(!queue.push(key), "second fixture did not overflow");
  queue.endProducerBatch();
  require(!queue.poll(owned), "second overflow retained input");
  queue.beginProducerBatch();
  require(!queue.push(key), "producer completion bypassed owner acknowledgement");
  require(queue.takeOverflow(), "second owner acknowledgement was lost");
  require(!queue.push(key), "acknowledgement reopened an in-progress producer batch");
  queue.endProducerBatch();
  require(queue.push(key), "completed recovery did not reopen input");
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
  testMouseMotionFloodPreservesEdgesAndRelativeTravel();
  testInterleavedTouchMotionPreservesContactsAndRelativeTravel();
  testMouseMotionDoesNotCrossButtonStateOrKeyEdges();
  testMouseMotionKeepsDevicesAndWindowsSeparateAtCapacity();
  testExportRetainsLowMemoryAcrossOverflowRecovery();
  testExportDefersViewportAndDeviceStateButDropsInput();
  testHostServicesCleanupBeforeJoinAndPropagatesStartupFailure();
  testPayloadSurvivesSourceAndQueueMoves();
  testEdgesRemainOrderedAndOverflowPreservesLifecycleWithoutFocusLoss();
  testDiscardCallbacksRunUnlockedForEverySuppressedOrLostEvent();
  testRealFocusLossRemainsDeliverableDuringRecovery();
  testExportStartDiscardsBufferedInputAndRetainsSystemState();
  testRecoveryWaitsForProducerBatchAndOwnerAcknowledgement();
  testStaleNativeOwnerCannotReceiveOrInvalidateReplacement();
  testProducerCompletesWhileConsumerIsStalled();
  std::cout << "application event handoff tests passed\n";
}
