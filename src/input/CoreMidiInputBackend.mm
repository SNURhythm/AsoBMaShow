#include "MidiPlatformBackends.h"

#if defined(__APPLE__)

#include "QueuedMidiInputBackend.h"

#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class CoreMidiInputBackend;

struct CoreMidiCallbackGate {
  CoreMidiInputBackend *acquire() {
    const std::lock_guard lock(mutex);
    if (closed || backend == nullptr) {
      return nullptr;
    }
    ++activeCallbacks;
    return backend;
  }

  void release() {
    const std::lock_guard lock(mutex);
    if (activeCallbacks > 0) {
      --activeCallbacks;
    }
    if (activeCallbacks == 0) {
      condition.notify_all();
    }
  }

  void close() {
    const std::lock_guard lock(mutex);
    closed = true;
    backend = nullptr;
  }

  void waitForCallbacks() {
    std::unique_lock lock(mutex);
    condition.wait(lock, [&] { return activeCallbacks == 0; });
  }

  std::mutex mutex;
  std::condition_variable condition;
  CoreMidiInputBackend *backend = nullptr;
  std::size_t activeCallbacks = 0;
  bool closed = false;
};

struct CoreMidiConnection {
  std::shared_ptr<CoreMidiCallbackGate> callbackGate;
  MIDIEndpointRef endpoint = 0;
  std::string stableId;
  std::string displayName;
  std::atomic_bool connected = true;
};

struct CoreMidiSourceDescriptor {
  MIDIEndpointRef endpoint = 0;
  std::string stableId;
  std::string displayName;
};

std::string stringProperty(MIDIObjectRef object, CFStringRef property) {
  CFStringRef value = nullptr;
  if (MIDIObjectGetStringProperty(object, property, &value) != noErr ||
      value == nullptr) {
    return {};
  }
  const CFIndex length = CFStringGetLength(value);
  const CFIndex maximumBytes =
      CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::string result;
  if (maximumBytes > 1) {
    std::vector<char> buffer(static_cast<std::size_t>(maximumBytes));
    if (CFStringGetCString(value, buffer.data(), maximumBytes,
                           kCFStringEncodingUTF8)) {
      result = buffer.data();
    }
  }
  CFRelease(value);
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

mach_timebase_info_data_t machTimebase() {
  mach_timebase_info_data_t value{};
  if (mach_timebase_info(&value) != KERN_SUCCESS || value.denom == 0) {
    value.numer = 1;
    value.denom = 1;
  }
  return value;
}

std::vector<CoreMidiSourceDescriptor> enumerateSources() {
  struct Candidate {
    MIDIEndpointRef endpoint = 0;
    std::string baseStableId;
    std::string fingerprint;
    std::string displayName;
  };

  std::vector<Candidate> candidates;
  const ItemCount count = MIDIGetNumberOfSources();
  candidates.reserve(count);
  for (ItemCount index = 0; index < count; ++index) {
    const MIDIEndpointRef endpoint = MIDIGetSource(index);
    if (endpoint == 0) {
      continue;
    }
    std::string displayName = stringProperty(endpoint, kMIDIPropertyDisplayName);
    if (displayName.empty()) {
      displayName = stringProperty(endpoint, kMIDIPropertyName);
    }
    if (displayName.empty()) {
      displayName = "CoreMIDI Source";
    }
    const std::string fingerprint =
        stringProperty(endpoint, kMIDIPropertyManufacturer) + "\n" +
        stringProperty(endpoint, kMIDIPropertyModel) + "\n" +
        stringProperty(endpoint, kMIDIPropertyName) + "\n" + displayName;

    SInt32 uniqueId = 0;
    std::string baseStableId;
    if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID,
                                     &uniqueId) == noErr &&
        uniqueId != 0) {
      baseStableId = "midi:core:" + std::to_string(uniqueId);
    } else {
      baseStableId = "midi:core:fallback:" + hex64(fnv1a64(fingerprint));
    }
    candidates.push_back({.endpoint = endpoint,
                          .baseStableId = std::move(baseStableId),
                          .fingerprint = fingerprint,
                          .displayName = std::move(displayName)});
  }

  std::map<std::string, std::size_t> baseTotals;
  for (const auto &candidate : candidates) {
    ++baseTotals[candidate.baseStableId];
  }
  std::vector<std::string> proposedIds;
  proposedIds.reserve(candidates.size());
  std::map<std::string, std::size_t> proposedTotals;
  for (const auto &candidate : candidates) {
    std::string proposed = candidate.baseStableId;
    if (baseTotals[candidate.baseStableId] > 1U) {
      proposed += ":" + hex64(fnv1a64(candidate.fingerprint));
    }
    ++proposedTotals[proposed];
    proposedIds.push_back(std::move(proposed));
  }

  std::map<std::string, std::size_t> ordinals;
  std::vector<CoreMidiSourceDescriptor> result;
  result.reserve(candidates.size());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    std::string stableId = proposedIds[index];
    const std::size_t ordinal = ordinals[stableId]++;
    if (proposedTotals[stableId] > 1U) {
      stableId += ":" + std::to_string(ordinal + 1U);
    }
    result.push_back({.endpoint = candidates[index].endpoint,
                      .stableId = std::move(stableId),
                      .displayName = std::move(candidates[index].displayName)});
  }
  return result;
}

std::uint64_t nowMicros() {
  static const mach_timebase_info_data_t timebase = machTimebase();
  const std::uint64_t ticks = mach_absolute_time();
  const auto nanos =
      static_cast<unsigned __int128>(ticks) * timebase.numer / timebase.denom;
  return static_cast<std::uint64_t>(nanos / 1000U);
}

std::uint64_t midiTimestampMicros(MIDITimeStamp timestamp) {
  if (timestamp == 0) {
    return nowMicros();
  }
  static const mach_timebase_info_data_t timebase = machTimebase();
  const auto nanos = static_cast<unsigned __int128>(timestamp) *
                     timebase.numer / timebase.denom;
  const auto micros = nanos / 1000U;
  return micros > std::numeric_limits<std::uint64_t>::max()
             ? std::numeric_limits<std::uint64_t>::max()
             : static_cast<std::uint64_t>(micros);
}

class CoreMidiInputBackend final : public QueuedMidiInputBackend {
public:
  explicit CoreMidiInputBackend(input::InputBackendSink sink)
      : QueuedMidiInputBackend(std::move(sink)) {}

  ~CoreMidiInputBackend() override { stop(); }

  bool start(std::string &errorMessage) override {
    errorMessage.clear();
    if (started_) {
      return true;
    }

    openQueue();
    callbackGate_ = std::make_shared<CoreMidiCallbackGate>();
    callbackGate_->backend = this;
    OSStatus result = MIDIClientCreate(CFSTR("AsoBMaShow MIDI Input"),
                                       &CoreMidiInputBackend::notify,
                                       callbackGate_.get(), &client_);
    if (result == noErr) {
      result = MIDIInputPortCreate(client_, CFSTR("AsoBMaShow MIDI Port"),
                                   &CoreMidiInputBackend::read, this, &port_);
    }
    if (result != noErr) {
      errorMessage = "CoreMIDI initialization failed (" +
                     std::to_string(static_cast<long long>(result)) + ").";
      shutdownNative();
      closeQueue();
      return false;
    }

    started_ = true;
    refreshSources();
    return true;
  }

  void stop() override {
    if (!started_ && client_ == 0 && port_ == 0 && !callbackGate_) {
      closeQueue();
      return;
    }
    started_ = false;
    shutdownNative();
    closeQueue();
  }

  void pump() override {
    if (started_ && refreshRequested_.exchange(false)) {
      refreshSources();
    }
    QueuedMidiInputBackend::pump();
  }

  void requestRefresh() { refreshRequested_.store(true); }

  void acceptPackets(CoreMidiConnection &connection,
                     const MIDIPacketList &packetList) {
    if (!connection.connected.load()) {
      return;
    }
    const MIDIPacket *packet = &packetList.packet[0];
    for (UInt32 index = 0; index < packetList.numPackets; ++index) {
      if (!connection.connected.load()) {
        return;
      }
      enqueuePacket(connection.stableId,
                    std::vector<std::uint8_t>(packet->data,
                                              packet->data + packet->length),
                    midiTimestampMicros(packet->timeStamp));
      packet = MIDIPacketNext(packet);
    }
  }

private:
  static void notify(const MIDINotification *, void *refCon) {
    auto *gate = static_cast<CoreMidiCallbackGate *>(refCon);
    if (gate == nullptr) {
      return;
    }
    CoreMidiInputBackend *backend = gate->acquire();
    if (backend != nullptr) {
      backend->requestRefresh();
      gate->release();
    }
  }

  static void read(const MIDIPacketList *packetList, void *,
                   void *sourceConnectionRefCon) {
    auto *connection =
        static_cast<CoreMidiConnection *>(sourceConnectionRefCon);
    if (packetList == nullptr || connection == nullptr ||
        !connection->connected.load()) {
      return;
    }
    const auto gate = connection->callbackGate;
    CoreMidiInputBackend *backend = gate != nullptr ? gate->acquire() : nullptr;
    if (backend != nullptr) {
      backend->acceptPackets(*connection, *packetList);
      gate->release();
    }
  }

  void refreshSources() {
    std::set<MIDIEndpointRef> currentEndpoints;
    for (const auto &source : enumerateSources()) {
      const MIDIEndpointRef endpoint = source.endpoint;
      currentEndpoints.insert(endpoint);
      if (connections_.contains(endpoint)) {
        continue;
      }

      auto connection = std::make_unique<CoreMidiConnection>();
      connection->callbackGate = callbackGate_;
      connection->endpoint = endpoint;
      connection->stableId = source.stableId;
      connection->displayName = source.displayName;
      if (MIDIPortConnectSource(port_, endpoint, connection.get()) != noErr) {
        connection->connected.store(false);
        continue;
      }
      enqueueDevice({.stableId = connection->stableId,
                     .displayName = connection->displayName,
                     .deviceClass = input::DeviceClass::Midi,
                     .connected = true});
      connections_.emplace(endpoint, std::move(connection));
    }

    for (auto iterator = connections_.begin();
         iterator != connections_.end();) {
      if (currentEndpoints.contains(iterator->first)) {
        ++iterator;
        continue;
      }
      auto connection = std::move(iterator->second);
      iterator = connections_.erase(iterator);
      connection->connected.store(false);
      (void)MIDIPortDisconnectSource(port_, connection->endpoint);
      enqueueDevice({.stableId = connection->stableId,
                     .displayName = connection->displayName,
                     .deviceClass = input::DeviceClass::Midi,
                     .connected = false});
      retiredConnections_.push_back(std::move(connection));
    }
  }

  void shutdownNative() {
    if (callbackGate_) {
      callbackGate_->close();
    }
    for (auto &[endpoint, connection] : connections_) {
      (void)endpoint;
      connection->connected.store(false);
      if (port_ != 0) {
        (void)MIDIPortDisconnectSource(port_, connection->endpoint);
      }
      retiredConnections_.push_back(std::move(connection));
    }
    connections_.clear();
    if (port_ != 0) {
      (void)MIDIPortDispose(port_);
      port_ = 0;
    }
    if (client_ != 0) {
      (void)MIDIClientDispose(client_);
      client_ = 0;
    }
    if (callbackGate_) {
      callbackGate_->waitForCallbacks();
    }
    retiredConnections_.clear();
    callbackGate_.reset();
    refreshRequested_.store(false);
  }

  MIDIClientRef client_ = 0;
  MIDIPortRef port_ = 0;
  std::shared_ptr<CoreMidiCallbackGate> callbackGate_;
  std::map<MIDIEndpointRef, std::unique_ptr<CoreMidiConnection>> connections_;
  std::vector<std::unique_ptr<CoreMidiConnection>> retiredConnections_;
  std::atomic_bool refreshRequested_ = false;
  bool started_ = false;
};

} // namespace

std::unique_ptr<IInputBackend>
makeCoreMidiInputBackend(input::InputBackendSink sink) {
  return std::make_unique<CoreMidiInputBackend>(std::move(sink));
}

#endif
