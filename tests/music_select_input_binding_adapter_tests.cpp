#include "music_select/MusicSelectInputBindingAdapter.h"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

input::InputBinding binding(input::InputScope scope,
                            input::LogicalAction action,
                            input::PhysicalControl control) {
  return {.scope = scope,
          .action = action,
          .control = std::move(control),
          .activationThreshold = 0.5F,
          .releaseThreshold = 0.25F};
}

input::PhysicalInputEvent event(input::PhysicalControl control, float value) {
  return {.control = std::move(control),
          .rawValue = value,
          .normalizedValue = value};
}

void testSourceConfigurationLayoutsAndScopes() {
  require(musicSelectKeyLayoutForConfig(0) == MusicSelectKeyLayout::Beat7K &&
              musicSelectKeyLayoutForConfig(1) ==
                  MusicSelectKeyLayout::Popn9K &&
              musicSelectKeyLayoutForConfig(2) ==
                  MusicSelectKeyLayout::Beat14K,
          "PlayerConfig.musicselectinput indexes the source enum directly");
  require(musicSelectInputScopes(MusicSelectKeyLayout::Beat7K) ==
                  std::vector<input::InputScope>{{1, 7}} &&
              musicSelectInputScopes(MusicSelectKeyLayout::Popn9K) ==
                  std::vector<input::InputScope>{{1, 9}} &&
              musicSelectInputScopes(MusicSelectKeyLayout::Beat14K) ==
                  std::vector<input::InputScope>{{1, 14}, {2, 14}},
          "each source selector layout consumes its matching input scopes");
}

void testExactLogicalKeyIndexes() {
  using Kind = input::LogicalActionKind;
  require(musicSelectKeyIndex(MusicSelectKeyLayout::Beat7K, {1, 7},
                              {Kind::Lane, 6}) == 6 &&
              musicSelectKeyIndex(MusicSelectKeyLayout::Beat7K, {1, 7},
                                  {Kind::ScratchCounterClockwise, 0}) == 7 &&
              musicSelectKeyIndex(MusicSelectKeyLayout::Beat7K, {1, 7},
                                  {Kind::ScratchClockwise, 0}) == 8,
          "7K positions match Beatoraja's nine input indexes");
  require(musicSelectKeyIndex(MusicSelectKeyLayout::Popn9K, {1, 9},
                              {Kind::Lane, 8}) == 8 &&
              !musicSelectKeyIndex(MusicSelectKeyLayout::Popn9K, {1, 9},
                                   {Kind::ScratchClockwise, 0}),
          "9K positions are nine note inputs without scratch indexes");
  require(musicSelectKeyIndex(MusicSelectKeyLayout::Beat14K, {1, 14},
                              {Kind::Lane, 6}) == 6 &&
              musicSelectKeyIndex(MusicSelectKeyLayout::Beat14K, {1, 14},
                                  {Kind::ScratchCounterClockwise, 0}) == 7 &&
              musicSelectKeyIndex(MusicSelectKeyLayout::Beat14K, {1, 14},
                                  {Kind::ScratchClockwise, 0}) == 8 &&
              musicSelectKeyIndex(MusicSelectKeyLayout::Beat14K, {2, 14},
                                  {Kind::Lane, 8}) == 9 &&
              musicSelectKeyIndex(MusicSelectKeyLayout::Beat14K, {2, 14},
                                  {Kind::Lane, 14}) == 15 &&
              musicSelectKeyIndex(MusicSelectKeyLayout::Beat14K, {2, 14},
                                  {Kind::ScratchCounterClockwise, 0}) == 16 &&
              musicSelectKeyIndex(MusicSelectKeyLayout::Beat14K, {2, 14},
                                  {Kind::ScratchClockwise, 0}) == 17,
          "14K positions match the source's repeated nine-input blocks");
  require(!musicSelectKeyIndex(MusicSelectKeyLayout::Beat14K, {1, 7},
                               {Kind::Lane, 0}) &&
              !musicSelectKeyIndex(MusicSelectKeyLayout::Beat14K, {2, 14},
                                   {Kind::Lane, 15}),
          "bindings outside the configured source input layout are ignored");
}

void testResolverFeedsDigitalAndGlobalOptionState() {
  using Class = input::DeviceClass;
  using Control = input::ControlKind;
  using Kind = input::LogicalActionKind;
  InputProfile profile;
  profile.bindings = {
      binding({1, 14}, {Kind::Lane, 0},
              {.deviceId = "keyboard",
               .deviceClass = Class::Keyboard,
               .kind = Control::Key,
               .index = 10}),
      binding({2, 14}, {Kind::Lane, 8},
              {.deviceId = "pad",
               .deviceClass = Class::GameController,
               .kind = Control::Button,
               .index = 2}),
      binding({1, 14}, {Kind::Start, 0},
              {.deviceId = "keyboard",
               .deviceClass = Class::Keyboard,
               .kind = Control::Key,
               .index = 11}),
      binding({2, 14}, {Kind::Start, 0},
              {.deviceId = "pad",
               .deviceClass = Class::GameController,
               .kind = Control::Button,
               .index = 3}),
  };
  MusicSelectInputBindingAdapter adapter(profile,
                                         MusicSelectKeyLayout::Beat14K);
  adapter.consume(event({.deviceId = "keyboard",
                         .deviceClass = Class::Keyboard,
                         .kind = Control::Key,
                         .index = 10},
                        1.0F));
  adapter.consume(event({.deviceId = "pad",
                         .deviceClass = Class::GameController,
                         .kind = Control::Button,
                         .index = 2},
                        1.0F));
  require(adapter.state().keys[0] && adapter.state().changed[0] &&
              adapter.state().keys[9] && adapter.state().changed[9],
          "configured keyboard and controller bindings share the resolver");
  adapter.clearFrameEdges();
  require(adapter.state().keys[0] && adapter.state().keys[9] &&
              !adapter.state().changed[0] && !adapter.state().changed[9],
          "frame-edge clearing preserves held logical keys");

  const input::PhysicalControl keyboardStart{
      .deviceId = "keyboard",
      .deviceClass = Class::Keyboard,
      .kind = Control::Key,
      .index = 11};
  const input::PhysicalControl padStart{.deviceId = "pad",
                                        .deviceClass = Class::GameController,
                                        .kind = Control::Button,
                                        .index = 3};
  adapter.consume(event(keyboardStart, 1.0F));
  adapter.consume(event(padStart, 1.0F));
  adapter.consume(event(keyboardStart, 0.0F));
  require(adapter.state().start,
          "one player's held Start survives the other player's release");
  adapter.consume(event(padStart, 0.0F));
  require(!adapter.state().start,
          "global Start releases after every configured source is released");
}

void testAxisBindingsPublishSourceAnalogTicks() {
  using Class = input::DeviceClass;
  using Control = input::ControlKind;
  using Direction = input::ControlDirection;
  using Kind = input::LogicalActionKind;
  InputProfile profile;
  profile.bindings = {binding(
      {1, 7}, {Kind::ScratchClockwise, 0},
      {.deviceId = "turntable",
       .deviceClass = Class::Joystick,
       .kind = Control::Axis,
       .index = 0,
       .direction = Direction::Positive})};
  MusicSelectInputBindingAdapter adapter(profile, MusicSelectKeyLayout::Beat7K);
  const input::PhysicalControl axis{.deviceId = "turntable",
                                    .deviceClass = Class::Joystick,
                                    .kind = Control::Axis,
                                    .index = 0,
                                    .direction = Direction::Any};
  adapter.consume(event(axis, 0.9F));
  require(adapter.state().analog[8] && adapter.state().keys[8] &&
              adapter.state().changed[8] &&
              adapter.state().analogDelta[8] == 100,
          "axis bindings use Beatoraja's 0.009 tick-size difference");
  adapter.clearFrameEdges();
  adapter.consume(event(axis, -0.9F));
  require(adapter.state().analogDelta[8] == 23,
          "axis wraparound uses Beatoraja's source difference formula");
  adapter.reset();
  require(!adapter.state().keys[8] && !adapter.state().changed[8] &&
              adapter.state().analogDelta[8] == 0,
          "adapter reset clears live input while preserving configuration");
}

} // namespace

int main() {
  testSourceConfigurationLayoutsAndScopes();
  testExactLogicalKeyIndexes();
  testResolverFeedsDigitalAndGlobalOptionState();
  testAxisBindingsPublishSourceAnalogTicks();
  if (failures != 0) return 1;
  std::cout << "music-select input binding adapter tests passed\n";
  return 0;
}
