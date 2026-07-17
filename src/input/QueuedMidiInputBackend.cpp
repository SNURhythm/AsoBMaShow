#include "QueuedMidiInputBackend.h"

#include <utility>

QueuedMidiInputBackend::DeviceActivation::DeviceActivation(
    QueuedMidiInputBackend &backend, input::InputDeviceSnapshot device)
    : backend_(&backend), device_(std::move(device)) {
  device_.connected = true;
  backend_->enqueueDevice(device_);
}

QueuedMidiInputBackend::DeviceActivation::DeviceActivation(
    DeviceActivation &&other) noexcept
    : backend_(std::exchange(other.backend_, nullptr)),
      device_(std::move(other.device_)) {}

QueuedMidiInputBackend::DeviceActivation &
QueuedMidiInputBackend::DeviceActivation::operator=(
    DeviceActivation &&other) noexcept {
  if (this != &other) {
    rollback();
    backend_ = std::exchange(other.backend_, nullptr);
    device_ = std::move(other.device_);
  }
  return *this;
}

QueuedMidiInputBackend::DeviceActivation::~DeviceActivation() { rollback(); }

void QueuedMidiInputBackend::DeviceActivation::commit() noexcept {
  backend_ = nullptr;
}

void QueuedMidiInputBackend::DeviceActivation::rollback() {
  if (backend_ == nullptr) {
    return;
  }
  backend_->enqueueDeviceDisconnect(std::move(device_));
  backend_ = nullptr;
}

QueuedMidiInputBackend::DeviceActivation
QueuedMidiInputBackend::beginDeviceActivation(
    input::InputDeviceSnapshot connectedDevice) {
  return DeviceActivation(*this, std::move(connectedDevice));
}

void QueuedMidiInputBackend::pump() {
  std::deque<QueuedEvent> pending;
  std::set<std::string> overflowedDevices;
  {
    const std::lock_guard lock(queueMutex_);
    pending.swap(queuedEvents_);
    overflowedDevices.swap(overflowedDevices_);
    queuedPacketBytes_ = 0;
  }

  for (auto &queued : pending) {
    if (auto *device = std::get_if<input::InputDeviceSnapshot>(&queued)) {
      if (device->stableId.empty()) {
        continue;
      }
      if (device->connected) {
        connectedDevices_.insert_or_assign(device->stableId, *device);
      } else {
        connectedDevices_.erase(device->stableId);
        parsers_.erase(device->stableId);
      }
      publishDevice(std::move(*device));
      continue;
    }

    auto &packet = std::get<Packet>(queued);
    if (overflowedDevices.contains(packet.stableId) ||
        !connectedDevices_.contains(packet.stableId)) {
      continue;
    }
    auto events = parsers_[packet.stableId].consume(
        packet.stableId, packet.bytes, packet.timestampMicros);
    for (auto &event : events) {
      publishInput(std::move(event));
    }
  }

  for (const auto &stableId : overflowedDevices) {
    const auto connected = connectedDevices_.find(stableId);
    if (connected == connectedDevices_.end()) {
      parsers_.erase(stableId);
      continue;
    }
    parsers_.erase(stableId);
    auto disconnected = connected->second;
    disconnected.connected = false;
    publishDevice(std::move(disconnected));
    publishDevice(connected->second);
  }
}

void QueuedMidiInputBackend::openQueue() {
  const std::lock_guard lock(queueMutex_);
  accepting_ = true;
  queuedEvents_.clear();
  overflowedDevices_.clear();
  queuedPacketBytes_ = 0;
  connectedDevices_.clear();
  parsers_.clear();
  immediateParsers_.clear();
}

void QueuedMidiInputBackend::closeQueue() {
  const std::lock_guard lock(queueMutex_);
  accepting_ = false;
  queuedEvents_.clear();
  overflowedDevices_.clear();
  queuedPacketBytes_ = 0;
  connectedDevices_.clear();
  parsers_.clear();
  immediateParsers_.clear();
}

void QueuedMidiInputBackend::enqueuePacket(std::string stableId,
                                           std::vector<std::uint8_t> bytes,
                                           std::uint64_t timestampMicros) {
  if (stableId.empty() || bytes.empty()) {
    return;
  }
  const std::lock_guard lock(queueMutex_);
  if (!accepting_ || overflowedDevices_.contains(stableId)) {
    return;
  }
  if (bytes.size() > kMaximumPacketBytes ||
      bytes.size() > kMaximumQueuedPacketBytes - queuedPacketBytes_) {
    overflowedDevices_.insert(std::move(stableId));
    return;
  }
  queuedPacketBytes_ += bytes.size();
  queuedEvents_.emplace_back(Packet{.stableId = std::move(stableId),
                                    .bytes = std::move(bytes),
                                    .timestampMicros = timestampMicros});
}

void QueuedMidiInputBackend::publishPacketImmediately(
    std::string_view stableId, std::span<const std::uint8_t> bytes,
    std::uint64_t timestampMicros) {
  if (stableId.empty() || bytes.empty() || bytes.size() > kMaximumPacketBytes) {
    return;
  }
  const std::lock_guard lock(queueMutex_);
  if (!accepting_) {
    return;
  }
  auto events = immediateParsers_[std::string(stableId)].consume(
      stableId, bytes, timestampMicros);
  for (auto &event : events) {
    publishInput(std::move(event));
  }
}

void QueuedMidiInputBackend::resetImmediateParser(std::string_view stableId) {
  const std::lock_guard lock(queueMutex_);
  immediateParsers_.erase(std::string(stableId));
}

void QueuedMidiInputBackend::enqueueDevice(input::InputDeviceSnapshot device) {
  if (device.stableId.empty()) {
    return;
  }
  const std::lock_guard lock(queueMutex_);
  if (accepting_) {
    queuedEvents_.emplace_back(std::move(device));
  }
}

void QueuedMidiInputBackend::enqueueDeviceDisconnect(
    input::InputDeviceSnapshot device) {
  device.connected = false;
  enqueueDevice(std::move(device));
}
