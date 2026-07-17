#pragma once

#include "IInputBackend.h"
#include "MidiMessageParser.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

class QueuedMidiInputBackend : public IInputBackend {
public:
  void pump() override;

protected:
  class DeviceActivation {
  public:
    DeviceActivation(const DeviceActivation &) = delete;
    DeviceActivation &operator=(const DeviceActivation &) = delete;
    DeviceActivation(DeviceActivation &&other) noexcept;
    DeviceActivation &operator=(DeviceActivation &&other) noexcept;
    ~DeviceActivation();

    void commit() noexcept;

  private:
    friend class QueuedMidiInputBackend;
    DeviceActivation(QueuedMidiInputBackend &backend,
                     input::InputDeviceSnapshot device);
    void rollback();

    QueuedMidiInputBackend *backend_ = nullptr;
    input::InputDeviceSnapshot device_;
  };

  explicit QueuedMidiInputBackend(input::InputBackendSink sink)
      : IInputBackend(std::move(sink)) {}

  DeviceActivation
  beginDeviceActivation(input::InputDeviceSnapshot connectedDevice);
  void openQueue();
  void closeQueue();
  void enqueuePacket(std::string stableId, std::vector<std::uint8_t> bytes,
                     std::uint64_t timestampMicros);
  void publishPacketImmediately(std::string_view stableId,
                                std::span<const std::uint8_t> bytes,
                                std::uint64_t timestampMicros);
  void resetImmediateParser(std::string_view stableId);
  void enqueueDevice(input::InputDeviceSnapshot device);

private:
  static constexpr std::size_t kMaximumPacketBytes = 64 * 1024;
  static constexpr std::size_t kMaximumQueuedPacketBytes = 1024 * 1024;

  struct Packet {
    std::string stableId;
    std::vector<std::uint8_t> bytes;
    std::uint64_t timestampMicros = 0;
  };
  using QueuedEvent = std::variant<Packet, input::InputDeviceSnapshot>;

  std::mutex queueMutex_;
  std::deque<QueuedEvent> queuedEvents_;
  std::set<std::string> overflowedDevices_;
  std::size_t queuedPacketBytes_ = 0;
  bool accepting_ = false;

  std::unordered_map<std::string, input::InputDeviceSnapshot> connectedDevices_;
  std::unordered_map<std::string, MidiMessageParser> parsers_;
  std::unordered_map<std::string, MidiMessageParser> immediateParsers_;
};
