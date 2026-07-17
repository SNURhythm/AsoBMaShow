#include "InputDeviceRegistry.h"

#include "MidiInputBackendFactory.h"
#include "GyroscopeInputBackendFactory.h"
#include "SDLInputBackend.h"
#if defined(_WIN32)
#include "RealtimeControllerDeviceMap.h"
#include "WindowsRealtimeInputBackend.h"
#endif

#include <SDL2/SDL_log.h>

#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
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
#if defined(_WIN32)
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

  void enqueue(QueuedPayload payload) {
    const std::lock_guard lock(mutex);
    if (!accepting) {
      return;
    }
    const std::uint64_t sequence = nextSequence++;
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
  }

  std::recursive_mutex mutex;
  std::deque<QueuedEvent> queue;
  std::map<std::uint64_t, std::shared_ptr<RealtimeInputSubscriptionState>>
      realtimeInputListeners;
  std::map<std::uint64_t, std::shared_ptr<RealtimeDeviceSubscriptionState>>
      realtimeDeviceListeners;
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

void InputDeviceRegistry::handleSdlEvent(const SDL_Event &event) {
  for (const auto &backend : backends_) {
    backend->handleSdlEvent(event);
  }
}

void InputDeviceRegistry::handleSdlEventAndDispatch(const SDL_Event &event) {
  handleSdlEvent(event);
  dispatchPending();
}

void InputDeviceRegistry::pump() { pumpInternal(true); }

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

void InputDeviceRegistry::setRealtimeInputClaimed(
    input::DeviceClass deviceClass, bool claimed) {
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

std::uint64_t InputDeviceRegistry::subscribeInput(InputListener listener) {
  const std::uint64_t token = nextSubscriptionToken_++;
  queueState_->atSequenceBoundary([&](std::uint64_t firstEventSequence) {
    inputListeners_.emplace(
        token, InputSubscription{.firstEventSequence = firstEventSequence,
                                 .listener = std::move(listener)});
  });
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

std::uint64_t InputDeviceRegistry::subscribeDevices(DeviceListener listener) {
  const std::uint64_t token = nextSubscriptionToken_++;
  deviceListeners_.emplace(token, std::move(listener));
  return token;
}

void InputDeviceRegistry::unsubscribe(std::uint64_t token) {
  queueState_->unsubscribeRealtime(token);
  inputListeners_.erase(token);
  deviceListeners_.erase(token);
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
