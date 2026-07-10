#include "input/MidiMessageParser.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

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
  return 0;
}
