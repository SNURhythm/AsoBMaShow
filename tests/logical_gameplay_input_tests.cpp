#include "input/LogicalGameplayInputAdapter.h"
#include "input/InputBindingResolver.h"
#include "input/InputNormalizer.h"
#include "input/InputProfile.h"

#include <SDL2/SDL_scancode.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

static_assert(!std::is_move_constructible_v<LogicalGameplayInputPipeline>);
static_assert(!std::is_copy_constructible_v<LogicalGameplayInputPipeline>);

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

void testScratchReversalFallsBackToOlderHeldDirection() {
  RecordingControl control;
  LogicalGameplayInputAdapter adapter(control, {});

  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::ScratchClockwise, true),
  });
  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::ScratchCounterClockwise,
                 true),
  });
  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::ScratchCounterClockwise,
                 false, 0, 0.0F),
  });
  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::ScratchClockwise, false, 0,
                 0.0F),
  });

  require(
      control.calls ==
          std::vector<ControlCall>{
              {.kind = ControlCall::Kind::Press, .lane = 7},
              {.kind = ControlCall::Kind::Release, .lane = 7, .backSpin = true},
              {.kind = ControlCall::Kind::Press, .lane = 7},
              {.kind = ControlCall::Kind::Release, .lane = 7, .backSpin = true},
              {.kind = ControlCall::Kind::Press, .lane = 7},
              {.kind = ControlCall::Kind::Release,
               .lane = 7,
               .backSpin = false},
          },
      "releasing the newest scratch direction reactivates the older held "
      "direction before its final release");
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

input::PhysicalInputEvent controlEvent(const input::PhysicalControl &control,
                                       bool pressed) {
  return {.control = control,
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

void testDirectKeyboardPolicyDoesNotReplayQueuedRegistryTap() {
  RecordingControl control;
  const InputProfile profile = makeDefaultInputProfile();
  LogicalGameplayInputPipeline pipeline(control, profile,
                                        makeGameplayInputScopes(7), {},
                                        {.acceptKeyboardFromRegistry = false});

  pipeline.consumeDirectKeyboard(SDL_SCANCODE_S, true);
  pipeline.consumeDirectKeyboard(SDL_SCANCODE_S, false);
  pipeline.consumeRegistryEvent(keyEvent(SDL_SCANCODE_S, true));
  pipeline.consumeRegistryEvent(keyEvent(SDL_SCANCODE_S, false));

  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 0},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 0,
                   .backSpin = false},
              },
          "a gated direct keyboard tap is not replayed by the registry queue");
}

void testDirectKeyboardPolicyRejectsViewConsumedRegistryKeys() {
  RecordingControl control;
  LogicalGameplayInputPipeline pipeline(control, makeDefaultInputProfile(),
                                        makeGameplayInputScopes(7), {},
                                        {.acceptKeyboardFromRegistry = false});

  pipeline.consumeRegistryEvent(keyEvent(SDL_SCANCODE_S, true));
  pipeline.consumeRegistryEvent(keyEvent(SDL_SCANCODE_S, false));

  require(control.calls.empty(),
          "a keyboard event consumed by the settings UI never reaches the "
          "preview through the registry");
}

void testDirectKeyboardPolicyStillAcceptsRegistryControllers() {
  RecordingControl control;
  const InputProfile profile{
      .bindings = {
          {.id = "preview-controller",
           .scope = {1, 7},
           .action = {input::LogicalActionKind::Lane, 2},
           .control = {.deviceId = "pad:preview",
                       .deviceClass = input::DeviceClass::GameController,
                       .kind = input::ControlKind::Button,
                       .index = 3,
                       .direction = input::ControlDirection::Any}}}};
  LogicalGameplayInputPipeline pipeline(control, profile,
                                        makeGameplayInputScopes(7), {},
                                        {.acceptKeyboardFromRegistry = false});
  const input::PhysicalControl button{
      .deviceId = "pad:preview",
      .deviceClass = input::DeviceClass::GameController,
      .kind = input::ControlKind::Button,
      .index = 3,
      .direction = input::ControlDirection::Any};

  pipeline.consumeRegistryEvent(
      {.control = button, .rawValue = 1.0, .normalizedValue = 1.0F});
  pipeline.consumeRegistryEvent(
      {.control = button, .rawValue = 0.0, .normalizedValue = 0.0F});

  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 2},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 2,
                   .backSpin = false},
              },
          "excluding registry keyboard events retains controller input");
}

void testDirectionalScratchDisconnectFallsBackThenResets() {
  RecordingControl control;
  const auto clockwiseControl =
      input::PhysicalControl{.deviceId = "pad:clockwise",
                             .deviceClass = input::DeviceClass::GameController,
                             .kind = input::ControlKind::Button,
                             .index = 1,
                             .direction = input::ControlDirection::Any};
  const auto counterClockwiseControl =
      input::PhysicalControl{.deviceId = "pad:counter-clockwise",
                             .deviceClass = input::DeviceClass::GameController,
                             .kind = input::ControlKind::Button,
                             .index = 2,
                             .direction = input::ControlDirection::Any};
  const InputProfile profile{
      .bindings = {
          {.id = "clockwise",
           .scope = {1, 7},
           .action = {input::LogicalActionKind::ScratchClockwise},
           .control = clockwiseControl},
          {.id = "counter-clockwise",
           .scope = {1, 7},
           .action = {input::LogicalActionKind::ScratchCounterClockwise},
           .control = counterClockwiseControl}}};
  LogicalGameplayInputPipeline pipeline(control, profile,
                                        makeGameplayInputScopes(7));
  pipeline.consumeRegistryEvent(controlEvent(clockwiseControl, true));
  pipeline.consumeRegistryEvent(controlEvent(counterClockwiseControl, true));
  control.calls.clear();
  pipeline.disconnectDevice("pad:counter-clockwise");
  require(
      control.calls ==
          std::vector<ControlCall>{
              {.kind = ControlCall::Kind::Release, .lane = 7, .backSpin = true},
              {.kind = ControlCall::Kind::Press, .lane = 7}},
      "disconnecting the newest scratch direction reactivates the older "
      "device hold");
  pipeline.reset();
  require(
      control.calls ==
          std::vector<ControlCall>{
              {.kind = ControlCall::Kind::Release, .lane = 7, .backSpin = true},
              {.kind = ControlCall::Kind::Press, .lane = 7},
              {.kind = ControlCall::Kind::Release,
               .lane = 7,
               .backSpin = false}},
      "pipeline reset releases the final effective scratch hold once");
}

void testDirectionalScratchResetClearsBothDirectionsAtomically() {
  const auto runReset = [](bool clockwisePressedFirst) {
    RecordingControl control;
    const input::PhysicalControl clockwiseControl{
        .deviceId = "pad:reset",
        .deviceClass = input::DeviceClass::GameController,
        .kind = input::ControlKind::Button,
        .index = 1,
        .direction = input::ControlDirection::Any};
    const input::PhysicalControl counterClockwiseControl{
        .deviceId = "pad:reset",
        .deviceClass = input::DeviceClass::GameController,
        .kind = input::ControlKind::Button,
        .index = 2,
        .direction = input::ControlDirection::Any};
    const InputProfile profile{
        .bindings = {
            {.id = "clockwise",
             .scope = {1, 7},
             .action = {input::LogicalActionKind::ScratchClockwise},
             .control = clockwiseControl},
            {.id = "counter-clockwise",
             .scope = {1, 7},
             .action = {input::LogicalActionKind::ScratchCounterClockwise},
             .control = counterClockwiseControl}}};
    LogicalGameplayInputPipeline pipeline(control, profile,
                                          makeGameplayInputScopes(7));

    const auto &first =
        clockwisePressedFirst ? clockwiseControl : counterClockwiseControl;
    const auto &second =
        clockwisePressedFirst ? counterClockwiseControl : clockwiseControl;
    pipeline.consumeRegistryEvent(controlEvent(first, true));
    pipeline.consumeRegistryEvent(controlEvent(second, true));
    control.calls.clear();

    pipeline.reset();
    const auto resetCalls = control.calls;
    control.calls.clear();
    pipeline.reset();
    require(control.calls.empty(), "an atomic scratch reset is idempotent");
    return resetCalls;
  };

  const std::vector expected{ControlCall{
      .kind = ControlCall::Kind::Release, .lane = 7, .backSpin = false}};
  require(runReset(true) == expected,
          "reset atomically clears clockwise then counter-clockwise holds");
  require(runReset(false) == expected,
          "reset atomically clears counter-clockwise then clockwise holds");
}

void testSameDeviceScratchDisconnectClearsBothDirectionsAtomically() {
  RecordingControl control;
  const input::PhysicalControl clockwiseControl{
      .deviceId = "pad:shared",
      .deviceClass = input::DeviceClass::GameController,
      .kind = input::ControlKind::Button,
      .index = 1,
      .direction = input::ControlDirection::Any};
  const input::PhysicalControl counterClockwiseControl{
      .deviceId = "pad:shared",
      .deviceClass = input::DeviceClass::GameController,
      .kind = input::ControlKind::Button,
      .index = 2,
      .direction = input::ControlDirection::Any};
  const InputProfile profile{
      .bindings = {
          {.id = "clockwise",
           .scope = {1, 7},
           .action = {input::LogicalActionKind::ScratchClockwise},
           .control = clockwiseControl},
          {.id = "counter-clockwise",
           .scope = {1, 7},
           .action = {input::LogicalActionKind::ScratchCounterClockwise},
           .control = counterClockwiseControl}}};
  LogicalGameplayInputPipeline pipeline(control, profile,
                                        makeGameplayInputScopes(7));

  pipeline.consumeRegistryEvent(controlEvent(counterClockwiseControl, true));
  pipeline.consumeRegistryEvent(controlEvent(clockwiseControl, true));
  control.calls.clear();
  pipeline.disconnectDevice("pad:shared");

  require(control.calls ==
              std::vector<ControlCall>{{.kind = ControlCall::Kind::Release,
                                        .lane = 7,
                                        .backSpin = false}},
          "same-device disconnect clears both scratch directions without a "
          "synthetic opposite press");
  control.calls.clear();
  pipeline.disconnectDevice("pad:shared");
  require(control.calls.empty(),
          "same-device scratch disconnect is idempotent");
}

void testScratchDisconnectPreservesIndependentDirectionReferences() {
  RecordingControl control;
  const input::PhysicalControl sharedClockwiseControl{
      .deviceId = "pad:shared",
      .deviceClass = input::DeviceClass::GameController,
      .kind = input::ControlKind::Button,
      .index = 1,
      .direction = input::ControlDirection::Any};
  const input::PhysicalControl survivingClockwiseControl{
      .deviceId = "pad:survivor",
      .deviceClass = input::DeviceClass::GameController,
      .kind = input::ControlKind::Button,
      .index = 3,
      .direction = input::ControlDirection::Any};
  const input::PhysicalControl sharedCounterClockwiseControl{
      .deviceId = "pad:shared",
      .deviceClass = input::DeviceClass::GameController,
      .kind = input::ControlKind::Button,
      .index = 2,
      .direction = input::ControlDirection::Any};
  const InputProfile profile{
      .bindings = {
          {.id = "clockwise-shared",
           .scope = {1, 7},
           .action = {input::LogicalActionKind::ScratchClockwise},
           .control = sharedClockwiseControl},
          {.id = "clockwise-survivor",
           .scope = {1, 7},
           .action = {input::LogicalActionKind::ScratchClockwise},
           .control = survivingClockwiseControl},
          {.id = "counter-clockwise-shared",
           .scope = {1, 7},
           .action = {input::LogicalActionKind::ScratchCounterClockwise},
           .control = sharedCounterClockwiseControl}}};
  LogicalGameplayInputPipeline pipeline(control, profile,
                                        makeGameplayInputScopes(7));

  pipeline.consumeRegistryEvent(controlEvent(sharedClockwiseControl, true));
  pipeline.consumeRegistryEvent(controlEvent(survivingClockwiseControl, true));
  pipeline.consumeRegistryEvent(
      controlEvent(sharedCounterClockwiseControl, true));
  control.calls.clear();
  pipeline.disconnectDevice("pad:shared");

  require(
      control.calls ==
          std::vector<ControlCall>{
              {.kind = ControlCall::Kind::Release, .lane = 7, .backSpin = true},
              {.kind = ControlCall::Kind::Press, .lane = 7}},
      "disconnect falls back to an independently referenced scratch direction");
  pipeline.disconnectDevice("pad:survivor");
  require(
      control.calls ==
          std::vector<ControlCall>{
              {.kind = ControlCall::Kind::Release, .lane = 7, .backSpin = true},
              {.kind = ControlCall::Kind::Press, .lane = 7},
              {.kind = ControlCall::Kind::Release,
               .lane = 7,
               .backSpin = false}},
      "the independently referenced scratch direction releases with its "
      "final device");
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

  const auto withFallback =
      makeGameplayInputProfileWithEscapeFallback(profile, activeScopes);
  require(withFallback.bindings.size() == profile.bindings.size(),
          "an explicit active Escape pause binding is not duplicated");
}

void testEscapeFallbackRunsInTheOrderedLogicalPipeline() {
  RecordingControl control;
  const auto scopes = makeGameplayInputScopes(7);
  const InputProfile profile = makeGameplayInputProfileWithEscapeFallback(
      makeDefaultInputProfile(), scopes);
  std::vector<input::LogicalInputTransition> commands;
  std::size_t controlCallsAtPause = 0;
  LogicalGameplayInputPipeline pipeline(
      control, profile, scopes, [&](const auto &command) {
        commands.push_back(command);
        if (command.pressed &&
            command.action.kind == input::LogicalActionKind::Pause) {
          controlCallsAtPause = control.calls.size();
        }
      });

  pipeline.consumeRegistryEvent(keyEvent(SDL_SCANCODE_S, true));
  pipeline.consumeRegistryEvent(keyEvent(SDL_SCANCODE_ESCAPE, true));

  require(control.calls ==
                  std::vector<ControlCall>{
                      {.kind = ControlCall::Kind::Press, .lane = 0}} &&
              commands.size() == 1 && commands.front().pressed &&
              commands.front().action.kind == input::LogicalActionKind::Pause &&
              controlCallsAtPause == 1,
          "the lane edge is applied before the queued Escape pause fallback");
}

} // namespace

int main() {
  testLaneTransitionsPreserveDpLaneNumbers();
  testGameplayScopesEnableBothPlayersOnlyForDp();
  testScratchReversalAndLateReleaseOrdering();
  testScratchReversalFallsBackToOlderHeldDirection();
  testSecondPlayerScratchUsesLaneFifteen();
  testResolverOrderedScratchReversalUsesBackspinRelease();
  testCommandsNeverReachRhythmControl();
  testResetReleasesOnlyHeldLogicalLanes();
  testDefaultProfileRoutesThroughResolverAndAdapter();
  testDirectKeyboardPolicyDoesNotReplayQueuedRegistryTap();
  testDirectKeyboardPolicyRejectsViewConsumedRegistryKeys();
  testDirectKeyboardPolicyStillAcceptsRegistryControllers();
  testDirectionalScratchDisconnectFallsBackThenResets();
  testSameDeviceScratchDisconnectClearsBothDirectionsAtomically();
  testDirectionalScratchResetClearsBothDirectionsAtomically();
  testScratchDisconnectPreservesIndependentDirectionReferences();
  testDefaultDpProfileActivatesSecondPlayerScope();
  testLegacyKeyboardCallbacksPreservePhysicalScancodes();
  testLaneAndDirectionalScratchShareOneEffectiveLaneHold();
  testSameLaneAcrossScopesUsesReferenceSemantics();
  testScratchReversalKeepsAnOverlappingDigitalHoldCoherent();
  testEscapeFallbackYieldsToAnActiveLogicalPauseBinding();
  testEscapeFallbackRunsInTheOrderedLogicalPipeline();
  return 0;
}
