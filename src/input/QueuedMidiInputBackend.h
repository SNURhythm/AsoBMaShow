#pragma once

#include "IInputBackend.h"
#include "MidiMessageParser.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

class QueuedMidiInputBackend : public IInputBackend {
public:
  void pump() override;

protected:
  explicit QueuedMidiInputBackend(input::InputBackendSink sink)
      : IInputBackend(std::move(sink)) {}

  void openQueue();
  void closeQueue();
  void enqueuePacket(std::string stableId, std::vector<std::uint8_t> bytes,
                     std::uint64_t timestampMicros);
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
};
