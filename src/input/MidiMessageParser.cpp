#include "MidiMessageParser.h"

#include <utility>

namespace {

constexpr std::uint8_t kStatusMask = 0x80;
constexpr std::uint8_t kMessageMask = 0xF0;
constexpr std::uint8_t kChannelMask = 0x0F;
constexpr std::uint8_t kNoteOff = 0x80;
constexpr std::uint8_t kNoteOn = 0x90;
constexpr std::uint8_t kControlChange = 0xB0;
constexpr std::uint8_t kSystemExclusive = 0xF0;
constexpr std::uint8_t kSystemExclusiveEnd = 0xF7;
constexpr std::uint8_t kFirstRealtimeStatus = 0xF8;

bool isStatus(std::uint8_t byte) { return (byte & kStatusMask) != 0; }

bool isChannelStatus(std::uint8_t status) {
  return status >= 0x80 && status < kSystemExclusive;
}

bool isRealtimeStatus(std::uint8_t status) {
  return status >= kFirstRealtimeStatus;
}

input::PhysicalInputEvent midiEvent(std::string_view deviceId,
                                    input::ControlKind kind, int index,
                                    double rawValue, float normalizedValue,
                                    std::uint64_t timestampMicros) {
  return {.control = {.deviceId = std::string(deviceId),
                      .deviceClass = input::DeviceClass::Midi,
                      .kind = kind,
                      .index = index,
                      .direction = input::ControlDirection::Any},
          .rawValue = rawValue,
          .normalizedValue = normalizedValue,
          .timestampMicros = timestampMicros};
}

} // namespace

std::vector<input::PhysicalInputEvent>
MidiMessageParser::consume(std::string_view deviceId,
                           std::span<const std::uint8_t> bytes,
                           std::uint64_t timestampMicros) {
  if (hasActiveDevice_ && activeDeviceId_ != deviceId) {
    reset();
  }
  hasActiveDevice_ = true;
  activeDeviceId_ = deviceId;

  std::vector<input::PhysicalInputEvent> events;
  for (const std::uint8_t byte : bytes) {
    if (isStatus(byte)) {
      if (isRealtimeStatus(byte)) {
        continue;
      }

      if (inSystemExclusive_) {
        if (byte == kSystemExclusive) {
          runningStatus_.reset();
          clearPending();
          continue;
        }
        inSystemExclusive_ = false;
      }

      if (byte == kSystemExclusive) {
        runningStatus_.reset();
        clearPending();
        inSystemExclusive_ = true;
        continue;
      }
      if (byte == kSystemExclusiveEnd) {
        runningStatus_.reset();
        clearPending();
        continue;
      }

      beginStatus(byte);
      continue;
    }

    if (inSystemExclusive_) {
      continue;
    }
    if (!pendingStatus_.has_value()) {
      if (!runningStatus_.has_value()) {
        continue;
      }
      beginStatus(*runningStatus_);
    }
    if (expectedDataCount_ == 0 || pendingDataCount_ >= pendingData_.size()) {
      clearPending();
      continue;
    }

    pendingData_[pendingDataCount_++] = byte;
    if (pendingDataCount_ != expectedDataCount_) {
      continue;
    }
    if (auto event = finishPending(deviceId, timestampMicros)) {
      events.push_back(std::move(*event));
    }
  }
  return events;
}

void MidiMessageParser::reset() {
  runningStatus_.reset();
  clearPending();
  inSystemExclusive_ = false;
  hasActiveDevice_ = false;
  activeDeviceId_.clear();
}

std::size_t MidiMessageParser::dataLength(std::uint8_t status) {
  if (isChannelStatus(status)) {
    switch (status & kMessageMask) {
    case 0xC0:
    case 0xD0:
      return 1;
    case 0x80:
    case 0x90:
    case 0xA0:
    case 0xB0:
    case 0xE0:
      return 2;
    default:
      return 0;
    }
  }

  switch (status) {
  case 0xF1:
  case 0xF3:
    return 1;
  case 0xF2:
    return 2;
  default:
    return 0;
  }
}

void MidiMessageParser::beginStatus(std::uint8_t status) {
  clearPending();
  if (isChannelStatus(status)) {
    runningStatus_ = status;
  } else {
    runningStatus_.reset();
  }

  expectedDataCount_ = dataLength(status);
  if (expectedDataCount_ != 0) {
    pendingStatus_ = status;
  }
}

void MidiMessageParser::clearPending() {
  pendingStatus_.reset();
  pendingDataCount_ = 0;
  expectedDataCount_ = 0;
}

std::optional<input::PhysicalInputEvent>
MidiMessageParser::finishPending(std::string_view deviceId,
                                 std::uint64_t timestampMicros) {
  if (!pendingStatus_.has_value()) {
    return std::nullopt;
  }

  const std::uint8_t status = *pendingStatus_;
  const std::uint8_t message = status & kMessageMask;
  const int channel = status & kChannelMask;
  const int number = pendingData_[0];
  const int value = expectedDataCount_ > 1 ? pendingData_[1] : 0;

  std::optional<input::PhysicalInputEvent> event;
  if (message == kNoteOff) {
    event = midiEvent(deviceId, input::ControlKind::MidiNote,
                      channel * 128 + number, 0.0, 0.0F, timestampMicros);
  } else if (message == kNoteOn) {
    const bool pressed = value != 0;
    event = midiEvent(deviceId, input::ControlKind::MidiNote,
                      channel * 128 + number, pressed ? value : 0.0,
                      pressed ? static_cast<float>(value) / 127.0F : 0.0F,
                      timestampMicros);
  } else if (message == kControlChange) {
    event = midiEvent(deviceId, input::ControlKind::MidiControl,
                      channel * 128 + number, value,
                      static_cast<float>(value) / 127.0F, timestampMicros);
  }

  clearPending();
  if (runningStatus_.has_value() && *runningStatus_ == status) {
    beginStatus(status);
  }
  return event;
}
