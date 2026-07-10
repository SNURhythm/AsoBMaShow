#include "MidiPlatformBackends.h"

#if defined(__APPLE__)

#include "LiveMidiDeviceIdAllocator.h"
#include "NativeCallbackLifetime.h"
#include "QueuedMidiInputBackend.h"

#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class CoreMidiInputBackend;

struct CoreMidiConnection {
  explicit CoreMidiConnection(CoreMidiInputBackend *connectionBackend)
      : backend(connectionBackend), callbackLifetime(this) {}

  CoreMidiInputBackend *backend = nullptr;
  NativeCallbackLifetime callbackLifetime;
  MIDIEndpointRef endpoint = 0;
  std::string stableId;
  std::string displayName;
  std::atomic_bool connected = true;
};

class CoreMidiClientService {
public:
  static CoreMidiClientService &instance() {
    // CoreMIDI explicitly recommends keeping an application's last client
    // alive. The service and its callback refCon therefore have process
    // lifetime; backend ports and subscriptions remain independently owned.
    static CoreMidiClientService *service = new CoreMidiClientService();
    return *service;
  }

  OSStatus client(MIDIClientRef &client) {
    const std::lock_guard lock(clientMutex_);
    if (client_ == 0) {
      const OSStatus result =
          MIDIClientCreate(CFSTR("AsoBMaShow MIDI Input"),
                           &CoreMidiClientService::notify, this, &client_);
      if (result != noErr) {
        client_ = 0;
        client = 0;
        return result;
      }
    }
    client = client_;
    return noErr;
  }

  void subscribe(void *token) {
    const std::lock_guard lock(tokensMutex_);
    if (token != nullptr &&
        std::find(tokens_.begin(), tokens_.end(), token) == tokens_.end()) {
      tokens_.push_back(token);
    }
  }

  void unsubscribe(void *token) {
    const std::lock_guard lock(tokensMutex_);
    std::erase(tokens_, token);
  }

  void dispatch();

private:
  static void notify(const MIDINotification *, void *refCon) {
    if (refCon != nullptr) {
      static_cast<CoreMidiClientService *>(refCon)->dispatch();
    }
  }

  std::mutex clientMutex_;
  std::mutex tokensMutex_;
  MIDIClientRef client_ = 0;
  std::vector<void *> tokens_;
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
    notificationLifetime_ = std::make_unique<NativeCallbackLifetime>(this);
    OSStatus result = CoreMidiClientService::instance().client(client_);
    if (result == noErr) {
      CoreMidiClientService::instance().subscribe(notificationLifetime_->token());
      notificationSubscribed_ = true;
      result = MIDIInputPortCreate(client_, CFSTR("AsoBMaShow MIDI Port"),
                                   &CoreMidiInputBackend::read, nullptr, &port_);
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
    if (!started_ && port_ == 0 && !notificationLifetime_) {
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
  static void read(const MIDIPacketList *packetList, void *,
                   void *sourceConnectionRefCon) {
    if (packetList == nullptr) {
      return;
    }
    auto lease = NativeCallbackLifetime::acquire(sourceConnectionRefCon);
    auto *connection = lease.ownerAs<CoreMidiConnection>();
    if (connection != nullptr && connection->connected.load()) {
      connection->backend->acceptPackets(*connection, *packetList);
    }
  }

  void refreshSources() {
    const auto sources = enumerateSources();
    std::vector<std::uintptr_t> existingKeys;
    existingKeys.reserve(connections_.size());
    for (const auto &[endpoint, connection] : connections_) {
      (void)connection;
      existingKeys.push_back(static_cast<std::uintptr_t>(endpoint));
    }
    std::vector<std::uintptr_t> currentKeys;
    currentKeys.reserve(sources.size());
    for (const auto &source : sources) {
      currentKeys.push_back(static_cast<std::uintptr_t>(source.endpoint));
    }

    for (const auto action :
         planLiveMidiDeviceRefresh(existingKeys, currentKeys)) {
      const auto endpoint = static_cast<MIDIEndpointRef>(action.key);
      if (action.kind == LiveMidiDeviceRefreshActionKind::Remove) {
        auto iterator = connections_.find(endpoint);
        if (iterator == connections_.end()) {
          continue;
        }
        auto connection = std::move(iterator->second);
        connections_.erase(iterator);
        connection->connected.store(false);
        (void)MIDIPortDisconnectSource(port_, connection->endpoint);
        connection->callbackLifetime.closeAndWait();
        enqueueDevice({.stableId = connection->stableId,
                       .displayName = connection->displayName,
                       .deviceClass = input::DeviceClass::Midi,
                       .connected = false});
        liveIds_.release(action.key);
        continue;
      }

      const auto source =
          std::find_if(sources.begin(), sources.end(), [&](const auto &value) {
            return value.endpoint == endpoint;
          });
      if (source == sources.end() || connections_.contains(endpoint)) {
        continue;
      }
      auto connection = std::make_unique<CoreMidiConnection>(this);
      connection->endpoint = endpoint;
      connection->stableId = liveIds_.claim(action.key, source->stableId);
      connection->displayName = source->displayName;
      auto activation = beginDeviceActivation(
          {.stableId = connection->stableId,
           .displayName = connection->displayName,
           .deviceClass = input::DeviceClass::Midi,
           .connected = true});
      if (MIDIPortConnectSource(port_, endpoint,
                               connection->callbackLifetime.token()) != noErr) {
        connection->connected.store(false);
        connection->callbackLifetime.closeAndWait();
        liveIds_.release(action.key);
        continue;
      }
      activation.commit();
      connections_.emplace(endpoint, std::move(connection));
    }
  }

  void shutdownNative() {
    if (notificationSubscribed_ && notificationLifetime_) {
      CoreMidiClientService::instance().unsubscribe(
          notificationLifetime_->token());
      notificationSubscribed_ = false;
    }
    if (notificationLifetime_) {
      notificationLifetime_->closeAndWait();
      notificationLifetime_.reset();
    }
    for (auto &[endpoint, connection] : connections_) {
      connection->connected.store(false);
      if (port_ != 0) {
        (void)MIDIPortDisconnectSource(port_, connection->endpoint);
      }
      connection->callbackLifetime.closeAndWait();
      liveIds_.release(static_cast<std::uintptr_t>(endpoint));
    }
    connections_.clear();
    if (port_ != 0) {
      (void)MIDIPortDispose(port_);
      port_ = 0;
    }
    client_ = 0;
    liveIds_.clear();
    refreshRequested_.store(false);
  }

  MIDIClientRef client_ = 0;
  MIDIPortRef port_ = 0;
  std::unique_ptr<NativeCallbackLifetime> notificationLifetime_;
  std::map<MIDIEndpointRef, std::unique_ptr<CoreMidiConnection>> connections_;
  LiveMidiDeviceIdAllocator liveIds_;
  std::atomic_bool refreshRequested_ = false;
  bool notificationSubscribed_ = false;
  bool started_ = false;
};

void CoreMidiClientService::dispatch() {
  std::vector<void *> tokens;
  {
    const std::lock_guard lock(tokensMutex_);
    tokens = tokens_;
  }
  for (void *token : tokens) {
    auto lease = NativeCallbackLifetime::acquire(token);
    if (auto *backend = lease.ownerAs<CoreMidiInputBackend>()) {
      backend->requestRefresh();
    }
  }
}

} // namespace

std::unique_ptr<IInputBackend>
makeCoreMidiInputBackend(input::InputBackendSink sink) {
  return std::make_unique<CoreMidiInputBackend>(std::move(sink));
}

#endif
