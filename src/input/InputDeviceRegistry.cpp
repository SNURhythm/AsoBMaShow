#include "InputDeviceRegistry.h"

#include "MidiInputBackendFactory.h"
#include "GyroscopeInputBackendFactory.h"
#include "SDLInputBackend.h"

#include <SDL2/SDL_log.h>

#include <algorithm>
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

} // namespace

struct InputDeviceRegistry::QueueState {
  void enqueue(QueuedPayload payload) {
    const std::lock_guard lock(mutex);
    if (accepting) {
      queue.push_back(
          {.sequence = nextSequence++, .payload = std::move(payload)});
    }
  }

  void enqueue(std::deque<QueuedPayload> payloads) {
    const std::lock_guard lock(mutex);
    if (!accepting) {
      return;
    }
    for (auto &payload : payloads) {
      queue.push_back(
          {.sequence = nextSequence++, .payload = std::move(payload)});
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
  }

  std::mutex mutex;
  std::deque<QueuedEvent> queue;
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
    : InputDeviceRegistry({[](input::InputBackendSink sink) {
                             return std::make_unique<SDLInputBackend>(
                                 std::move(sink));
                           },
                           [](input::InputBackendSink sink) {
                             return makeMidiInputBackend(std::move(sink));
                           },
                           [](input::InputBackendSink sink) {
                             return makeGyroscopeInputBackend(std::move(sink));
                           }}) {}

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

std::optional<input::PhysicalInputEvent>
InputDeviceRegistry::translateRealtimeSdlInput(const SDL_Event &event) const {
  if (sdlInputBackend_ == nullptr) {
    return std::nullopt;
  }
  return sdlInputBackend_->translateRealtimeInput(event);
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

std::uint64_t InputDeviceRegistry::subscribeDevices(DeviceListener listener) {
  const std::uint64_t token = nextSubscriptionToken_++;
  deviceListeners_.emplace(token, std::move(listener));
  return token;
}

void InputDeviceRegistry::unsubscribe(std::uint64_t token) {
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
