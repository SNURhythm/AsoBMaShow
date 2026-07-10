#include "input/LogicalGameplayInputAdapter.h"
#include "input/InputBindingResolver.h"
#include "input/InputNormalizer.h"
#include "input/InputProfile.h"

#include <SDL2/SDL_scancode.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ControlCall {
  enum class Kind { Press, Release } kind = Kind::Press;
  int lane = 0;
  bool backSpin = false;
  auto operator<=>(const ControlCall &) const = default;
};

class RecordingControl final : public IRhythmControl {
public:
  bms_parser::Note *pressLane(int mainLane, int compensateLane,
                              double inputDelay) override {
    (void)compensateLane;
    (void)inputDelay;
    calls.push_back({.kind = ControlCall::Kind::Press, .lane = mainLane});
    return nullptr;
  }

  bms_parser::Note *pressLane(int lane, double inputDelay) override {
    (void)inputDelay;
    calls.push_back({.kind = ControlCall::Kind::Press, .lane = lane});
    return nullptr;
  }

  bms_parser::Note *releaseLane(int lane, double inputDelay,
                                bool isBackSpin) override {
    (void)inputDelay;
    calls.push_back({.kind = ControlCall::Kind::Release,
                     .lane = lane,
                     .backSpin = isBackSpin});
    return nullptr;
  }

  std::vector<ControlCall> calls;
};

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

input::LogicalInputTransition transition(input::InputScope scope,
                                         input::LogicalActionKind kind,
                                         bool pressed, int lane = 0,
                                         float value = 1.0F) {
  return {.scope = scope,
          .action = {.kind = kind, .lane = lane},
          .pressed = pressed,
          .value = value};
}

void testLaneTransitionsPreserveDpLaneNumbers() {
  RecordingControl control;
  std::vector<input::LogicalInputTransition> commands;
  LogicalGameplayInputAdapter adapter(
      control, [&](const auto &value) { commands.push_back(value); });

  const std::vector transitions = {
      transition({1, 14}, input::LogicalActionKind::Lane, true, 0),
      transition({2, 14}, input::LogicalActionKind::Lane, true, 15),
      transition({1, 14}, input::LogicalActionKind::Lane, false, 0, 0.0F),
      transition({2, 14}, input::LogicalActionKind::Lane, false, 15, 0.0F),
  };
  adapter.apply(transitions);

  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 0},
                  {.kind = ControlCall::Kind::Press, .lane = 15},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 0,
                   .backSpin = false},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 15,
                   .backSpin = false},
              },
          "lane transitions preserve the full DP 0-15 lane space");
  require(commands.empty(), "lane transitions never leak to commands");
}

void testGameplayScopesEnableBothPlayersOnlyForDp() {
  require(makeGameplayInputScopes(7) ==
              std::vector<input::InputScope>{{.player = 1, .keyMode = 7}},
          "single-player modes activate only player one");
  require(makeGameplayInputScopes(14) ==
              std::vector<input::InputScope>{{.player = 1, .keyMode = 14},
                                             {.player = 2, .keyMode = 14}},
          "double-play modes activate both existing player scopes");
}

void testScratchReversalAndLateReleaseOrdering() {
  RecordingControl control;
  LogicalGameplayInputAdapter adapter(control, {});

  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::ScratchClockwise, true),
      transition({1, 7}, input::LogicalActionKind::ScratchCounterClockwise,
                 true),
      transition({1, 7}, input::LogicalActionKind::ScratchClockwise, false, 0,
                 0.0F),
      transition({1, 7}, input::LogicalActionKind::ScratchCounterClockwise,
                 false, 0, 0.0F),
  });

  require(
      control.calls ==
          std::vector<ControlCall>{
              {.kind = ControlCall::Kind::Press, .lane = 7},
              {.kind = ControlCall::Kind::Release, .lane = 7, .backSpin = true},
              {.kind = ControlCall::Kind::Press, .lane = 7},
              {.kind = ControlCall::Kind::Release,
               .lane = 7,
               .backSpin = false},
          },
      "scratch reversal releases the old direction before the new press, "
      "ignores its late release, and normally releases the active side");
}

void testSecondPlayerScratchUsesLaneFifteen() {
  RecordingControl control;
  LogicalGameplayInputAdapter adapter(control, {});
  adapter.apply(std::vector{
      transition({2, 14}, input::LogicalActionKind::ScratchCounterClockwise,
                 true),
      transition({2, 14}, input::LogicalActionKind::ScratchCounterClockwise,
                 false, 0, 0.0F),
  });
  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 15},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 15,
                   .backSpin = false},
              },
          "player two scratch maps to the existing DP scratch lane");
}

void testResolverOrderedScratchReversalUsesBackspinRelease() {
  RecordingControl control;
  LogicalGameplayInputAdapter adapter(control, {});
  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::ScratchClockwise, true),
  });
  control.calls.clear();
  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::ScratchClockwise, false, 0,
                 0.0F),
      transition({1, 7}, input::LogicalActionKind::ScratchCounterClockwise,
                 true),
  });
  require(
      control.calls ==
          std::vector<ControlCall>{
              {.kind = ControlCall::Kind::Release, .lane = 7, .backSpin = true},
              {.kind = ControlCall::Kind::Press, .lane = 7},
          },
      "resolver release-first batches preserve scratch reversal semantics");
}

void testCommandsNeverReachRhythmControl() {
  RecordingControl control;
  std::vector<input::LogicalInputTransition> commands;
  LogicalGameplayInputAdapter adapter(
      control, [&](const auto &value) { commands.push_back(value); });
  const std::vector transitions = {
      transition({1, 7}, input::LogicalActionKind::Start, true),
      transition({1, 7}, input::LogicalActionKind::Pause, true),
      transition({1, 7}, input::LogicalActionKind::LaneCoverIncrease, false, 0,
                 0.0F),
  };
  adapter.apply(transitions);
  require(control.calls.empty(), "commands never invoke lane control");
  require(commands.size() == transitions.size(),
          "every command transition is forwarded");
  for (std::size_t index = 0; index < transitions.size(); ++index) {
    require(commands[index].scope == transitions[index].scope &&
                commands[index].action == transitions[index].action &&
                commands[index].pressed == transitions[index].pressed &&
                commands[index].value == transitions[index].value,
            "commands are forwarded unchanged and in source order");
  }
}

void testResetReleasesOnlyHeldLogicalLanes() {
  RecordingControl control;
  LogicalGameplayInputAdapter adapter(control, {});
  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::Lane, true, 3),
      transition({2, 14}, input::LogicalActionKind::ScratchClockwise, true),
  });
  control.calls.clear();
  adapter.reset();
  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Release,
                   .lane = 3,
                   .backSpin = false},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 15,
                   .backSpin = false},
              },
          "reset releases held lanes and scratch without backspin");
  control.calls.clear();
  adapter.reset();
  require(control.calls.empty(), "reset is idempotent");
}

input::PhysicalInputEvent keyEvent(SDL_Scancode scancode, bool pressed) {
  return {.control = {.deviceId = "keyboard",
                      .deviceClass = input::DeviceClass::Keyboard,
                      .kind = input::ControlKind::Key,
                      .index = static_cast<int>(scancode),
                      .direction = input::ControlDirection::Any},
          .rawValue = pressed ? 1.0 : 0.0,
          .normalizedValue = pressed ? 1.0F : 0.0F};
}

void testDefaultProfileRoutesThroughResolverAndAdapter() {
  RecordingControl control;
  LogicalGameplayInputAdapter adapter(control, {});
  const InputProfile profile = makeDefaultInputProfile();
  InputBindingResolver resolver(
      profile, makeGameplayInputScopes(7),
      {.onTransitions =
           [&](std::span<const input::LogicalInputTransition> transitions) {
             adapter.apply(transitions);
           }});

  resolver.consume(keyEvent(SDL_SCANCODE_S, true));
  resolver.consume(keyEvent(SDL_SCANCODE_S, false));
  resolver.consume(keyEvent(SDL_SCANCODE_LSHIFT, true));
  resolver.disconnectDevice("keyboard");

  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 0},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 0,
                   .backSpin = false},
                  {.kind = ControlCall::Kind::Press, .lane = 7},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 7,
                   .backSpin = false},
              },
          "current 7K defaults and disconnect releases pass through the new "
          "resolver-adapter path");
}

void testDefaultDpProfileActivatesSecondPlayerScope() {
  RecordingControl control;
  LogicalGameplayInputAdapter adapter(control, {});
  const InputProfile profile = makeDefaultInputProfile();
  InputBindingResolver resolver(
      profile, makeGameplayInputScopes(14),
      {.onTransitions =
           [&](std::span<const input::LogicalInputTransition> transitions) {
             adapter.apply(transitions);
           }});
  resolver.consume(keyEvent(SDL_SCANCODE_M, true));
  resolver.consume(keyEvent(SDL_SCANCODE_M, false));
  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 8},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 8,
                   .backSpin = false},
              },
          "14K player-two defaults remain lanes 8-15 through active scopes");
}

void testLegacyKeyboardCallbacksPreservePhysicalScancodes() {
  require(InputNormalizer::normalizeScancode(SDL_SCANCODE_S, ScanCode) ==
              SDL_SCANCODE_S,
          "legacy SDL scan-code callbacks stay physical-layout based");
  require(InputNormalizer::normalizeScancode(-1, ScanCode) ==
              SDL_SCANCODE_UNKNOWN,
          "invalid legacy scan codes fail closed");
}

void testLaneAndDirectionalScratchShareOneEffectiveLaneHold() {
  RecordingControl control;
  LogicalGameplayInputAdapter adapter(control, {});
  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::Lane, true, 7),
      transition({1, 7}, input::LogicalActionKind::ScratchClockwise, true),
      transition({1, 7}, input::LogicalActionKind::Lane, false, 7, 0.0F),
      transition({1, 7}, input::LogicalActionKind::ScratchClockwise, false, 0,
                 0.0F),
  });
  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 7},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 7,
                   .backSpin = false},
              },
          "digital and directional scratch bindings reference one lane hold");
}

void testSameLaneAcrossScopesUsesReferenceSemantics() {
  RecordingControl control;
  LogicalGameplayInputAdapter adapter(control, {});
  adapter.apply(std::vector{
      transition({1, 14}, input::LogicalActionKind::Lane, true, 3),
      transition({2, 14}, input::LogicalActionKind::Lane, true, 3),
      transition({1, 14}, input::LogicalActionKind::Lane, false, 3, 0.0F),
  });
  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 3},
              },
          "releasing one logical scope preserves another scope's lane hold");
  adapter.apply(std::vector{
      transition({2, 14}, input::LogicalActionKind::Lane, false, 3, 0.0F),
  });
  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 3},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 3,
                   .backSpin = false},
              },
          "logical scopes sharing a lane release only after the final hold");
}

void testScratchReversalKeepsAnOverlappingDigitalHoldCoherent() {
  RecordingControl control;
  LogicalGameplayInputAdapter adapter(control, {});
  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::Lane, true, 7),
      transition({1, 7}, input::LogicalActionKind::ScratchClockwise, true),
  });
  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::ScratchClockwise, false, 0,
                 0.0F),
      transition({1, 7}, input::LogicalActionKind::ScratchCounterClockwise,
                 true),
  });
  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::Lane, false, 7, 0.0F),
      transition({1, 7}, input::LogicalActionKind::ScratchCounterClockwise,
                 false, 0, 0.0F),
  });
  require(
      control.calls ==
          std::vector<ControlCall>{
              {.kind = ControlCall::Kind::Press, .lane = 7},
              {.kind = ControlCall::Kind::Release, .lane = 7, .backSpin = true},
              {.kind = ControlCall::Kind::Press, .lane = 7},
              {.kind = ControlCall::Kind::Release,
               .lane = 7,
               .backSpin = false},
          },
      "scratch reversal remains ordered while a digital scratch hold overlaps");
}

void testEscapeFallbackYieldsToAnActiveLogicalPauseBinding() {
  InputProfile profile = makeDefaultInputProfile();
  profile.bindings.push_back({
      .id = "pause-escape",
      .scope = {1, 7},
      .action = {input::LogicalActionKind::Pause, 0},
      .control = {.deviceId = "keyboard",
                  .deviceClass = input::DeviceClass::Keyboard,
                  .kind = input::ControlKind::Key,
                  .index = SDL_SCANCODE_ESCAPE,
                  .direction = input::ControlDirection::Any},
  });
  const auto activeScopes = makeGameplayInputScopes(7);
  require(hasActiveKeyboardActionBinding(profile, activeScopes,
                                         SDL_SCANCODE_ESCAPE,
                                         input::LogicalActionKind::Pause),
          "the Escape fallback detects an active logical Pause binding");
  const auto inactiveScopes = makeGameplayInputScopes(8);
  require(!hasActiveKeyboardActionBinding(profile, inactiveScopes,
                                          SDL_SCANCODE_ESCAPE,
                                          input::LogicalActionKind::Pause),
          "inactive key-mode bindings do not suppress the Escape fallback");
}

} // namespace

int main() {
  testLaneTransitionsPreserveDpLaneNumbers();
  testGameplayScopesEnableBothPlayersOnlyForDp();
  testScratchReversalAndLateReleaseOrdering();
  testSecondPlayerScratchUsesLaneFifteen();
  testResolverOrderedScratchReversalUsesBackspinRelease();
  testCommandsNeverReachRhythmControl();
  testResetReleasesOnlyHeldLogicalLanes();
  testDefaultProfileRoutesThroughResolverAndAdapter();
  testDefaultDpProfileActivatesSecondPlayerScope();
  testLegacyKeyboardCallbacksPreservePhysicalScancodes();
  testLaneAndDirectionalScratchShareOneEffectiveLaneHold();
  testSameLaneAcrossScopesUsesReferenceSemantics();
  testScratchReversalKeepsAnOverlappingDigitalHoldCoherent();
  testEscapeFallbackYieldsToAnActiveLogicalPauseBinding();
  return 0;
}
