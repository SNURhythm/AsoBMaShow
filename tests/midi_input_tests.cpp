#include "input/LiveMidiDeviceIdAllocator.h"
#include "input/MidiMessageParser.h"
#include "input/NativeCallbackLifetime.h"
#include "input/QueuedMidiInputBackend.h"
#include "input/Utf16ToUtf8.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class TestMidiBackend final : public QueuedMidiInputBackend {
public:
  explicit TestMidiBackend(input::InputBackendSink sink)
      : QueuedMidiInputBackend(std::move(sink)) {}

  bool start(std::string &errorMessage) override {
    errorMessage.clear();
    openQueue();
    return true;
  }

  void stop() override { closeQueue(); }

  void connect(std::string stableId, std::string displayName) {
    enqueueDevice({.stableId = std::move(stableId),
                   .displayName = std::move(displayName),
                   .deviceClass = input::DeviceClass::Midi,
                   .connected = true});
  }

  void disconnect(std::string stableId, std::string displayName) {
    enqueueDevice({.stableId = std::move(stableId),
                   .displayName = std::move(displayName),
                   .deviceClass = input::DeviceClass::Midi,
                   .connected = false});
  }

  void packet(std::string stableId, std::vector<std::uint8_t> bytes,
              std::uint64_t timestampMicros) {
    enqueuePacket(std::move(stableId), std::move(bytes), timestampMicros);
  }

  void immediatePacket(std::string_view stableId,
                       std::span<const std::uint8_t> bytes,
                       std::uint64_t timestampMicros) {
    publishPacketImmediately(stableId, bytes, timestampMicros);
  }

  void activate(std::string stableId, bool success, bool emitPacket) {
    const std::string packetDeviceId = stableId;
    auto activation = beginDeviceActivation(
        {.stableId = std::move(stableId),
         .displayName = "Activation",
         .deviceClass = input::DeviceClass::Midi,
         .connected = true});
    if (emitPacket) {
      packet(packetDeviceId, {0x90, 60, 127}, 200);
    }
    if (success) {
      activation.commit();
    }
  }

  void activateThenDisconnect(std::string stableId) {
    const std::string disconnectedId = stableId;
    auto activation = beginDeviceActivation(
        {.stableId = std::move(stableId),
         .displayName = "Activation",
         .deviceClass = input::DeviceClass::Midi,
         .connected = true});
    activation.commit();
    enqueueDeviceDisconnect(
        {.stableId = disconnectedId,
         .displayName = "Activation",
         .deviceClass = input::DeviceClass::Midi,
         .connected = true});
  }
};

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void requireNear(float actual, float expected, std::string_view message) {
  require(std::fabs(actual - expected) < 0.00001F, message);
}

void requireMidiEvent(const input::PhysicalInputEvent &event,
                      std::string_view deviceId, input::ControlKind kind,
                      int index, double rawValue, float normalizedValue,
                      std::uint64_t timestampMicros) {
  require(event.control.deviceId == deviceId, "MIDI event keeps device ID");
  require(event.control.deviceClass == input::DeviceClass::Midi,
          "MIDI event reports the MIDI device class");
  require(event.control.kind == kind, "MIDI event keeps the control kind");
  require(event.control.index == index, "MIDI event uses canonical index");
  require(event.control.direction == input::ControlDirection::Any,
          "MIDI controls are direction-neutral");
  require(event.rawValue == rawValue, "MIDI event preserves raw value");
  requireNear(event.normalizedValue, normalizedValue,
              "MIDI event normalizes to zero through one");
  require(event.timestampMicros == timestampMicros,
          "MIDI event keeps packet timestamp");
}

void testNoteOnOffAndVelocityZero() {
  MidiMessageParser parser;
  const auto noteOn = parser.consume(
      "midi:test", std::array<std::uint8_t, 3>{0x92, 60, 127}, 10);
  require(noteOn.size() == 1, "note-on emits one event");
  requireMidiEvent(noteOn.front(), "midi:test", input::ControlKind::MidiNote,
                   2 * 128 + 60, 127.0, 1.0F, 10);

  const auto noteOff = parser.consume(
      "midi:test", std::array<std::uint8_t, 3>{0x82, 60, 73}, 11);
  require(noteOff.size() == 1, "note-off emits one release");
  requireMidiEvent(noteOff.front(), "midi:test", input::ControlKind::MidiNote,
                   2 * 128 + 60, 0.0, 0.0F, 11);

  const auto zeroVelocity =
      parser.consume("midi:test", std::array<std::uint8_t, 3>{0x92, 61, 0}, 12);
  require(zeroVelocity.size() == 1, "zero-velocity note-on emits one release");
  requireMidiEvent(zeroVelocity.front(), "midi:test",
                   input::ControlKind::MidiNote, 2 * 128 + 61, 0.0, 0.0F, 12);
}

void testControlChangeNormalization() {
  MidiMessageParser parser;
  const auto events = parser.consume(
      "midi:knobs", std::array<std::uint8_t, 3>{0xB1, 7, 64}, 20);
  require(events.size() == 1, "control change emits one event");
  requireMidiEvent(events.front(), "midi:knobs",
                   input::ControlKind::MidiControl, 1 * 128 + 7, 64.0,
                   64.0F / 127.0F, 20);
}

void testRunningStatusAcrossPacketsAndRealtimeBytes() {
  MidiMessageParser parser;
  require(parser.consume("midi:keys", std::array<std::uint8_t, 2>{0x90, 60}, 30)
              .empty(),
          "incomplete note buffers across packets");
  const auto continued =
      parser.consume("midi:keys", std::array<std::uint8_t, 3>{100, 61, 0}, 31);
  require(continued.size() == 2,
          "running status completes buffered and following messages");
  requireMidiEvent(continued[0], "midi:keys", input::ControlKind::MidiNote, 60,
                   100.0, 100.0F / 127.0F, 31);
  requireMidiEvent(continued[1], "midi:keys", input::ControlKind::MidiNote, 61,
                   0.0, 0.0F, 31);

  const auto afterRealtime = parser.consume(
      "midi:keys", std::array<std::uint8_t, 4>{0xF8, 62, 0xFA, 127}, 32);
  require(afterRealtime.size() == 1,
          "real-time bytes do not cancel running status");
  requireMidiEvent(afterRealtime.front(), "midi:keys",
                   input::ControlKind::MidiNote, 62, 127.0, 1.0F, 32);
}

void testMultipleMessagesAndStatusReplacement() {
  MidiMessageParser parser;
  const auto events = parser.consume(
      "midi:multi",
      std::array<std::uint8_t, 9>{0x90, 40, 1, 41, 2, 0xB0, 10, 127, 0xF8}, 40);
  require(events.size() == 3,
          "one packet can emit running and explicit-status messages");
  requireMidiEvent(events[0], "midi:multi", input::ControlKind::MidiNote, 40,
                   1.0, 1.0F / 127.0F, 40);
  requireMidiEvent(events[1], "midi:multi", input::ControlKind::MidiNote, 41,
                   2.0, 2.0F / 127.0F, 40);
  requireMidiEvent(events[2], "midi:multi", input::ControlKind::MidiControl, 10,
                   127.0, 1.0F, 40);
}

void testNewStatusAbandonsIncompleteMessageWithoutGettingStuck() {
  MidiMessageParser parser;
  require(
      parser.consume("midi:recover", std::array<std::uint8_t, 2>{0x90, 60}, 50)
          .empty(),
      "partial message is buffered initially");
  const auto recovered = parser.consume(
      "midi:recover", std::array<std::uint8_t, 5>{0xB0, 7, 100, 8, 127}, 51);
  require(
      recovered.size() == 2,
      "new status discards partial data and establishes new running status");
  requireMidiEvent(recovered[0], "midi:recover",
                   input::ControlKind::MidiControl, 7, 100.0, 100.0F / 127.0F,
                   51);
  requireMidiEvent(recovered[1], "midi:recover",
                   input::ControlKind::MidiControl, 8, 127.0, 1.0F, 51);
}

void testRealtimeCanAppearInsideAChannelMessage() {
  MidiMessageParser parser;
  const auto events =
      parser.consume("midi:clocked",
                     std::array<std::uint8_t, 5>{0x90, 0xF8, 63, 0xFE, 96}, 60);
  require(events.size() == 1,
          "real-time bytes are ignored inside an incomplete message");
  requireMidiEvent(events.front(), "midi:clocked", input::ControlKind::MidiNote,
                   63, 96.0, 96.0F / 127.0F, 60);
}

void testSystemExclusiveAndCommonMessagesClearRunningStatus() {
  MidiMessageParser parser;
  const auto sysex =
      parser.consume("midi:system",
                     std::array<std::uint8_t, 12>{0x90, 60, 1, 0xF0, 1, 2, 0xF8,
                                                  3, 0xF7, 61, 2, 0xF8},
                     70);
  require(sysex.size() == 1,
          "SysEx is ignored and its terminator leaves no running status");
  requireMidiEvent(sysex.front(), "midi:system", input::ControlKind::MidiNote,
                   60, 1.0, 1.0F / 127.0F, 70);

  const auto common = parser.consume(
      "midi:system",
      std::array<std::uint8_t, 10>{0x90, 62, 3, 0xF2, 0, 0, 63, 4, 0xF6, 0xF8},
      71);
  require(common.size() == 1,
          "system-common messages consume their data and clear running status");
  requireMidiEvent(common.front(), "midi:system", input::ControlKind::MidiNote,
                   62, 3.0, 3.0F / 127.0F, 71);
}

void testSystemMessagesSpanPacketsAndMalformedSysexRecovers() {
  MidiMessageParser parser;
  require(parser
              .consume("midi:system-split",
                       std::array<std::uint8_t, 2>{0xF2, 1}, 75)
              .empty(),
          "incomplete system-common data buffers across packets");
  const auto afterCommon = parser.consume(
      "midi:system-split",
      std::array<std::uint8_t, 7>{0xF8, 2, 64, 1, 0x90, 65, 127}, 76);
  require(afterCommon.size() == 1,
          "system-common completion leaves data bytes without running status");
  requireMidiEvent(afterCommon.front(), "midi:system-split",
                   input::ControlKind::MidiNote, 65, 127.0, 1.0F, 76);

  const auto malformedSysex = parser.consume(
      "midi:system-split",
      std::array<std::uint8_t, 7>{0xF0, 1, 2, 0x90, 66, 64, 0xF8}, 77);
  require(malformedSysex.size() == 1,
          "a channel status aborts unterminated SysEx and recovers framing");
  requireMidiEvent(malformedSysex.front(), "midi:system-split",
                   input::ControlKind::MidiNote, 66, 64.0, 64.0F / 127.0F, 77);
}

void testIgnoredChannelMessagesConsumeCorrectLengths() {
  MidiMessageParser parser;
  const auto ignored = parser.consume(
      "midi:ignored",
      std::array<std::uint8_t, 16>{0xA0, 1, 2, 0xC0, 3, 4, 0xD0, 5, 6, 0xE0, 7,
                                   8, 0x90, 64, 127, 0xF8},
      80);
  require(ignored.size() == 1,
          "unsupported channel messages keep stream framing synchronized");
  requireMidiEvent(ignored.front(), "midi:ignored",
                   input::ControlKind::MidiNote, 64, 127.0, 1.0F, 80);
}

void testResetAndDeviceSwitchClearPartialState() {
  MidiMessageParser parser;
  require(parser.consume("midi:a", std::array<std::uint8_t, 2>{0x90, 65}, 90)
              .empty(),
          "device A starts a partial message");
  require(
      parser.consume("midi:b", std::array<std::uint8_t, 1>{127}, 91).empty(),
      "device B cannot complete device A's partial message");
  const auto deviceB =
      parser.consume("midi:b", std::array<std::uint8_t, 3>{0x91, 66, 127}, 92);
  require(deviceB.size() == 1, "device B can establish its own status");
  requireMidiEvent(deviceB.front(), "midi:b", input::ControlKind::MidiNote,
                   128 + 66, 127.0, 1.0F, 92);

  parser.reset();
  require(parser.consume("midi:b", std::array<std::uint8_t, 2>{67, 127}, 93)
              .empty(),
          "reset clears running status");
  require(parser.consume("midi:b", std::array<std::uint8_t, 3>{0xF0, 1, 2}, 94)
              .empty(),
          "reset test can enter SysEx state");
  parser.reset();
  const auto afterSysexReset =
      parser.consume("midi:b", std::array<std::uint8_t, 3>{0x90, 68, 1}, 95);
  require(afterSysexReset.size() == 1, "reset exits SysEx state");
}

void testMalformedBytesRecoverAtNextValidStatus() {
  MidiMessageParser parser;
  require(parser
              .consume("midi:bad",
                       std::array<std::uint8_t, 6>{1, 2, 0xF4, 3, 4, 0xF7}, 100)
              .empty(),
          "stray data and undefined system status are ignored");
  const auto recovered = parser.consume(
      "midi:bad", std::array<std::uint8_t, 3>{0x9F, 127, 127}, 101);
  require(recovered.size() == 1,
          "malformed input cannot leave the parser permanently stuck");
  requireMidiEvent(recovered.front(), "midi:bad", input::ControlKind::MidiNote,
                   15 * 128 + 127, 127.0, 1.0F, 101);
}

void testBackendQueuesNativePacketsUntilMainThreadPump() {
  std::vector<input::PhysicalInputEvent> inputs;
  std::vector<input::InputDeviceSnapshot> devices;
  TestMidiBackend backend({
      .enqueueInput =
          [&](input::PhysicalInputEvent event) {
            inputs.push_back(std::move(event));
          },
      .enqueueDevice =
          [&](input::InputDeviceSnapshot device) {
            devices.push_back(std::move(device));
          },
  });
  std::string error;
  require(backend.start(error), "test MIDI backend starts");
  backend.connect("midi:test", "Test keyboard");
  backend.packet("midi:test", {0x90, 60, 127}, 110);
  require(inputs.empty() && devices.empty(),
          "native callbacks only enqueue before main-thread pump");
  backend.pump();
  require(devices.size() == 1 && devices.front().connected,
          "pump publishes the queued MIDI connection");
  require(inputs.size() == 1, "pump parses and publishes the queued packet");
  requireMidiEvent(inputs.front(), "midi:test", input::ControlKind::MidiNote,
                   60, 127.0, 1.0F, 110);
  backend.stop();
}

void testBackendCanPublishNativePacketsImmediately() {
  std::vector<input::PhysicalInputEvent> inputs;
  TestMidiBackend backend({
      .enqueueInput =
          [&](input::PhysicalInputEvent event) {
            inputs.push_back(std::move(event));
          },
      .enqueueDevice = [](input::InputDeviceSnapshot) {},
  });
  std::string error;
  require(backend.start(error), "immediate MIDI backend starts");

  constexpr std::array<std::uint8_t, 3> note{0x93, 64, 111};
  backend.immediatePacket("midi:immediate", note, 111111);
  require(inputs.size() == 1,
          "native MIDI packet publishes without waiting for pump");
  requireMidiEvent(inputs.front(), "midi:immediate",
                   input::ControlKind::MidiNote, 3 * 128 + 64, 111.0,
                   111.0F / 127.0F, 111111);

  backend.stop();
  backend.immediatePacket("midi:immediate", note, 111112);
  require(inputs.size() == 1,
          "stopped MIDI backend rejects delayed immediate callbacks");
}

void testBackendIsolatesParsersAndDropsPacketsAfterDisconnect() {
  std::vector<input::PhysicalInputEvent> inputs;
  std::vector<input::InputDeviceSnapshot> devices;
  TestMidiBackend backend({
      .enqueueInput =
          [&](input::PhysicalInputEvent event) {
            inputs.push_back(std::move(event));
          },
      .enqueueDevice =
          [&](input::InputDeviceSnapshot device) {
            devices.push_back(std::move(device));
          },
  });
  std::string error;
  require(backend.start(error), "isolated MIDI backend starts");
  backend.connect("midi:a", "A");
  backend.connect("midi:b", "B");
  backend.packet("midi:a", {0x90, 60}, 120);
  backend.packet("midi:b", {0x91, 61, 127}, 121);
  backend.packet("midi:a", {127}, 122);
  backend.pump();
  require(inputs.size() == 2,
          "each connected device retains independent parser state");
  requireMidiEvent(inputs[0], "midi:b", input::ControlKind::MidiNote, 128 + 61,
                   127.0, 1.0F, 121);
  requireMidiEvent(inputs[1], "midi:a", input::ControlKind::MidiNote, 60, 127.0,
                   1.0F, 122);

  backend.disconnect("midi:a", "A");
  backend.packet("midi:a", {0x90, 62, 127}, 123);
  backend.pump();
  require(inputs.size() == 2,
          "packets queued after disconnect are not published");
  require(devices.size() == 3 && !devices.back().connected,
          "disconnect reaches the registry sink in queue order");
  backend.stop();
}

void testBackendCloseRevokesQueuedNativeWork() {
  std::vector<input::PhysicalInputEvent> inputs;
  std::vector<input::InputDeviceSnapshot> devices;
  TestMidiBackend backend({
      .enqueueInput =
          [&](input::PhysicalInputEvent event) {
            inputs.push_back(std::move(event));
          },
      .enqueueDevice =
          [&](input::InputDeviceSnapshot device) {
            devices.push_back(std::move(device));
          },
  });
  std::string error;
  require(backend.start(error), "revocation MIDI backend starts");
  backend.connect("midi:late", "Late");
  backend.packet("midi:late", {0x90, 60, 127}, 130);
  backend.stop();
  backend.packet("midi:late", {0x90, 61, 127}, 131);
  backend.pump();
  require(inputs.empty() && devices.empty(),
          "stop clears queued work and rejects late native callbacks");
}

void testBackendOverflowForcesAReleaseBoundary() {
  std::vector<input::PhysicalInputEvent> inputs;
  std::vector<input::InputDeviceSnapshot> devices;
  TestMidiBackend backend({
      .enqueueInput =
          [&](input::PhysicalInputEvent event) {
            inputs.push_back(std::move(event));
          },
      .enqueueDevice =
          [&](input::InputDeviceSnapshot device) {
            devices.push_back(std::move(device));
          },
  });
  std::string error;
  require(backend.start(error), "overflow MIDI backend starts");
  backend.connect("midi:overflow", "Overflow");
  backend.packet("midi:overflow", std::vector<std::uint8_t>(64 * 1024 + 1),
                 140);
  backend.pump();
  require(inputs.empty(), "oversized native packets are never parsed");
  require(devices.size() == 3 && devices[0].connected &&
              !devices[1].connected && devices[2].connected,
          "queue overflow emits a disconnect/reconnect release boundary");
  backend.stop();
}

void testBackendAcceptsConcurrentNativeProducers() {
  std::vector<input::PhysicalInputEvent> inputs;
  TestMidiBackend backend({
      .enqueueInput =
          [&](input::PhysicalInputEvent event) {
            inputs.push_back(std::move(event));
          },
      .enqueueDevice = [](input::InputDeviceSnapshot) {},
  });
  std::string error;
  require(backend.start(error), "concurrent MIDI backend starts");
  for (int producer = 0; producer < 4; ++producer) {
    backend.connect("midi:thread-" + std::to_string(producer), "Thread");
  }
  backend.pump();

  std::vector<std::jthread> producers;
  for (int producer = 0; producer < 4; ++producer) {
    producers.emplace_back([&, producer] {
      for (int message = 0; message < 100; ++message) {
        backend.packet("midi:thread-" + std::to_string(producer),
                       {0x90, static_cast<std::uint8_t>(message % 128), 127},
                       static_cast<std::uint64_t>(150 + message));
      }
    });
  }
  producers.clear();
  backend.pump();
  require(inputs.size() == 400,
          "concurrent native callbacks enqueue every bounded packet safely");
  backend.stop();
}

void testNativeCallbackLifetimeWaitsForActiveLease() {
  int owner = 7;
  NativeCallbackLifetime lifetime(&owner);
  const void *token = lifetime.token();
  std::optional<NativeCallbackLifetime::Lease> lease(
      NativeCallbackLifetime::acquire(const_cast<void *>(token)));
  require(*lease && lease->ownerAs<int>() == &owner,
          "callback lease retains its registered owner");

  std::binary_semaphore closeStarted(0);
  std::binary_semaphore closeFinished(0);
  std::jthread closer([&] {
    closeStarted.release();
    lifetime.closeAndWait();
    closeFinished.release();
  });
  closeStarted.acquire();
  require(!closeFinished.try_acquire_for(std::chrono::milliseconds(20)),
          "callback close waits while an acquired lease is active");
  lease.reset();
  closeFinished.acquire();
  closer.join();
  require(!NativeCallbackLifetime::acquire(const_cast<void *>(token)),
          "closed callback token cannot be acquired again");
}

void testNativeCallbackLifetimeRejectsDelayedEntryAfterClose() {
  int owner = 11;
  NativeCallbackLifetime lifetime(&owner);
  void *token = lifetime.token();
  lifetime.closeAndWait();

  auto delayedEntry = NativeCallbackLifetime::acquire(token);
  require(!delayedEntry,
          "callback delayed before token lookup cannot enter after close");
}

void testNativeCallbackLifetimeNeverReusesStaleTokens() {
  int firstOwner = 1;
  NativeCallbackLifetime first(&firstOwner);
  void *staleToken = first.token();
  first.closeAndWait();

  int secondOwner = 2;
  NativeCallbackLifetime second(&secondOwner);
  require(second.token() != staleToken,
          "new callback lifetime receives a non-reused opaque token");
  require(!NativeCallbackLifetime::acquire(staleToken),
          "stale token cannot alias a newly registered callback owner");
  {
    auto current = NativeCallbackLifetime::acquire(second.token());
    require(current && current.ownerAs<int>() == &secondOwner,
            "current opaque token resolves to the current owner");
  }
  second.closeAndWait();
}

void testBackendPublishesConnectBeforeSynchronousActivationPacket() {
  std::vector<std::string> publicationOrder;
  TestMidiBackend backend({
      .enqueueInput =
          [&](input::PhysicalInputEvent) {
            publicationOrder.emplace_back("input");
          },
      .enqueueDevice =
          [&](input::InputDeviceSnapshot device) {
            publicationOrder.emplace_back(device.connected ? "connect"
                                                            : "disconnect");
          },
  });
  std::string error;
  require(backend.start(error), "activation-order MIDI backend starts");
  backend.activate("midi:activation", true, true);
  backend.pump();
  require(publicationOrder == std::vector<std::string>{"connect", "input"},
          "device connect publishes before a synchronous activation packet");
  backend.stop();
}

void testBackendRollsBackFailedActivationBeforeLaterPackets() {
  std::vector<input::PhysicalInputEvent> inputs;
  std::vector<bool> connectionStates;
  TestMidiBackend backend({
      .enqueueInput =
          [&](input::PhysicalInputEvent event) {
            inputs.push_back(std::move(event));
          },
      .enqueueDevice =
          [&](input::InputDeviceSnapshot device) {
            connectionStates.push_back(device.connected);
          },
  });
  std::string error;
  require(backend.start(error), "activation-rollback MIDI backend starts");
  backend.activate("midi:failed-activation", false, false);
  backend.packet("midi:failed-activation", {0x90, 61, 127}, 201);
  backend.pump();
  require(connectionStates == std::vector<bool>{true, false},
          "failed activation publishes a compensating disconnect");
  require(inputs.empty(),
          "packets after activation rollback remain disconnected and drop");
  backend.stop();
}

void testBackendQueuesDisconnectBehindPendingActivation() {
  std::vector<bool> connectionStates;
  TestMidiBackend backend({
      .enqueueInput = [](input::PhysicalInputEvent) {},
      .enqueueDevice =
          [&](input::InputDeviceSnapshot device) {
            connectionStates.push_back(device.connected);
          },
  });
  std::string error;
  require(backend.start(error), "disconnect-order MIDI backend starts");
  backend.activateThenDisconnect("midi:short-lived");
  require(connectionStates.empty(),
          "activation and disconnect both wait for the queue pump");
  backend.pump();
  require(connectionStates == std::vector<bool>{true, false},
          "disconnect remains ordered behind its pending connection");
  backend.stop();
}

void testLiveMidiDeviceIdsStayUniqueAcrossIdenticalDeviceReadd() {
  LiveMidiDeviceIdAllocator ids;
  require(ids.claim(1, "midi:core:base") == "midi:core:base",
          "first live CoreMIDI source claims its preferred ID");
  const std::string deviceB =
      ids.claim(2, "midi:core:base:identical:2");
  ids.release(1);
  const std::string readdedDeviceA =
      ids.claim(3, "midi:core:base:identical:2");
  require(readdedDeviceA != deviceB,
          "re-added identical source cannot collide with a live source");
  require(ids.claim(2, "midi:core:changed-preference") == deviceB,
          "an existing live source preserves its claimed stable ID");

  ids.release(2);
  require(ids.claim(4, deviceB) == deviceB,
          "released stable ID can be reclaimed by a later source");
  ids.clear();
  require(ids.claim(5, "midi:core:base") == "midi:core:base",
          "clearing a stopped backend releases every live ID");
}

void testLiveMidiRefreshReleasesReplacementBeforeNewClaims() {
  constexpr std::string_view canonicalId = "midi:core:42";
  LiveMidiDeviceIdAllocator ids;
  require(ids.claim(1, std::string(canonicalId)) == canonicalId,
          "old CoreMIDI endpoint initially owns the canonical ID");

  constexpr std::array<std::uintptr_t, 1> existingKeys{1};
  constexpr std::array<std::uintptr_t, 2> currentKeys{2, 3};
  std::string replacementId;
  std::string duplicateId;
  std::vector<LiveMidiDeviceRefreshActionKind> actionKinds;
  for (const auto action :
       planLiveMidiDeviceRefresh(existingKeys, currentKeys)) {
    actionKinds.push_back(action.kind);
    if (action.kind == LiveMidiDeviceRefreshActionKind::Remove) {
      ids.release(action.key);
      continue;
    }
    const std::string claimed = ids.claim(action.key, std::string(canonicalId));
    if (action.key == 2) {
      replacementId = claimed;
    } else if (action.key == 3) {
      duplicateId = claimed;
    }
  }

  require(actionKinds ==
              std::vector<LiveMidiDeviceRefreshActionKind>{
                  LiveMidiDeviceRefreshActionKind::Remove,
                  LiveMidiDeviceRefreshActionKind::Add,
                  LiveMidiDeviceRefreshActionKind::Add},
          "same-refresh reconciliation removes stale endpoints before additions");
  require(replacementId == canonicalId,
          "replacement endpoint retains the saved canonical CoreMIDI ID");
  require(!duplicateId.empty() && duplicateId != replacementId,
          "simultaneously live duplicate CoreMIDI endpoints remain distinct");
}

void testUtf16MidiNamesConvertToCanonicalUtf8() {
  require(utf16ToUtf8(u"MIDI \u00e9") == "MIDI \xC3\xA9",
          "BMP MIDI device names convert to canonical UTF-8");
  require(utf16ToUtf8(std::u16string{static_cast<char16_t>(0xD83C),
                                    static_cast<char16_t>(0xDFB9)}) ==
              "\xF0\x9F\x8E\xB9",
          "supplementary MIDI name characters combine surrogate pairs");
}

void testUtf16MidiNamesReplaceMalformedSurrogates() {
  const std::u16string malformed{static_cast<char16_t>(0xD83C), u'A',
                                 static_cast<char16_t>(0xDFB9)};
  require(utf16ToUtf8(malformed) == "\xEF\xBF\xBD"
                                      "A"
                                      "\xEF\xBF\xBD",
          "unpaired MIDI name surrogates become replacement characters");
}

} // namespace

int main() {
  testNoteOnOffAndVelocityZero();
  testControlChangeNormalization();
  testRunningStatusAcrossPacketsAndRealtimeBytes();
  testMultipleMessagesAndStatusReplacement();
  testNewStatusAbandonsIncompleteMessageWithoutGettingStuck();
  testRealtimeCanAppearInsideAChannelMessage();
  testSystemExclusiveAndCommonMessagesClearRunningStatus();
  testSystemMessagesSpanPacketsAndMalformedSysexRecovers();
  testIgnoredChannelMessagesConsumeCorrectLengths();
  testResetAndDeviceSwitchClearPartialState();
  testMalformedBytesRecoverAtNextValidStatus();
  testBackendQueuesNativePacketsUntilMainThreadPump();
  testBackendCanPublishNativePacketsImmediately();
  testBackendIsolatesParsersAndDropsPacketsAfterDisconnect();
  testBackendCloseRevokesQueuedNativeWork();
  testBackendOverflowForcesAReleaseBoundary();
  testBackendAcceptsConcurrentNativeProducers();
  testNativeCallbackLifetimeWaitsForActiveLease();
  testNativeCallbackLifetimeRejectsDelayedEntryAfterClose();
  testNativeCallbackLifetimeNeverReusesStaleTokens();
  testBackendPublishesConnectBeforeSynchronousActivationPacket();
  testBackendRollsBackFailedActivationBeforeLaterPackets();
  testBackendQueuesDisconnectBehindPendingActivation();
  testLiveMidiDeviceIdsStayUniqueAcrossIdenticalDeviceReadd();
  testLiveMidiRefreshReleasesReplacementBeforeNewClaims();
  testUtf16MidiNamesConvertToCanonicalUtf8();
  testUtf16MidiNamesReplaceMalformedSurrogates();
  return 0;
}
