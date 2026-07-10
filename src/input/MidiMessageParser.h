#pragma once

#include "InputTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class MidiMessageParser {
public:
  std::vector<input::PhysicalInputEvent>
  consume(std::string_view deviceId, std::span<const std::uint8_t> bytes,
          std::uint64_t timestampMicros);
  void reset();

private:
  static std::size_t dataLength(std::uint8_t status);
  void beginStatus(std::uint8_t status);
  void clearPending();
  std::optional<input::PhysicalInputEvent>
  finishPending(std::string_view deviceId, std::uint64_t timestampMicros);

  std::optional<std::uint8_t> runningStatus_;
  std::optional<std::uint8_t> pendingStatus_;
  std::array<std::uint8_t, 2> pendingData_{};
  std::size_t pendingDataCount_ = 0;
  std::size_t expectedDataCount_ = 0;
  bool inSystemExclusive_ = false;
  bool hasActiveDevice_ = false;
  std::string activeDeviceId_;
};
