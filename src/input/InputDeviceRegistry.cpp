#include "InputDeviceRegistry.h"

#include "MidiInputBackendFactory.h"
#include "GyroscopeInputBackendFactory.h"
#include "SDLInputBackend.h"
#include "../targets.h"
#if TARGET_OS_OSX || TARGET_OS_LINUX
#include "DesktopRealtimeKeyboardBackend.h"
#endif
#if defined(_WIN32)
#include "RealtimeControllerDeviceMap.h"
#include "WindowsRealtimeInputBackend.h"
#endif

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <mutex>
#include <limits>
#include <utility>

namespace {

input::InputDeviceSnapshot keyboardSnapshot() {
  return {.stableId = "keyboard",
          .displayName = "Keyboard",
          .deviceClass = input::DeviceClass::Keyboard,
          .connected = true};
}

std::vector<InputDeviceRegistry::BackendFactory> defaultBackendFactories() {
  std::vector<InputDeviceRegistry::BackendFactory> factories;
#if defined(_WIN32) || TARGET_OS_OSX || TARGET_OS_LINUX
  auto controllerMap = std::make_shared<RealtimeControllerDeviceMap>();
  factories.emplace_back(
      [controllerMap](input::InputBackendSink sink) {
        return std::make_unique<SDLInputBackend>(std::move(sink), nullptr,
                                                 controllerMap);
      });
#else
  factories.emplace_back([](input::InputBackendSink sink) {
    return std::make_unique<SDLInputBackend>(std::move(sink));
  });
#endif
  factories.emplace_back([](input::InputBackendSink sink) {
    return makeMidiInputBackend(std::move(sink));
  });
  factories.emplace_back([](input::InputBackendSink sink) {
    return makeGyroscopeInputBackend(std::move(sink));
  });
#if defined(_WIN32)
  factories.emplace_back(
      [controllerMap](input::InputBackendSink sink) {
        return makeWindowsRealtimeInputBackend(std::move(sink), controllerMap);
      });
#endif
#if TARGET_OS_OSX || TARGET_OS_LINUX
  factories.emplace_back(
      [controllerMap](input::InputBackendSink sink) {
        return makeDesktopRealtimeKeyboardBackend(std::move(sink), controllerMap);
      });
#endif
  return factories;
}

} // namespace

struct InputDeviceRegistry::QueueState {
  struct RealtimeInputSubscriptionState {
    explicit RealtimeInputSubscriptionState(InputListener inputListener)
        : listener(std::move(inputListener)) {}
    InputListener listener;
  };

  struct RealtimeDeviceSubscriptionState {
    explicit RealtimeDeviceSubscriptionState(DeviceListener deviceListener)
        : listener(std::move(deviceListener)) {}
    DeviceListener listener;
  };

  struct RealtimeInterruptionSubscriptionState {
    explicit RealtimeInterruptionSubscriptionState(InterruptionListener callback)
        : listener(std::move(callback)) {}
    InterruptionListener listener;
  };

  void enqueue(QueuedPayload payload) {
    const std::lock_guard lock(mutex);
    if (!accepting) {
      return;
    }
    const std::uint64_t sequence = nextSequence++;
    if (const auto *interruption = std::get_if<input::InputInterruption>(&payload)) {
      const auto lastToken = realtimeInterruptionListeners.empty()
          ? 0 : realtimeInterruptionListeners.rbegin()->first;
      std::uint64_t previousToken = 0;
      while (previousToken < lastToken) {
        const auto listener = realtimeInterruptionListeners.upper_bound(previousToken);
        if (listener == realtimeInterruptionListeners.end() || listener->first > lastToken) break;
        previousToken = listener->first;
        const auto retained = listener->second;
        retained->listener(*interruption);
      }
      return;
    }
    if (const auto *event =
            std::get_if<input::PhysicalInputEvent>(&payload)) {
      const std::uint64_t lastEligibleToken =
          realtimeInputListeners.empty() ? 0
                                         : realtimeInputListeners.rbegin()->first;
      std::uint64_t previousToken = 0;
      while (previousToken < lastEligibleToken) {
        const auto listener = realtimeInputListeners.upper_bound(previousToken);
        if (listener == realtimeInputListeners.end() ||
            listener->first > lastEligibleToken) {
          break;
        }
        previousToken = listener->first;
        // Keep the callable alive if it unsubscribes itself recursively.
        const auto listenerState = listener->second;
        try {
          listenerState->listener(*event);
        } catch (...) {
          SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                       "Realtime input listener threw an exception");
        }
      }
      const auto deviceClass =
          static_cast<std::size_t>(event->control.deviceClass);
      if (deviceClass < realtimeInputClaimed.size() &&
          realtimeInputClaimed[deviceClass]) {
        return;
      }
    } else if (const auto *device =
                   std::get_if<input::InputDeviceSnapshot>(&payload)) {
      const std::uint64_t lastEligibleToken =
          realtimeDeviceListeners.empty()
              ? 0
              : realtimeDeviceListeners.rbegin()->first;
      std::uint64_t previousToken = 0;
      while (previousToken < lastEligibleToken) {
        const auto listener =
            realtimeDeviceListeners.upper_bound(previousToken);
        if (listener == realtimeDeviceListeners.end() ||
            listener->first > lastEligibleToken) {
          break;
        }
        previousToken = listener->first;
        const auto listenerState = listener->second;
        try {
          listenerState->listener(*device);
        } catch (...) {
          SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                       "Realtime device listener threw an exception");
        }
      }
    }
    QueuedEvent event{.sequence = sequence, .payload = std::move(payload)};
    if (queue.empty() || queue.back().sequence < sequence) {
      queue.push_back(std::move(event));
      return;
    }
    const auto position = std::ranges::upper_bound(
        queue, sequence, {}, &QueuedEvent::sequence);
    queue.insert(position, std::move(event));
  }

  void enqueue(std::deque<QueuedPayload> payloads) {
    for (auto &payload : payloads) {
      enqueue(std::move(payload));
    }
  }

  void subscribeRealtime(std::uint64_t token, InputListener listener) {
    const std::lock_guard lock(mutex);
    if (accepting) {
      realtimeInputListeners.emplace(
          token, std::make_shared<RealtimeInputSubscriptionState>(
                     std::move(listener)));
    }
  }

  void unsubscribeRealtime(std::uint64_t token) {
    const std::lock_guard lock(mutex);
    realtimeInputListeners.erase(token);
    realtimeDeviceListeners.erase(token);
    realtimeInterruptionListeners.erase(token);
  }

  void subscribeRealtimeDevices(std::uint64_t token,
                                DeviceListener listener) {
    const std::lock_guard lock(mutex);
    if (accepting) {
      realtimeDeviceListeners.emplace(
          token, std::make_shared<RealtimeDeviceSubscriptionState>(
                     std::move(listener)));
    }
  }

  void subscribeRealtimeInterruptions(std::uint64_t token,
                                      InterruptionListener listener) {
    const std::lock_guard lock(mutex);
    if (accepting) {
      realtimeInterruptionListeners.emplace(
          token, std::make_shared<RealtimeInterruptionSubscriptionState>(std::move(listener)));
    }
  }

  void setRealtimeClaimed(input::DeviceClass deviceClass, bool claimed) {
    const std::lock_guard lock(mutex);
    const auto index = static_cast<std::size_t>(deviceClass);
    if (index < realtimeInputClaimed.size()) {
      realtimeInputClaimed[index] = claimed;
    }
  }

  template <typename Callback> void atSequenceBoundary(Callback &&callback) {
    const std::lock_guard lock(mutex);
    callback(nextSequence);
  }

  std::deque<QueuedEvent> take() {
    std::deque<QueuedEvent> result;
    const std::lock_guard lock(mutex);
    result.swap(queue);
    return result;
  }

  void close() {
    const std::lock_guard lock(mutex);
    accepting = false;
    queue.clear();
    realtimeInputListeners.clear();
    realtimeDeviceListeners.clear();
    realtimeInterruptionListeners.clear();
  }

  std::recursive_mutex mutex;
  std::deque<QueuedEvent> queue;
  std::map<std::uint64_t, std::shared_ptr<RealtimeInputSubscriptionState>>
      realtimeInputListeners;
  std::map<std::uint64_t, std::shared_ptr<RealtimeDeviceSubscriptionState>>
      realtimeDeviceListeners;
  std::map<std::uint64_t, std::shared_ptr<RealtimeInterruptionSubscriptionState>>
      realtimeInterruptionListeners;
  std::array<bool, 6> realtimeInputClaimed{};
  std::uint64_t nextSequence = 1;
  bool accepting = true;
};

struct InputDeviceRegistry::BackendSinkGate {
  void enqueue(QueuedPayload payload) {
    const std::lock_guard lock(mutex);
    if (closed) {
      return;
    }
    if (!active) {
      staged.emplace_back(std::move(payload));
      return;
    }
    if (const auto destinationQueue = destination.lock()) {
      destinationQueue->enqueue(std::move(payload));
    }
  }

  void activate() {
    const std::lock_guard lock(mutex);
    if (closed) {
      return;
    }
    active = true;
    if (const auto destinationQueue = destination.lock()) {
      destinationQueue->enqueue(std::move(staged));
    }
    staged.clear();
  }

  void close() {
    const std::lock_guard lock(mutex);
    closed = true;
    staged.clear();
  }

  std::mutex mutex;
  std::weak_ptr<QueueState> destination;
  std::deque<QueuedPayload> staged;
  bool active = false;
  bool closed = false;
};

InputDeviceRegistry::InputDeviceRegistry()
    : InputDeviceRegistry(defaultBackendFactories()) {}

InputDeviceRegistry::InputDeviceRegistry(
    std::vector<BackendFactory> backendFactories)
    : queueState_(std::make_shared<QueueState>()) {
  const auto keyboard = keyboardSnapshot();
  devices_.emplace(keyboard.stableId, keyboard);
  enqueueDevice(keyboard);

  backends_.reserve(backendFactories.size());
  backendSinkGates_.reserve(backendFactories.size());
  for (auto &factory : backendFactories) {
    auto sinkGate = std::make_shared<BackendSinkGate>();
    sinkGate->destination = queueState_;
    input::InputBackendSink sink{
        .enqueueInput =
            [sinkGate](input::PhysicalInputEvent event) {
              sinkGate->enqueue(QueuedPayload(std::move(event)));
            },
        .enqueueDevice =
            [sinkGate](input::InputDeviceSnapshot device) {
              sinkGate->enqueue(QueuedPayload(std::move(device)));
            },
        .enqueueInterruption =
            [sinkGate](input::InputInterruption interruption) {
              sinkGate->enqueue(QueuedPayload(interruption));
            }};
    auto backend = factory(sink);
    if (!backend) {
      sinkGate->close();
      const std::string message = "Input backend factory returned null";
      diagnostics_.push_back(message);
      SDL_LogError(SDL_LOG_CATEGORY_INPUT, "%s", message.c_str());
      continue;
    }

    auto *sdlBackend = dynamic_cast<SDLInputBackend *>(backend.get());

    std::string errorMessage;
    if (!backend->start(errorMessage)) {
      if (errorMessage.empty()) {
        errorMessage = "unknown error";
      }
      const std::string message = "Input backend start failed: " + errorMessage;
      diagnostics_.push_back(message);
      SDL_LogError(SDL_LOG_CATEGORY_INPUT, "%s", message.c_str());
      sinkGate->close();
      backend->stop();
      continue;
    }
    if (sdlBackend != nullptr) {
      sdlInputBackend_ = sdlBackend;
    }
    sinkGate->activate();
    backends_.push_back(std::move(backend));
    backendSinkGates_.push_back(std::move(sinkGate));
  }
}

InputDeviceRegistry::~InputDeviceRegistry() {
  for (std::size_t index = backends_.size(); index > 0; --index) {
    backendSinkGates_[index - 1]->close();
    backends_[index - 1]->stop();
  }
  queueState_->close();
}

void InputDeviceRegistry::clearSdlInputState() {
  if (sdlInputBackend_) sdlInputBackend_->clearInputState();
  const std::lock_guard lock(legacyInputMutex_);
  pressedSdlScancodes_.reset();
  pressedGdxKeys_.reset();
  pressedGdxKeyCounts_.fill(0);
  pressedMouseButtons_.reset();
  legacyTouches_.fill(std::nullopt);
  pendingLegacyEdges_ = {};
  currentLegacyEdges_ = {};
}

void InputDeviceRegistry::reconcileSdlDevices() {
  if (sdlInputBackend_) sdlInputBackend_->reconcileDevices();
}

std::optional<InputDeviceRegistry::PointerPosition>
InputDeviceRegistry::pointerPosition() const noexcept {
  const std::lock_guard lock(legacyInputMutex_);
  return pointerPosition_;
}

void InputDeviceRegistry::handleSdlEvent(const SDL_Event &event) {
  std::optional<PointerPosition> pointer;
  if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.which != SDL_TOUCH_MOUSEID) {
    pointer = PointerPosition{static_cast<float>(event.motion.x),
                              static_cast<float>(event.motion.y), false};
  } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
             event.button.which != SDL_TOUCH_MOUSEID) {
    pointer = PointerPosition{static_cast<float>(event.button.x),
                              static_cast<float>(event.button.y), false};
  } else if ((event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION) &&
             event.tfinger.touchID != SDL_MOUSE_TOUCHID &&
             std::isfinite(event.tfinger.x) && std::isfinite(event.tfinger.y)) {
    pointer = PointerPosition{event.tfinger.x, event.tfinger.y, true};
  }
  if (pointer) {
    const std::lock_guard lock(legacyInputMutex_);
    pointerPosition_ = pointer;
  }
  {
    const std::lock_guard lock(legacyInputMutex_);
    const bool mouseMotion = event.type == SDL_EVENT_MOUSE_MOTION && event.motion.which != SDL_TOUCH_MOUSEID;
    const bool mouseButton = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                             event.button.which != SDL_TOUCH_MOUSEID;
    if (mouseMotion || mouseButton) {
      const float x = mouseMotion ? event.motion.x : event.button.x;
      const float y = mouseMotion ? event.motion.y : event.button.y;
      if (std::isfinite(x) && std::isfinite(y)) legacyPointerPosition_ = PointerPosition{x, y, false};
      if (mouseMotion) {
        if (std::isfinite(event.motion.xrel)) pendingLegacyEdges_.mouseDeltaX += event.motion.xrel;
        if (std::isfinite(event.motion.yrel)) pendingLegacyEdges_.mouseDeltaY += event.motion.yrel;
      } else {
        int button = -1;
        switch (event.button.button) {
        case SDL_BUTTON_LEFT: button = 0; break;
        case SDL_BUTTON_RIGHT: button = 1; break;
        case SDL_BUTTON_MIDDLE: button = 2; break;
        case SDL_BUTTON_X1: button = 3; break;
        case SDL_BUTTON_X2: button = 4; break;
        default: break;
        }
        if (button >= 0) {
          const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
          if (down && !pressedMouseButtons_.test(button)) pendingLegacyEdges_.justTouched = true;
          pressedMouseButtons_.set(button, down);
        }
      }
    }
    const bool touchDown = event.type == SDL_EVENT_FINGER_DOWN;
    const bool touchMotion = event.type == SDL_EVENT_FINGER_MOTION;
    const bool touchUp = event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED;
    if ((touchDown || touchMotion || touchUp) && event.tfinger.touchID != SDL_MOUSE_TOUCHID) {
      const auto &touch = event.tfinger;
      auto found = std::ranges::find_if(legacyTouches_, [&](const auto &slot) {
        return slot && slot->device == touch.touchID && slot->finger == touch.fingerID;
      });
      const bool finite = std::isfinite(touch.x) && std::isfinite(touch.y);
      if (touchDown && finite && found == legacyTouches_.end()) {
        found = std::ranges::find_if(legacyTouches_, [](const auto &slot) { return !slot; });
        if (found != legacyTouches_.end()) {
          *found = LegacyTouch{touch.touchID, touch.fingerID};
          pendingLegacyEdges_.justTouched = true;
        }
      }
      if (found != legacyTouches_.end()) {
        if (found == legacyTouches_.begin() && finite) {
          // Derive touch delta from coordinates: native iOS/Android bridge events
          // intentionally carry no SDL dx/dy values.
          if (touchMotion && legacyPointerPosition_ && legacyPointerPosition_->normalized) {
            pendingLegacyEdges_.touchDeltaX += touch.x - legacyPointerPosition_->x;
            pendingLegacyEdges_.touchDeltaY += touch.y - legacyPointerPosition_->y;
          }
          legacyPointerPosition_ = PointerPosition{touch.x, touch.y, true};
        }
        if (touchUp) found->reset();
      }
      if (event.type == SDL_EVENT_FINGER_CANCELED) {
        pendingLegacyEdges_ = {};
        currentLegacyEdges_ = {};
      }
    }
  }
  for (const auto &backend : backends_) {
    backend->handleSdlEvent(event);
  }
  if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
    clearSdlInputState();
    return;
  }
  if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
    const int scancode = static_cast<int>(event.key.scancode);
    if (scancode >= 0 && scancode < SDL_SCANCODE_COUNT) {
      const bool pressed = event.type == SDL_EVENT_KEY_DOWN;
      const std::lock_guard lock(legacyInputMutex_);
      const bool wasPressed =
          pressedSdlScancodes_.test(static_cast<std::size_t>(scancode));
      if (wasPressed == pressed) {
        return;
      }
      pressedSdlScancodes_.set(static_cast<std::size_t>(scancode), pressed);
      const int gdxKeyCode = gdxKeyCodeForSdlScancode(
          static_cast<SDL_Scancode>(scancode));
      if (gdxKeyCode >= 0 &&
          static_cast<std::size_t>(gdxKeyCode) < pressedGdxKeys_.size()) {
        auto &count = pressedGdxKeyCounts_[static_cast<std::size_t>(gdxKeyCode)];
        if (pressed) {
          if (!event.key.repeat) pendingLegacyEdges_.keys.set(static_cast<std::size_t>(gdxKeyCode));
          ++count;
        } else if (count != 0) {
          --count;
        }
        pressedGdxKeys_.set(static_cast<std::size_t>(gdxKeyCode), count != 0);
      }
    }
  }
}

void InputDeviceRegistry::handleSdlEventAndDispatch(const SDL_Event &event) {
  handleSdlEvent(event);
  dispatchPending();
}

void InputDeviceRegistry::pump() { pumpInternal(true); }

void InputDeviceRegistry::completeRealtimeInputFallback() {
  if (sdlInputBackend_ != nullptr) sdlInputBackend_->pump();
}

void InputDeviceRegistry::configureGyroscopeTurntable(
    input::GyroscopeTurntableConfig config) {
  for (const auto &backend : backends_) {
    backend->configureGyroscopeTurntable(config);
  }
  dispatchPending();
}

void InputDeviceRegistry::resetGyroscopeTurntableSession() {
  for (const auto &backend : backends_) {
    backend->resetGyroscopeTurntableSession();
  }
  dispatchPending();
}

void InputDeviceRegistry::setGyroscopeBindingsPresent(bool present) {
  gyroscopeBindingsPresent_ = present;
  updateGyroscopeDemand();
  dispatchPending();
}

void InputDeviceRegistry::updateGyroscopeDemand() {
  const bool enabled = gyroscopeBindingsPresent_ ||
      std::ranges::any_of(inputListeners_, [](const auto &entry) {
        return entry.second.monitorGyroscope;
      });
  if (enabled == gyroscopeEnabled_) return;
  gyroscopeEnabled_ = enabled;
  for (const auto &backend : backends_) {
    backend->setGyroscopeEnabled(enabled);
  }
}

void InputDeviceRegistry::setRealtimeInputClaimed(
    input::DeviceClass deviceClass, bool claimed) {
  if (claimed && deviceClass == input::DeviceClass::Keyboard) {
    // A source can fail in menus before the next backend pump. Finish that
    // unclaimed handoff before newly admitted gameplay can receive input.
    completeRealtimeInputFallback();
  }
  if (claimed) {
    queueState_->setRealtimeClaimed(deviceClass, true);
  }
  for (const auto &backend : backends_) {
    backend->setRealtimeInputClaimed(deviceClass, claimed);
  }
  if (!claimed) {
    queueState_->setRealtimeClaimed(deviceClass, false);
  }
}

std::optional<input::PhysicalInputEvent>
InputDeviceRegistry::translateRealtimeSdlInput(const SDL_Event &event) const {
  if (sdlInputBackend_ == nullptr) {
    return std::nullopt;
  }
  return sdlInputBackend_->translateRealtimeInput(event);
}

std::size_t InputDeviceRegistry::translateRealtimeSdlInputs(
    const SDL_Event &event, std::span<input::PhysicalInputEvent> output,
    bool consumeOnce) {
  if (sdlInputBackend_ == nullptr) {
    return 0;
  }
  return sdlInputBackend_->translateRealtimeInputs(event, output, consumeOnce);
}

void InputDeviceRegistry::discardRealtimeSdlInput(const SDL_Event &event) {
  if (sdlInputBackend_) sdlInputBackend_->discardRealtimeInput(event);
}

std::optional<std::string> InputDeviceRegistry::realtimeDisconnectedSdlDevice(
    const SDL_Event &event) const {
  if (sdlInputBackend_ == nullptr) {
    return std::nullopt;
  }
  return sdlInputBackend_->realtimeDisconnectedDeviceId(event);
}

void InputDeviceRegistry::dispatchPending() { pumpInternal(false); }

void InputDeviceRegistry::pumpInternal(bool pumpBackends) {
  if (pumping_) {
    repumpRequested_ = true;
    repumpBackendsRequested_ = repumpBackendsRequested_ || pumpBackends;
    return;
  }
  pumping_ = true;
  bool shouldPumpBackends = pumpBackends;

  try {
    do {
      repumpRequested_ = false;
      if (shouldPumpBackends) {
        for (const auto &backend : backends_) {
          backend->pump();
        }
      }
      shouldPumpBackends = false;

      std::deque<QueuedEvent> pending = queueState_->take();

      for (const auto &event : pending) {
        if (const auto *inputEvent =
                std::get_if<input::PhysicalInputEvent>(&event.payload)) {
          std::vector<std::pair<std::uint64_t, InputSubscription>> listeners(
              inputListeners_.begin(), inputListeners_.end());
          for (const auto &[token, subscription] : listeners) {
            if (!inputListeners_.contains(token) ||
                event.sequence < subscription.firstEventSequence) {
              continue;
            }
            subscription.listener(*inputEvent);
          }
          continue;
        }

        const auto &device =
            std::get<input::InputDeviceSnapshot>(event.payload);
        devices_.insert_or_assign(device.stableId, device);
        std::vector<std::pair<std::uint64_t, DeviceListener>> listeners(
            deviceListeners_.begin(), deviceListeners_.end());
        for (const auto &[token, listener] : listeners) {
          if (!deviceListeners_.contains(token)) {
            continue;
          }
          listener(device);
        }
      }
      if (repumpBackendsRequested_) {
        shouldPumpBackends = true;
        repumpBackendsRequested_ = false;
      }
    } while (repumpRequested_);
  } catch (...) {
    pumping_ = false;
    repumpRequested_ = false;
    repumpBackendsRequested_ = false;
    throw;
  }
  pumping_ = false;
}

std::uint64_t InputDeviceRegistry::subscribeInput(InputListener listener,
                                                  bool monitorGyroscope) {
  const std::uint64_t token = nextSubscriptionToken_++;
  queueState_->atSequenceBoundary([&](std::uint64_t firstEventSequence) {
    inputListeners_.emplace(
        token, InputSubscription{.firstEventSequence = firstEventSequence,
                                 .listener = std::move(listener),
                                 .monitorGyroscope = monitorGyroscope});
  });
  updateGyroscopeDemand();
  return token;
}

std::uint64_t
InputDeviceRegistry::subscribeRealtimeInput(InputListener listener) {
  const std::uint64_t token = nextSubscriptionToken_++;
  queueState_->subscribeRealtime(token, std::move(listener));
  return token;
}

std::uint64_t
InputDeviceRegistry::subscribeRealtimeDevices(DeviceListener listener) {
  const std::uint64_t token = nextSubscriptionToken_++;
  queueState_->subscribeRealtimeDevices(token, std::move(listener));
  return token;
}

std::uint64_t InputDeviceRegistry::subscribeRealtimeInterruptions(
    InterruptionListener listener) {
  const std::uint64_t token = nextSubscriptionToken_++;
  queueState_->subscribeRealtimeInterruptions(token, std::move(listener));
  return token;
}

std::uint64_t InputDeviceRegistry::subscribeDevices(DeviceListener listener) {
  const std::uint64_t token = nextSubscriptionToken_++;
  deviceListeners_.emplace(token, std::move(listener));
  return token;
}

void InputDeviceRegistry::unsubscribe(std::uint64_t token) {
  queueState_->unsubscribeRealtime(token);
  inputListeners_.erase(token);
  deviceListeners_.erase(token);
  updateGyroscopeDemand();
}

std::vector<input::InputDeviceSnapshot> InputDeviceRegistry::snapshot() const {
  std::vector<input::InputDeviceSnapshot> result;
  result.reserve(devices_.size());
  for (const auto &[stableId, device] : devices_) {
    (void)stableId;
    result.push_back(device);
  }
  std::ranges::sort(result, {}, &input::InputDeviceSnapshot::stableId);
  return result;
}

input::LegacyInputGeneration InputDeviceRegistry::legacyInputGeneration(
    int drawableWidth, int drawableHeight) const noexcept {
  input::LegacyInputGeneration result =
      sdlInputBackend_ == nullptr
          ? input::LegacyInputGeneration{}
          : sdlInputBackend_->legacyControllerGeneration();
  const std::lock_guard lock(legacyInputMutex_);
  result.sequence = ++legacyInputSequence_;
  result.drawableWidth = std::max(0, drawableWidth);
  result.drawableHeight = std::max(0, drawableHeight);
  result.anyKeyPressed = pressedSdlScancodes_.any();
  result.pressedGdxKeys = pressedGdxKeys_;
  result.justPressedGdxKeys = currentLegacyEdges_.keys;
  result.justTouched = currentLegacyEdges_.justTouched;
  result.pressedMouseButtons = pressedMouseButtons_;
  if (std::ranges::any_of(legacyTouches_, [](const auto &slot) { return slot.has_value(); })) {
    result.pressedMouseButtons.set(0);
  }
  // Graphics dimensions already use drawable pixels; keep pointer coordinates
  // in that same top-left screen space, without the scene's Y-axis inversion.
  const double scaleX = legacyLogicalWidth_ > 0 ? double(result.drawableWidth) / legacyLogicalWidth_ : 0.0;
  const double scaleY = legacyLogicalHeight_ > 0 ? double(result.drawableHeight) / legacyLogicalHeight_ : 0.0;
  const auto screenInteger = [](double value) {
    if (!std::isfinite(value)) return 0;
    return static_cast<int>(std::clamp(value, double(std::numeric_limits<int>::min()),
                                     double(std::numeric_limits<int>::max())));
  };
  if (legacyPointerPosition_) {
    result.pointerX = screenInteger(legacyPointerPosition_->x *
        (legacyPointerPosition_->normalized ? result.drawableWidth : scaleX));
    result.pointerY = screenInteger(legacyPointerPosition_->y *
        (legacyPointerPosition_->normalized ? result.drawableHeight : scaleY));
  }
  result.deltaX = screenInteger(currentLegacyEdges_.mouseDeltaX * scaleX +
                               currentLegacyEdges_.touchDeltaX * result.drawableWidth);
  result.deltaY = screenInteger(currentLegacyEdges_.mouseDeltaY * scaleY +
                               currentLegacyEdges_.touchDeltaY * result.drawableHeight);
  return result;
}

bool InputDeviceRegistry::isConnected(std::string_view stableId) const {
  const auto device = devices_.find(std::string(stableId));
  return device != devices_.end() && device->second.connected;
}

const std::vector<std::string> &InputDeviceRegistry::diagnostics() const {
  return diagnostics_;
}

void InputDeviceRegistry::enqueueInput(input::PhysicalInputEvent event) {
  queueState_->enqueue(QueuedPayload(std::move(event)));
}

void InputDeviceRegistry::enqueueDevice(input::InputDeviceSnapshot device) {
  queueState_->enqueue(QueuedPayload(std::move(device)));
}

void InputDeviceRegistry::advanceLegacyInputFrame(int logicalWidth, int logicalHeight) noexcept {
  const std::lock_guard lock(legacyInputMutex_);
  currentLegacyEdges_ = std::exchange(pendingLegacyEdges_, {});
  legacyLogicalWidth_ = std::max(0, logicalWidth);
  legacyLogicalHeight_ = std::max(0, logicalHeight);
}

bool InputDeviceRegistry::hasCurrentLegacyPressEdge() const noexcept {
  const std::lock_guard lock(legacyInputMutex_);
  return currentLegacyEdges_.justTouched || currentLegacyEdges_.keys.test(66) ||
         currentLegacyEdges_.keys.test(62);
}
