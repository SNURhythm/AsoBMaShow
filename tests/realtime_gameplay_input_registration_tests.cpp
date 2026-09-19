#include "scene/play/RealtimeGameplayInputRegistration.h"

#include <SDL2/SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

class Backend final : public IInputBackend {
public:
  explicit Backend(input::InputBackendSink sink) : IInputBackend(std::move(sink)) {}
  bool start(std::string &) override { return true; }
  void stop() override {}
  void pump() override {}
  void sendInput() {
    publishInput({.control = {.deviceId = "keyboard",
                             .deviceClass = input::DeviceClass::Keyboard,
                             .kind = input::ControlKind::Key}});
  }
  void sendDevice() {
    publishDevice({.stableId = "keyboard", .deviceClass = input::DeviceClass::Keyboard});
  }
  void setRealtimeInputClaimed(input::DeviceClass deviceClass, bool claimed) override {
    claims.emplace_back(deviceClass, claimed);
    if (claimed && failClaim == deviceClass) throw std::runtime_error("backend activation failed");
    if (!claimed) sendInput();
  }
  std::vector<std::pair<input::DeviceClass, bool>> claims;
  std::optional<input::DeviceClass> failClaim;
};

struct Fixture {
  Backend *backend = nullptr;
  InputDeviceRegistry registry{{[this](input::InputBackendSink sink) {
    auto result = std::make_unique<Backend>(std::move(sink));
    backend = result.get();
    return result;
  }}};
  std::atomic_bool accepting = false;
  int inputs = 0, devices = 0, watches = 0;
  std::function<void()> onWatch;
  std::vector<std::pair<input::DeviceClass, bool>> legacy;

  static int SDLCALL watch(void *context, SDL_Event *event) {
    if (event->type == SDL_USEREVENT) {
      auto &fixture = *static_cast<Fixture *>(context);
      ++fixture.watches;
      if (fixture.onWatch) fixture.onWatch();
    }
    return 0;
  }
  void pushEvent() {
    SDL_Event event{};
    event.type = SDL_USEREVENT;
    require(SDL_PushEvent(&event) == 1, "SDL fixture event is accepted");
  }
  gameplay::RealtimeGameplayInputRegistration::Configuration configuration() {
    gameplay::RealtimeGameplayInputRegistration::Configuration result;
    result.claimedClasses[static_cast<std::size_t>(input::DeviceClass::Keyboard)] = true;
    result.setLegacyClassEnabled = [this](auto deviceClass, bool enabled) {
      legacy.emplace_back(deviceClass, enabled);
    };
    result.onInput = [this](const auto &) { ++inputs; };
    result.onDevice = [this](const auto &) { ++devices; };
    result.sdlWatch = &watch;
    result.sdlWatchContext = this;
    return result;
  }
};

void testRegistrationActivationAndClosePreserveRouting() {
  Fixture fixture;
  int ordinaryInputs = 0;
  const auto ordinary = fixture.registry.subscribeInput([&](const auto &) { ++ordinaryInputs; });
  gameplay::RealtimeGameplayInputRegistration registration(
      fixture.registry, fixture.accepting, fixture.configuration());
  require(fixture.legacy == std::vector{std::pair{input::DeviceClass::Keyboard, false}},
          "registration disables the selected legacy class once");
  fixture.backend->sendInput();
  fixture.backend->sendDevice();
  fixture.pushEvent();
  fixture.registry.pump();
  require(fixture.inputs == 0 && fixture.devices == 0 && fixture.watches == 0,
          "registered native callbacks remain gated until activation");
  require(ordinaryInputs == 1, "ordinary registry delivery remains available before activation");
  require(registration.activate() && registration.activate(), "activation is idempotent");
  require(fixture.backend->claims == std::vector{std::pair{input::DeviceClass::Keyboard, true}},
          "activation claims the backend class once");
  fixture.backend->sendInput();
  fixture.backend->sendDevice();
  fixture.pushEvent();
  fixture.registry.pump();
  require(fixture.inputs == 1 && fixture.devices == 1 && fixture.watches == 1,
          "active native subscriptions and SDL watch deliver events");
  require(ordinaryInputs == 1, "claimed input is excluded from ordinary frame delivery");
  registration.close();
  registration.close();
  require(!fixture.accepting && !registration.activate(), "closed registration stays closed");
  require(fixture.legacy.size() == 2 && fixture.legacy.back().second,
          "close restores legacy routing exactly once");
  require(fixture.backend->claims.size() == 2 && !fixture.backend->claims.back().second,
          "close releases the backend class exactly once");
  fixture.accepting = true; // Detect retained subscriptions, independently of their gate.
  fixture.backend->sendInput();
  fixture.backend->sendDevice();
  fixture.pushEvent();
  fixture.registry.pump();
  require(fixture.inputs == 1 && fixture.devices == 1 && fixture.watches == 1,
          "close removes all three native callback registrations");
  require(ordinaryInputs == 2, "ordinary input delivery resumes after release");
  fixture.registry.unsubscribe(ordinary);
}

void testDestructionAndUnactivatedCloseReleaseRegistrations() {
  Fixture fixture;
  {
    gameplay::RealtimeGameplayInputRegistration registration(
        fixture.registry, fixture.accepting, fixture.configuration());
  }
  require(!fixture.accepting && fixture.legacy.size() == 2 && fixture.legacy.back().second,
          "an unactivated registration restores its staged legacy routing");
  {
    gameplay::RealtimeGameplayInputRegistration registration(
        fixture.registry, fixture.accepting, fixture.configuration());
    require(registration.activate(), "a fresh owner may activate after prior teardown");
  }
  require(!fixture.accepting, "destruction closes active acceptance");
  fixture.accepting = true;
  fixture.backend->sendInput();
  fixture.backend->sendDevice();
  fixture.pushEvent();
  require(fixture.inputs == 0 && fixture.devices == 0 && fixture.watches == 0,
          "destruction leaves no callback retaining the former owner");
}

void testPartialStartupRollsBackBeforeRethrowing() {
  Fixture fixture;
  auto config = fixture.configuration();
  config.claimedClasses[static_cast<std::size_t>(input::DeviceClass::Midi)] = true;
  config.setLegacyClassEnabled = [&](auto deviceClass, bool enabled) {
    fixture.legacy.emplace_back(deviceClass, enabled);
    if (!enabled && deviceClass == input::DeviceClass::Midi) {
      throw std::runtime_error("legacy routing setup failed");
    }
  };
  bool failed = false;
  try {
    gameplay::RealtimeGameplayInputRegistration registration(
        fixture.registry, fixture.accepting, std::move(config));
  } catch (const std::runtime_error &) { failed = true; }
  require(failed && !fixture.accepting, "partial registration propagates failure with acceptance closed");
  require(fixture.legacy.size() == 4 && fixture.legacy[2].second && fixture.legacy[3].second,
          "partial startup restores every legacy class it attempted to disable");
  fixture.accepting = true;
  fixture.backend->sendInput();
  fixture.backend->sendDevice();
  require(fixture.inputs == 0 && fixture.devices == 0, "failed construction removes both subscriptions");
}

void testBackendActivationFailureClosesAllNativeInputs() {
  Fixture fixture;
  auto config = fixture.configuration();
  config.claimedClasses[static_cast<std::size_t>(input::DeviceClass::Midi)] = true;
  fixture.backend->failClaim = input::DeviceClass::Midi;
  gameplay::RealtimeGameplayInputRegistration registration(
      fixture.registry, fixture.accepting, std::move(config));
  bool failed = false;
  try { (void)registration.activate(); } catch (const std::runtime_error &) { failed = true; }
  require(failed && !fixture.accepting && !registration.activate(),
          "backend activation failure closes the owner before propagating");
  fixture.accepting = true;
  fixture.backend->sendInput();
  fixture.backend->sendDevice();
  fixture.pushEvent();
  require(fixture.inputs == 0 && fixture.devices == 0 && fixture.watches == 0,
          "partial activation leaves no native registration active");
  require(fixture.legacy.size() == 4 && fixture.legacy[2].second && fixture.legacy[3].second,
          "partial backend activation restores both legacy classes");
}

void testCloseWaitsForInFlightCallback(int source) {
  Fixture fixture;
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, release = false;
  int completed = 0;
  auto config = fixture.configuration();
  const auto callback = [&] {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    require(changed.wait_for(lock, 5s, [&] { return release; }), "callback release arrives");
    ++completed;
  };
  config.onInput = [&](const auto &) { if (source == 0) callback(); };
  config.onDevice = [&](const auto &) { if (source == 1) callback(); };
  fixture.onWatch = [&] { if (source == 2) callback(); };
  gameplay::RealtimeGameplayInputRegistration registration(
      fixture.registry, fixture.accepting, std::move(config));
  require(registration.activate(), "in-flight fixture activates");
  const auto emit = [&] {
    if (source == 0) fixture.backend->sendInput();
    else if (source == 1) fixture.backend->sendDevice();
    else fixture.pushEvent();
  };
  auto producer = std::async(std::launch::async, emit);
  {
    std::unique_lock lock(mutex);
    require(changed.wait_for(lock, 5s, [&] { return entered; }), "native callback enters");
  }
  auto close = std::async(std::launch::async, [&] { registration.close(); });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (fixture.accepting && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  const bool gateClosed = !fixture.accepting;
  const bool waited = close.wait_for(20ms) == std::future_status::timeout;
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
  producer.get();
  close.get();
  require(gateClosed && waited && completed == 1,
          "close gates new input and joins the active callback before returning");
  fixture.accepting = true;
  emit();
  require(completed == 1, "the detached callback cannot run after close");
}
} // namespace

int main() {
  if (SDL_Init(SDL_INIT_EVENTS) != 0) return 1;
  try {
    testRegistrationActivationAndClosePreserveRouting();
    testDestructionAndUnactivatedCloseReleaseRegistrations();
    testPartialStartupRollsBackBeforeRethrowing();
    testBackendActivationFailureClosesAllNativeInputs();
    testCloseWaitsForInFlightCallback(0);
    testCloseWaitsForInFlightCallback(1);
    testCloseWaitsForInFlightCallback(2);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    SDL_Quit();
    return 1;
  }
  SDL_Quit();
}
