#include "MidiPlatformBackends.h"

#if defined(_WIN32)

#include "NativeCallbackLifetime.h"
#include "QueuedMidiInputBackend.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class WinMidiInputBackend;

struct WinMidiConnection {
  explicit WinMidiConnection(WinMidiInputBackend *connectionBackend)
      : backend(connectionBackend), callbackLifetime(this) {}

  WinMidiInputBackend *backend = nullptr;
  NativeCallbackLifetime callbackLifetime;
  HMIDIIN handle = nullptr;
  UINT deviceId = 0;
  std::string stableId;
  std::string displayName;
  std::uint64_t startedAtMicros = 0;
  std::atomic_bool connected = true;
};

struct WinMidiDeviceDescriptor {
  UINT deviceId = 0;
  std::string stableId;
  std::string displayName;
};

std::uint64_t monotonicMicros() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string utf8FromWide(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  const int length = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(length), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), length,
                          nullptr, nullptr) <= 0) {
    return {};
  }
  return result;
}

std::uint64_t fnv1a64(std::string_view value) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string hex64(std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(16, '0');
  for (std::size_t index = result.size(); index > 0; --index) {
    result[index - 1] = digits[value & 0xFU];
    value >>= 4U;
  }
  return result;
}

std::size_t shortMessageLength(std::uint8_t status) {
  if (status < 0x80U) {
    return 0;
  }
  if (status < 0xF0U) {
    const std::uint8_t kind = status & 0xF0U;
    return kind == 0xC0U || kind == 0xD0U ? 2U : 3U;
  }
  switch (status) {
  case 0xF1U:
  case 0xF3U:
    return 2;
  case 0xF2U:
    return 3;
  default:
    return 1;
  }
}

std::vector<WinMidiDeviceDescriptor> enumerateDevices() {
  struct Candidate {
    UINT deviceId = 0;
    std::string fingerprint;
    std::string displayName;
  };

  std::vector<Candidate> candidates;
  const UINT deviceCount = midiInGetNumDevs();
  candidates.reserve(deviceCount);
  for (UINT deviceId = 0; deviceId < deviceCount; ++deviceId) {
    MIDIINCAPSW capabilities{};
    if (midiInGetDevCapsW(deviceId, &capabilities, sizeof(capabilities)) !=
        MMSYSERR_NOERROR) {
      continue;
    }
    std::string displayName = utf8FromWide(capabilities.szPname);
    if (displayName.empty()) {
      displayName = "Windows MIDI " + std::to_string(deviceId + 1U);
    }
    std::string fingerprint = std::to_string(capabilities.wMid) + ":" +
                              std::to_string(capabilities.wPid) + ":" +
                              std::to_string(capabilities.vDriverVersion) +
                              ":" + displayName;
    candidates.push_back({.deviceId = deviceId,
                          .fingerprint = std::move(fingerprint),
                          .displayName = std::move(displayName)});
  }

  std::map<std::string, std::size_t> totals;
  for (const auto &candidate : candidates) {
    ++totals[candidate.fingerprint];
  }
  std::map<std::string, std::size_t> ordinals;
  std::vector<WinMidiDeviceDescriptor> result;
  result.reserve(candidates.size());
  for (auto &candidate : candidates) {
    const std::size_t ordinal = ordinals[candidate.fingerprint]++;
    std::string stableId =
        "midi:winmm:" + hex64(fnv1a64(candidate.fingerprint));
    if (totals[candidate.fingerprint] > 1U) {
      stableId += ":" + std::to_string(ordinal + 1U);
    }
    result.push_back({.deviceId = candidate.deviceId,
                      .stableId = std::move(stableId),
                      .displayName = std::move(candidate.displayName)});
  }
  return result;
}

class WinMidiInputBackend final : public QueuedMidiInputBackend {
public:
  explicit WinMidiInputBackend(input::InputBackendSink sink)
      : QueuedMidiInputBackend(std::move(sink)) {}

  ~WinMidiInputBackend() override { stop(); }

  bool start(std::string &errorMessage) override {
    errorMessage.clear();
    if (started_) {
      return true;
    }
    openQueue();
    started_ = true;
    refreshDevices();
    nextRefresh_ = std::chrono::steady_clock::now() + kRefreshInterval;
    return true;
  }

  void stop() override {
    if (!started_) {
      closeQueue();
      return;
    }
    started_ = false;
    for (auto &[stableId, connection] : connections_) {
      (void)stableId;
      closeConnection(std::move(connection), false);
    }
    connections_.clear();
    refreshRequested_.store(false);
    closeQueue();
  }

  void pump() override {
    const auto now = std::chrono::steady_clock::now();
    if (started_ &&
        (refreshRequested_.exchange(false) || now >= nextRefresh_)) {
      refreshDevices();
      nextRefresh_ = now + kRefreshInterval;
    }
    QueuedMidiInputBackend::pump();
  }

  void acceptShortMessage(WinMidiConnection &connection, DWORD packedMessage,
                          DWORD elapsedMillis) {
    if (!connection.connected.load()) {
      return;
    }
    const auto status = static_cast<std::uint8_t>(packedMessage & 0xFFU);
    const std::size_t length = shortMessageLength(status);
    if (length == 0) {
      return;
    }
    std::vector<std::uint8_t> bytes(length);
    for (std::size_t index = 0; index < length; ++index) {
      bytes[index] =
          static_cast<std::uint8_t>((packedMessage >> (index * 8U)) & 0xFFU);
    }
    enqueuePacket(connection.stableId, std::move(bytes),
                  connection.startedAtMicros +
                      static_cast<std::uint64_t>(elapsedMillis) * 1000ULL);
  }

  void requestRefresh() { refreshRequested_.store(true); }

private:
  static constexpr auto kRefreshInterval = std::chrono::seconds(1);

  static void CALLBACK midiCallback(HMIDIIN, UINT message, DWORD_PTR instance,
                                    DWORD_PTR parameter1,
                                    DWORD_PTR parameter2) {
    auto lease = NativeCallbackLifetime::acquire(
        reinterpret_cast<void *>(instance));
    auto *connection = lease.ownerAs<WinMidiConnection>();
    if (connection == nullptr || !connection->connected.load()) {
      return;
    }
    if (message == MIM_DATA) {
      connection->backend->acceptShortMessage(
          *connection, static_cast<DWORD>(parameter1),
          static_cast<DWORD>(parameter2));
    } else if (message == MIM_ERROR || message == MIM_CLOSE) {
      connection->backend->requestRefresh();
    }
  }

  void refreshDevices() {
    const auto devices = enumerateDevices();
    std::set<std::string> currentStableIds;
    for (const auto &device : devices) {
      currentStableIds.insert(device.stableId);
      if (connections_.contains(device.stableId)) {
        continue;
      }

      auto connection = std::make_unique<WinMidiConnection>(this);
      connection->deviceId = device.deviceId;
      connection->stableId = device.stableId;
      connection->displayName = device.displayName;
      MMRESULT result = midiInOpen(
          &connection->handle, device.deviceId,
          reinterpret_cast<DWORD_PTR>(&WinMidiInputBackend::midiCallback),
          reinterpret_cast<DWORD_PTR>(connection->callbackLifetime.token()),
          CALLBACK_FUNCTION);
      if (result != MMSYSERR_NOERROR) {
        connection->connected.store(false);
        continue;
      }
      connection->startedAtMicros = monotonicMicros();
      auto activation = beginDeviceActivation(
          {.stableId = connection->stableId,
           .displayName = connection->displayName,
           .deviceClass = input::DeviceClass::Midi,
           .connected = true});
      result = midiInStart(connection->handle);
      if (result != MMSYSERR_NOERROR) {
        connection->connected.store(false);
        connection->callbackLifetime.closeAndWait();
        (void)midiInClose(connection->handle);
        connection->handle = nullptr;
        continue;
      }
      activation.commit();
      connections_.emplace(connection->stableId, std::move(connection));
    }

    for (auto iterator = connections_.begin();
         iterator != connections_.end();) {
      if (currentStableIds.contains(iterator->first)) {
        ++iterator;
        continue;
      }
      auto connection = std::move(iterator->second);
      iterator = connections_.erase(iterator);
      closeConnection(std::move(connection), true);
    }
  }

  void closeConnection(std::unique_ptr<WinMidiConnection> connection,
                       bool publishDisconnect) {
    if (!connection) {
      return;
    }
    connection->connected.store(false);
    connection->callbackLifetime.closeAndWait();
    if (connection->handle != nullptr) {
      (void)midiInStop(connection->handle);
      (void)midiInReset(connection->handle);
      (void)midiInClose(connection->handle);
      connection->handle = nullptr;
    }
    if (publishDisconnect) {
      enqueueDevice({.stableId = connection->stableId,
                     .displayName = connection->displayName,
                     .deviceClass = input::DeviceClass::Midi,
                     .connected = false});
    }
  }

  std::map<std::string, std::unique_ptr<WinMidiConnection>> connections_;
  std::atomic_bool refreshRequested_ = false;
  std::chrono::steady_clock::time_point nextRefresh_{};
  bool started_ = false;
};

} // namespace

std::unique_ptr<IInputBackend>
makeWinMidiInputBackend(input::InputBackendSink sink) {
  return std::make_unique<WinMidiInputBackend>(std::move(sink));
}

#endif
