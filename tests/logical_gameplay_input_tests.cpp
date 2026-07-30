#include "input/LogicalGameplayInputAdapter.h"
#include "input/RealtimePhysicalInputRouter.h"
#include "input/InputBindingResolver.h"
#include "input/InputNormalizer.h"
#include "input/InputProfile.h"
#include "scene/play/RhythmState.h"

#include <SDL2/SDL_scancode.h>

#include <limits>
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

void testAppliedObserverSeesOneCanonicalReplayStream() {
  RecordingControl control;
  std::vector<LogicalGameplayInputAdapter::AppliedTransition> applied;
  LogicalGameplayInputAdapter adapter(
      control, {}, [&](const auto &value) { applied.push_back(value); });

  adapter.apply(std::vector{
      transition({1, 7}, input::LogicalActionKind::Lane, true, 2),
      transition({1, 7}, input::LogicalActionKind::Lane, true, 2),
      transition({1, 7}, input::LogicalActionKind::Lane, false, 2, 0.0F),
      transition({1, 7}, input::LogicalActionKind::ScratchClockwise, true),
      transition({1, 7}, input::LogicalActionKind::ScratchCounterClockwise,
                 true),
      transition({1, 7}, input::LogicalActionKind::ScratchClockwise, false, 0,
                 0.0F),
      transition({1, 7}, input::LogicalActionKind::ScratchCounterClockwise,
                 false, 0, 0.0F),
      transition({1, 7}, input::LogicalActionKind::Start, true),
      transition({1, 7}, input::LogicalActionKind::Start, false, 0, 0.0F),
      transition({1, 7}, input::LogicalActionKind::Select, true),
      transition({1, 7}, input::LogicalActionKind::Select, false, 0, 0.0F),
      transition({1, 7}, input::LogicalActionKind::Pause, true),
  });

  require(applied.size() == 10,
          "observer sees effective lane, scratch, Start, and Select only");
  require(applied[0].control ==
                  replay::LogicalControl{.kind =
                                             replay::LogicalControlKind::Lane,
                                         .player = 1,
                                         .lane = 2} &&
              applied[0].pressed && !applied[0].replayOnly,
          "lane observer uses the canonical replay control");
  require(applied[2].control.kind ==
                  replay::LogicalControlKind::ScratchClockwise &&
              applied[2].pressed &&
              applied[3].control.kind ==
                  replay::LogicalControlKind::ScratchClockwise &&
              !applied[3].pressed &&
              applied[4].control.kind ==
                  replay::LogicalControlKind::ScratchCounterClockwise &&
              applied[4].pressed,
          "scratch reversal is an ordered release and press in replay space");
  require(applied[6].control.kind == replay::LogicalControlKind::Start &&
              applied[6].pressed &&
              applied[7].control.kind == replay::LogicalControlKind::Start &&
              !applied[7].pressed &&
              applied[8].control.kind == replay::LogicalControlKind::Select &&
              applied[8].pressed &&
              applied[9].control.kind == replay::LogicalControlKind::Select &&
              !applied[9].pressed,
          "Start and Select share the replay observer boundary");
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

void testTouchAndHardwareShareOneLaneOwnershipBoundary() {
  RecordingControl control;
  std::vector<LogicalGameplayInputAdapter::AppliedTransition> applied;
  LogicalGameplayInputPipeline pipeline(
      control, makeDefaultInputProfile(), makeGameplayInputScopes(7), {}, {},
      [&](const auto &value) { applied.push_back(value); });
  const auto touchDown =
      transition({1, 7}, input::LogicalActionKind::Lane, true, 0);
  const auto touchUp =
      transition({1, 7}, input::LogicalActionKind::Lane, false, 0, 0.0F);

  pipeline.consumeDirectKeyboard(SDL_SCANCODE_S, true);
  (void)pipeline.consumeTouchTransition(touchDown);
  (void)pipeline.consumeTouchTransition(touchUp);
  pipeline.consumeDirectKeyboard(SDL_SCANCODE_S, false);

  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 0},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 0,
                   .backSpin = false}},
          "touch release cannot end an overlapping hardware lane hold");
  require(applied.size() == 2 && applied.front().pressed &&
              !applied.back().pressed,
          "overlapping touch and hardware owners emit one replay edge pair");

  control.calls.clear();
  applied.clear();
  (void)pipeline.consumeTouchTransition(touchDown);
  pipeline.consumeDirectKeyboard(SDL_SCANCODE_S, true);
  pipeline.consumeDirectKeyboard(SDL_SCANCODE_S, false);
  (void)pipeline.consumeTouchTransition(touchUp);

  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 0},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 0,
                   .backSpin = false}},
          "hardware release cannot end an overlapping touch lane hold");
  require(applied.size() == 2 && applied.front().pressed &&
              !applied.back().pressed,
          "touch-first overlap still emits one replay edge pair");
}

void testTouchScratchAndDigitalScratchShareOneOwnershipBoundary() {
  RecordingControl control;
  std::vector<LogicalGameplayInputAdapter::AppliedTransition> applied;
  LogicalGameplayInputAdapter adapter(
      control, {}, [&](const auto &value) { applied.push_back(value); });
  const auto digitalDown =
      transition({1, 7}, input::LogicalActionKind::Lane, true, 7);
  const auto digitalUp =
      transition({1, 7}, input::LogicalActionKind::Lane, false, 7, 0.0F);
  const auto touchDown = transition(
      {1, 7}, input::LogicalActionKind::ScratchClockwise, true);
  const auto touchUp = transition(
      {1, 7}, input::LogicalActionKind::ScratchClockwise, false, 0, 0.0F);

  adapter.apply(std::span(&digitalDown, 1));
  (void)adapter.applyTouch(touchDown);
  (void)adapter.applyTouch(touchUp);
  adapter.apply(std::span(&digitalUp, 1));

  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 7},
                  {.kind = ControlCall::Kind::Release,
                   .lane = 7,
                   .backSpin = false}},
          "touch scratch release preserves an overlapping digital hold");
  require(applied.size() == 2 && applied.front().pressed &&
              !applied.back().pressed &&
              applied.front().control.kind ==
                  replay::LogicalControlKind::ScratchClockwise &&
              applied.back().control == applied.front().control,
          "overlapping scratch owners emit one canonical replay edge pair");
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

void testRealtimePhysicalInputPreservesNativeTimestamp() {
  InputProfile profile;
  profile.bindings.push_back(
      {.id = "native-key-lane",
       .scope = {.player = 1, .keyMode = 7},
       .action = {.kind = input::LogicalActionKind::Lane, .lane = 3},
       .control = {.deviceId = "keyboard",
                   .deviceClass = input::DeviceClass::Keyboard,
                   .kind = input::ControlKind::Key,
                   .index = SDL_SCANCODE_F}});
  std::vector<input::RealtimePhysicalInputTransition> output;
  input::RealtimePhysicalInputRouter router(
      profile, makeGameplayInputScopes(7),
      [&](const auto &transition) {
        output.push_back(transition);
        return true;
      });
  router.setGameplayEnabled(true, 9000);

  input::PhysicalInputEvent down{
      .control = {.deviceId = "keyboard",
                  .deviceClass = input::DeviceClass::Keyboard,
                  .kind = input::ControlKind::Key,
                  .index = SDL_SCANCODE_F},
      .rawValue = 1.0,
      .normalizedValue = 1.0F};
  input::PhysicalInputEvent up = down;
  up.rawValue = 0.0;
  up.normalizedValue = 0.0F;
  router.consume(down, 1234567);
  router.consume(up, 1234999);

  require(output.size() == 2 &&
              output[0].type ==
                  input::RealtimePhysicalInputTransitionType::Press &&
              output[0].lane == 3 &&
              output[0].hasReplayControl &&
              output[0].replayControl ==
                  replay::LogicalControl{.kind =
                                             replay::LogicalControlKind::Lane,
                                         .player = 1,
                                         .lane = 3} &&
              output[0].steadyTimestampMicros == 1234567 &&
              output[1].type ==
                  input::RealtimePhysicalInputTransitionType::Release &&
              output[1].lane == 3 &&
              output[1].hasReplayControl &&
              output[1].replayControl == output[0].replayControl &&
              output[1].steadyTimestampMicros == 1234999,
          "native physical edges reach realtime lanes with their source "
          "timestamps");
}

void testNonStockKeyModesCaptureBmsChannelReplayControls() {
  struct Case {
    int keyMode;
    int physicalLane;
  };
  for (const auto [keyMode, lane] :
       {Case{4, 4}, Case{6, 6}, Case{8, 7}}) {
    InputProfile profile;
    profile.bindings.push_back(
        {.id = "custom-key-lane",
         .scope = {.player = 1, .keyMode = keyMode},
         .action = {.kind = input::LogicalActionKind::Lane, .lane = lane},
         .control = {.deviceId = "keyboard",
                     .deviceClass = input::DeviceClass::Keyboard,
                     .kind = input::ControlKind::Key,
                     .index = SDL_SCANCODE_D}});
    std::vector<input::RealtimePhysicalInputTransition> output;
    input::RealtimePhysicalInputRouter router(
        profile, makeGameplayInputScopes(keyMode),
        [&](const auto &transition) {
          output.push_back(transition);
          return true;
        });
    router.setGameplayEnabled(true, 9000);

    const auto down = keyEvent(SDL_SCANCODE_D, true);
    const auto up = keyEvent(SDL_SCANCODE_D, false);
    router.consume(down, 1234567);
    router.consume(up, 1234999);

    require(output.size() == 2 && output[0].lane == lane &&
                output[1].lane == lane && output[0].hasReplayControl &&
                output[1].hasReplayControl &&
                output[0].replayControl == replay::LogicalControl{
                    .kind = replay::LogicalControlKind::Lane,
                    .player = 1,
                    .lane = lane} &&
                output[1].replayControl == output[0].replayControl,
            "non-stock key modes capture their exact BMS channel lane in "
            "BRD metadata");
  }
}

void testArbitraryLaneInputDoesNotDependOnBrdControls() {
  struct Case {
    int keyMode;
    int physicalLane;
  };
  for (const auto [keyMode, lane] :
       {Case{1, 0}, Case{17, 31}, Case{65, 129}, Case{128, 1024}}) {
    InputProfile profile;
    profile.bindings.push_back(
        {.id = "custom-key-lane",
         .scope = {.player = 1, .keyMode = keyMode},
         .action = {.kind = input::LogicalActionKind::Lane, .lane = lane},
         .control = {.deviceId = "keyboard",
                     .deviceClass = input::DeviceClass::Keyboard,
                     .kind = input::ControlKind::Key,
                     .index = SDL_SCANCODE_D}});
    std::vector<input::RealtimePhysicalInputTransition> output;
    input::RealtimePhysicalInputRouter router(
        profile, makeGameplayInputScopes(keyMode),
        [&](const auto &transition) {
          output.push_back(transition);
          return true;
        });
    router.setGameplayEnabled(true, 9000);

    router.consume(keyEvent(SDL_SCANCODE_D, true), 1234567);
    router.consume(keyEvent(SDL_SCANCODE_D, false), 1234999);

    require(output.size() == 2 && output[0].lane == lane &&
                output[1].lane == lane && !output[0].hasReplayControl &&
                !output[1].hasReplayControl,
            "arbitrary physical lanes reach gameplay without requiring BRD "
            "control metadata");
  }
}

void testPhysicalTouchLaneDoesNotDependOnBrdControls() {
  RecordingControl control;
  std::vector<LogicalGameplayInputAdapter::AppliedTransition> applied;
  LogicalGameplayInputPipeline pipeline(
      control, makeDefaultInputProfile(), makeGameplayInputScopes(4), {}, {},
      [&](const auto &transition) { applied.push_back(transition); });

  (void)pipeline.consumePhysicalTouchLane(
      {.player = 1, .keyMode = 4}, 4, true, std::nullopt);
  (void)pipeline.consumePhysicalTouchLane(
      {.player = 1, .keyMode = 4}, 4, false, std::nullopt);

  require(control.calls ==
              std::vector<ControlCall>{
                  {.kind = ControlCall::Kind::Press, .lane = 4},
                  {.kind = ControlCall::Kind::Release, .lane = 4}},
          "physical touch lanes reach gameplay without a BRD layout");
  require(applied.size() == 2 && applied[0].physicalLane == 4 &&
              applied[1].physicalLane == 4 &&
              applied[0].hasReplayControl && applied[1].hasReplayControl &&
              applied[0].control == replay::LogicalControl{
                  .kind = replay::LogicalControlKind::Lane,
                  .player = 1,
                  .lane = 4} &&
              applied[1].control == applied[0].control,
          "physical touch lanes preserve non-stock BRD channel metadata");
}

void testRealtimeScratchReversalCarriesCanonicalDirections() {
  InputProfile profile;
  const input::PhysicalControl clockwise{
      .deviceId = "pad:scratch",
      .deviceClass = input::DeviceClass::GameController,
      .kind = input::ControlKind::Button,
      .index = 1};
  const input::PhysicalControl counterClockwise{
      .deviceId = "pad:scratch",
      .deviceClass = input::DeviceClass::GameController,
      .kind = input::ControlKind::Button,
      .index = 2};
  profile.bindings.push_back({.id = "scratch-cw",
                              .scope = {1, 7},
                              .action = {input::LogicalActionKind::
                                             ScratchClockwise},
                              .control = clockwise});
  profile.bindings.push_back({.id = "scratch-ccw",
                              .scope = {1, 7},
                              .action = {input::LogicalActionKind::
                                             ScratchCounterClockwise},
                              .control = counterClockwise});
  std::vector<input::RealtimePhysicalInputTransition> output;
  input::RealtimePhysicalInputRouter router(
      profile, makeGameplayInputScopes(7),
      [&](const auto &value) {
        output.push_back(value);
        return true;
      });
  router.setGameplayEnabled(true, 50);
  router.consume(controlEvent(clockwise, true), 100);
  router.consume(controlEvent(counterClockwise, true), 200);
  router.consume(controlEvent(clockwise, false), 250);
  router.consume(controlEvent(counterClockwise, false), 300);

  require(output.size() == 4 &&
              output[0].type ==
                  input::RealtimePhysicalInputTransitionType::Press &&
              output[0].replayControl.kind ==
                  replay::LogicalControlKind::ScratchClockwise &&
              output[1].type ==
                  input::RealtimePhysicalInputTransitionType::Release &&
              output[1].backSpin &&
              output[1].replayControl.kind ==
                  replay::LogicalControlKind::ScratchClockwise &&
              output[2].type ==
                  input::RealtimePhysicalInputTransitionType::Press &&
              output[2].replayControl.kind ==
                  replay::LogicalControlKind::ScratchCounterClockwise &&
              output[3].type ==
                  input::RealtimePhysicalInputTransitionType::Release &&
              output[3].replayControl.kind ==
                  replay::LogicalControlKind::ScratchCounterClockwise,
          "realtime scratch reversal keeps the exact ordered logical sides");
}

void testRealtimeStartProducesCommandAndReplayEdge() {
  InputProfile profile;
  const input::PhysicalControl start{
      .deviceId = "pad:start",
      .deviceClass = input::DeviceClass::GameController,
      .kind = input::ControlKind::Button,
      .index = 7};
  profile.bindings.push_back({.id = "start",
                              .scope = {1, 7},
                              .action = {input::LogicalActionKind::Start},
                              .control = start});
  std::vector<input::RealtimePhysicalInputTransition> output;
  input::RealtimePhysicalInputRouter router(
      profile, makeGameplayInputScopes(7),
      [&](const auto &value) {
        output.push_back(value);
        return true;
      });
  router.setGameplayEnabled(true, 50);
  router.consume(controlEvent(start, true), 100);
  router.consume(controlEvent(start, false), 200);

  require(output.size() == 4 &&
              output[0].type ==
                  input::RealtimePhysicalInputTransitionType::Command &&
              output[1].replayOnly && output[1].hasReplayControl &&
              output[1].replayControl.kind ==
                  replay::LogicalControlKind::Start &&
              output[1].type ==
                  input::RealtimePhysicalInputTransitionType::Press &&
              output[2].type ==
                  input::RealtimePhysicalInputTransitionType::Command &&
              output[3].replayOnly &&
              output[3].replayControl.kind ==
                  replay::LogicalControlKind::Start &&
              output[3].type ==
                  input::RealtimePhysicalInputTransitionType::Release,
          "Start remains a UI command and also enters the raw replay stream");
}

void testRealtimePhysicalInputPauseDefersReleasedLaneUntilResume() {
  InputProfile profile;
  profile.bindings.push_back(
      {.id = "native-controller-lane",
       .scope = {.player = 1, .keyMode = 7},
       .action = {.kind = input::LogicalActionKind::Lane, .lane = 5},
       .control = {.deviceId = "pad:one",
                   .deviceClass = input::DeviceClass::GameController,
                   .kind = input::ControlKind::Button,
                   .index = SDL_CONTROLLER_BUTTON_A}});
  std::vector<input::RealtimePhysicalInputTransition> output;
  input::RealtimePhysicalInputRouter router(
      profile, makeGameplayInputScopes(7),
      [&](const auto &transition) {
        output.push_back(transition);
        return true;
      });
  router.setGameplayEnabled(true, 100);
  router.consume(
      {.control = {.deviceId = "pad:one",
                   .deviceClass = input::DeviceClass::GameController,
                   .kind = input::ControlKind::Button,
                   .index = SDL_CONTROLLER_BUTTON_A},
       .rawValue = 1.0,
       .normalizedValue = 1.0F},
      200);
  router.setGameplayEnabled(false, 300);
  router.consume(
      {.control = {.deviceId = "pad:one",
                   .deviceClass = input::DeviceClass::GameController,
                   .kind = input::ControlKind::Button,
                   .index = SDL_CONTROLLER_BUTTON_A},
       .rawValue = 0.0,
       .normalizedValue = 0.0F},
      400);
  router.setGameplayEnabled(true, 500);

  require(output.size() == 2 &&
              output[0].type ==
                  input::RealtimePhysicalInputTransitionType::Press &&
              output[1].type ==
                  input::RealtimePhysicalInputTransitionType::Release &&
              output[1].lane == 5 &&
              output[1].steadyTimestampMicros == 500,
          "a release received while paused is deferred until resume");
}

void testArbitraryPhysicalLaneReleaseWhilePausedReconcilesOnResume() {
  InputProfile profile;
  profile.bindings.push_back(
      {.id = "custom-lane-above-legacy-capacity",
       .scope = {.player = 1, .keyMode = 130},
       .action = {.kind = input::LogicalActionKind::Lane, .lane = 129},
       .control = {.deviceId = "keyboard",
                   .deviceClass = input::DeviceClass::Keyboard,
                   .kind = input::ControlKind::Key,
                   .index = SDL_SCANCODE_D}});
  std::vector<input::RealtimePhysicalInputTransition> output;
  input::RealtimePhysicalInputRouter router(
      profile, makeGameplayInputScopes(130),
      [&](const auto &transition) {
        output.push_back(transition);
        return true;
      });
  router.setGameplayEnabled(true, 100);
  router.consume(keyEvent(SDL_SCANCODE_D, true), 200);
  router.setGameplayEnabled(false, 300);
  router.consume(keyEvent(SDL_SCANCODE_D, false), 400);
  router.setGameplayEnabled(true, 500);

  require(output.size() == 2 &&
              output.front().type ==
                  input::RealtimePhysicalInputTransitionType::Press &&
              output.back().type ==
                  input::RealtimePhysicalInputTransitionType::Release &&
              output.back().lane == 129 &&
              output.back().steadyTimestampMicros == 500,
          "arbitrary physical lane state reconciles across pause and resume");
}

void testExtremeImportedLaneUsesSparsePauseState() {
  constexpr int kExtremeLane = std::numeric_limits<int>::max();
  InputProfile profile;
  profile.bindings.push_back(
      {.id = "extreme-imported-lane",
       .scope = {.player = 1, .keyMode = 7},
       .action = {.kind = input::LogicalActionKind::Lane,
                  .lane = kExtremeLane},
       .control = {.deviceId = "keyboard",
                   .deviceClass = input::DeviceClass::Keyboard,
                   .kind = input::ControlKind::Key,
                   .index = SDL_SCANCODE_D}});
  std::vector<input::RealtimePhysicalInputTransition> output;
  input::RealtimePhysicalInputRouter router(
      profile, makeGameplayInputScopes(7),
      [&](const auto &transition) {
        output.push_back(transition);
        return true;
      });
  router.setGameplayEnabled(true, 100);
  router.consume(keyEvent(SDL_SCANCODE_D, true), 200);
  router.setGameplayEnabled(false, 300);
  router.consume(keyEvent(SDL_SCANCODE_D, false), 400);
  router.setGameplayEnabled(true, 500);

  require(output.size() == 2 && output.front().lane == kExtremeLane &&
              output.back().lane == kExtremeLane &&
              output.back().type ==
                  input::RealtimePhysicalInputTransitionType::Release &&
              output.back().steadyTimestampMicros == 500,
          "extreme imported lanes reconcile without allocating by lane value");
}

void testRealtimePhysicalInputHeldThroughPauseStaysPressed() {
  InputProfile profile;
  profile.bindings.push_back(
      {.id = "native-controller-held-lane",
       .scope = {.player = 1, .keyMode = 7},
       .action = {.kind = input::LogicalActionKind::Lane, .lane = 5},
       .control = {.deviceId = "pad:one",
                   .deviceClass = input::DeviceClass::GameController,
                   .kind = input::ControlKind::Button,
                   .index = SDL_CONTROLLER_BUTTON_A}});
  std::vector<input::RealtimePhysicalInputTransition> output;
  input::RealtimePhysicalInputRouter router(
      profile, makeGameplayInputScopes(7),
      [&](const auto &transition) {
        output.push_back(transition);
        return true;
      });
  router.setGameplayEnabled(true, 100);
  router.consume(
      {.control = {.deviceId = "pad:one",
                   .deviceClass = input::DeviceClass::GameController,
                   .kind = input::ControlKind::Button,
                   .index = SDL_CONTROLLER_BUTTON_A},
       .rawValue = 1.0,
       .normalizedValue = 1.0F},
      200);
  router.setGameplayEnabled(false, 300);
  router.setGameplayEnabled(true, 400);
  require(output.size() == 1 &&
              output.front().type ==
                  input::RealtimePhysicalInputTransitionType::Press,
          "pausing and resuming does not synthesize a held-lane release");

  router.consume(
      {.control = {.deviceId = "pad:one",
                   .deviceClass = input::DeviceClass::GameController,
                   .kind = input::ControlKind::Button,
                   .index = SDL_CONTROLLER_BUTTON_A},
       .rawValue = 0.0,
       .normalizedValue = 0.0F},
      500);
  require(output.size() == 2 &&
              output.back().type ==
                  input::RealtimePhysicalInputTransitionType::Release &&
              output.back().steadyTimestampMicros == 500,
          "a key held through pause releases normally after resume");
}

void testRealtimePhysicalInputDisconnectReleasesHeldLane() {
  InputProfile profile;
  profile.bindings.push_back(
      {.id = "native-midi-lane",
       .scope = {.player = 1, .keyMode = 7},
       .action = {.kind = input::LogicalActionKind::Lane, .lane = 2},
       .control = {.deviceId = "midi:one",
                   .deviceClass = input::DeviceClass::Midi,
                   .kind = input::ControlKind::MidiNote,
                   .index = 60}});
  std::vector<input::RealtimePhysicalInputTransition> output;
  input::RealtimePhysicalInputRouter router(
      profile, makeGameplayInputScopes(7),
      [&](const auto &transition) {
        output.push_back(transition);
        return true;
      });
  router.setGameplayEnabled(true, 100);
  router.consume(
      {.control = {.deviceId = "midi:one",
                   .deviceClass = input::DeviceClass::Midi,
                   .kind = input::ControlKind::MidiNote,
                   .index = 60},
       .rawValue = 127.0,
       .normalizedValue = 1.0F},
      200);
  router.disconnectDevice("midi:one", 250);

  require(output.size() == 2 &&
              output[0].type ==
                  input::RealtimePhysicalInputTransitionType::Press &&
              output[1].type ==
                  input::RealtimePhysicalInputTransitionType::Release &&
              output[1].lane == 2 &&
              output[1].steadyTimestampMicros == 250,
          "native disconnect releases held realtime lanes immediately");
}

void testPlaybackClearPolicyCapsEverySuccessfulClearPath() {
  const audio::PlaybackRate assistedRate{75};
  require(clear_policy::assistClearRequired(assistedRate),
          "non-neutral playback requires the assisted clear mark");
  require(!clear_policy::assistClearRequired(audio::PlaybackRate{100}),
          "neutral playback preserves the selected gauge clear mark");
  require(clear_policy::capRankForPlayback(kClearTypeHardClearRank,
                                           audio::PlaybackRate{100}) ==
              kClearTypeHardClearRank,
          "neutral playback leaves hard clears unchanged");
  require(
      clear_policy::capRankForPlayback(kClearTypeHardClearRank, assistedRate) ==
          kClearTypeAssistedEasyClearRank,
      "rate-assisted hard clears cap at Assisted Easy");
  require(
      clear_policy::capRankForPlayback(kClearTypeFullComboRank, assistedRate) ==
          kClearTypeAssistedEasyClearRank,
      "rate-assisted full combos cannot bypass the clear cap");
  require(clear_policy::capRankForPlayback(
              kClearTypeFailedRank, assistedRate) == kClearTypeFailedRank,
          "the assisted clear cap never promotes a failed attempt");
  require(clear_policy::fullComboRankForPlayback(kClearTypeHardClearRank, true,
                                                 assistedRate) ==
              kClearTypeAssistedEasyClearRank,
          "rate-assisted full-combo derivation stays capped");
  require(clear_policy::fullComboRankForPlayback(kClearTypeHardClearRank, true,
                                                 audio::PlaybackRate{100}) ==
              kClearTypeFullComboRank,
          "neutral full-combo derivation remains unchanged");
  require(clear_policy::fullComboRankForPlayback(
              kClearTypeFailedRank, true, assistedRate) == kClearTypeFailedRank,
          "full-combo derivation never promotes a failed attempt");
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
  testAppliedObserverSeesOneCanonicalReplayStream();
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
  testTouchAndHardwareShareOneLaneOwnershipBoundary();
  testTouchScratchAndDigitalScratchShareOneOwnershipBoundary();
  testScratchReversalKeepsAnOverlappingDigitalHoldCoherent();
  testEscapeFallbackYieldsToAnActiveLogicalPauseBinding();
  testEscapeFallbackRunsInTheOrderedLogicalPipeline();
  testRealtimePhysicalInputPreservesNativeTimestamp();
  testNonStockKeyModesCaptureBmsChannelReplayControls();
  testArbitraryLaneInputDoesNotDependOnBrdControls();
  testPhysicalTouchLaneDoesNotDependOnBrdControls();
  testRealtimeScratchReversalCarriesCanonicalDirections();
  testRealtimeStartProducesCommandAndReplayEdge();
  testRealtimePhysicalInputPauseDefersReleasedLaneUntilResume();
  testArbitraryPhysicalLaneReleaseWhilePausedReconcilesOnResume();
  testExtremeImportedLaneUsesSparsePauseState();
  testRealtimePhysicalInputHeldThroughPauseStaysPressed();
  testRealtimePhysicalInputDisconnectReleasesHeldLane();
  testPlaybackClearPolicyCapsEverySuccessfulClearPath();
  return 0;
}
