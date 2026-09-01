#include "InputProfile.h"
#include "ChartLaneBinding.h"

#include <SDL2/SDL_scancode.h>

#include <string>

namespace {

void addKeyboardBinding(InputProfile &profile, input::InputScope scope,
                        int lane, SDL_Scancode scancode) {
  profile.bindings.push_back({
      .id = "default-keyboard-p" + std::to_string(scope.player) + "-k" +
            std::to_string(scope.keyMode) + "-lane" + std::to_string(lane) +
            "-scancode" + std::to_string(static_cast<int>(scancode)),
      .scope = scope,
      .action = {input::LogicalActionKind::Lane, lane},
      .control = {.deviceId = "keyboard",
                  .deviceClass = input::DeviceClass::Keyboard,
                  .kind = input::ControlKind::Key,
                  .index = static_cast<int>(scancode),
                  .direction = input::ControlDirection::Any},
  });
}

void addKeyboardPositionBinding(InputProfile &profile,
                                input::InputScope scope, int position,
                                SDL_Scancode scancode) {
  const auto lane =
      input_profile::chartLaneForKeyPosition(scope.keyMode, position);
  if (lane.has_value()) {
    addKeyboardBinding(profile, scope, *lane, scancode);
  }
}

void addKeyboardScratchBinding(InputProfile &profile, input::InputScope scope,
                               input::LogicalActionKind direction,
                               SDL_Scancode scancode) {
  const std::string directionId =
      direction == input::LogicalActionKind::ScratchClockwise
          ? "scratch-clockwise"
          : "scratch-counter-clockwise";
  profile.bindings.push_back({
      .id = "default-keyboard-p" + std::to_string(scope.player) + "-k" +
            std::to_string(scope.keyMode) + "-" + directionId +
            "-scancode" + std::to_string(static_cast<int>(scancode)),
      .scope = scope,
      .action = {direction, 0},
      .control = {.deviceId = "keyboard",
                  .deviceClass = input::DeviceClass::Keyboard,
                  .kind = input::ControlKind::Key,
                  .index = static_cast<int>(scancode),
                  .direction = input::ControlDirection::Any},
  });
}

} // namespace

InputProfile makeDefaultInputProfile() {
  InputProfile profile;

  addKeyboardPositionBinding(profile, {1, 4}, 0, SDL_SCANCODE_D);
  addKeyboardPositionBinding(profile, {1, 4}, 1, SDL_SCANCODE_F);
  addKeyboardPositionBinding(profile, {1, 4}, 2, SDL_SCANCODE_J);
  addKeyboardPositionBinding(profile, {1, 4}, 3, SDL_SCANCODE_K);

  addKeyboardBinding(profile, {1, 5}, 0, SDL_SCANCODE_D);
  addKeyboardBinding(profile, {1, 5}, 1, SDL_SCANCODE_F);
  addKeyboardBinding(profile, {1, 5}, 2, SDL_SCANCODE_SPACE);
  addKeyboardBinding(profile, {1, 5}, 3, SDL_SCANCODE_J);
  addKeyboardBinding(profile, {1, 5}, 4, SDL_SCANCODE_K);
  addKeyboardScratchBinding(
      profile, {1, 5}, input::LogicalActionKind::ScratchCounterClockwise,
      SDL_SCANCODE_LSHIFT);
  addKeyboardScratchBinding(profile, {1, 5},
                            input::LogicalActionKind::ScratchClockwise,
                            SDL_SCANCODE_RSHIFT);

  addKeyboardPositionBinding(profile, {1, 6}, 0, SDL_SCANCODE_S);
  addKeyboardPositionBinding(profile, {1, 6}, 1, SDL_SCANCODE_D);
  addKeyboardPositionBinding(profile, {1, 6}, 2, SDL_SCANCODE_F);
  addKeyboardPositionBinding(profile, {1, 6}, 3, SDL_SCANCODE_J);
  addKeyboardPositionBinding(profile, {1, 6}, 4, SDL_SCANCODE_K);
  addKeyboardPositionBinding(profile, {1, 6}, 5, SDL_SCANCODE_L);

  addKeyboardBinding(profile, {1, 7}, 0, SDL_SCANCODE_S);
  addKeyboardBinding(profile, {1, 7}, 1, SDL_SCANCODE_D);
  addKeyboardBinding(profile, {1, 7}, 2, SDL_SCANCODE_F);
  addKeyboardBinding(profile, {1, 7}, 3, SDL_SCANCODE_SPACE);
  addKeyboardBinding(profile, {1, 7}, 4, SDL_SCANCODE_J);
  addKeyboardBinding(profile, {1, 7}, 5, SDL_SCANCODE_K);
  addKeyboardBinding(profile, {1, 7}, 6, SDL_SCANCODE_L);
  addKeyboardScratchBinding(
      profile, {1, 7}, input::LogicalActionKind::ScratchCounterClockwise,
      SDL_SCANCODE_LSHIFT);
  addKeyboardScratchBinding(profile, {1, 7},
                            input::LogicalActionKind::ScratchClockwise,
                            SDL_SCANCODE_RSHIFT);

  addKeyboardPositionBinding(profile, {1, 8}, 0, SDL_SCANCODE_A);
  addKeyboardPositionBinding(profile, {1, 8}, 1, SDL_SCANCODE_S);
  addKeyboardPositionBinding(profile, {1, 8}, 2, SDL_SCANCODE_D);
  addKeyboardPositionBinding(profile, {1, 8}, 3, SDL_SCANCODE_F);
  addKeyboardPositionBinding(profile, {1, 8}, 4, SDL_SCANCODE_J);
  addKeyboardPositionBinding(profile, {1, 8}, 5, SDL_SCANCODE_K);
  addKeyboardPositionBinding(profile, {1, 8}, 6, SDL_SCANCODE_L);
  addKeyboardPositionBinding(profile, {1, 8}, 7, SDL_SCANCODE_SEMICOLON);

  addKeyboardBinding(profile, {1, 10}, 0, SDL_SCANCODE_Z);
  addKeyboardBinding(profile, {1, 10}, 1, SDL_SCANCODE_S);
  addKeyboardBinding(profile, {1, 10}, 2, SDL_SCANCODE_X);
  addKeyboardBinding(profile, {1, 10}, 3, SDL_SCANCODE_D);
  addKeyboardBinding(profile, {1, 10}, 4, SDL_SCANCODE_C);
  addKeyboardScratchBinding(
      profile, {1, 10}, input::LogicalActionKind::ScratchCounterClockwise,
      SDL_SCANCODE_LSHIFT);
  addKeyboardBinding(profile, {2, 10}, 8, SDL_SCANCODE_COMMA);
  addKeyboardBinding(profile, {2, 10}, 9, SDL_SCANCODE_L);
  addKeyboardBinding(profile, {2, 10}, 10, SDL_SCANCODE_PERIOD);
  addKeyboardBinding(profile, {2, 10}, 11, SDL_SCANCODE_SEMICOLON);
  addKeyboardBinding(profile, {2, 10}, 12, SDL_SCANCODE_SLASH);
  addKeyboardScratchBinding(profile, {2, 10},
                            input::LogicalActionKind::ScratchClockwise,
                            SDL_SCANCODE_RSHIFT);

  addKeyboardBinding(profile, {1, 14}, 0, SDL_SCANCODE_Z);
  addKeyboardBinding(profile, {1, 14}, 1, SDL_SCANCODE_S);
  addKeyboardBinding(profile, {1, 14}, 2, SDL_SCANCODE_X);
  addKeyboardBinding(profile, {1, 14}, 3, SDL_SCANCODE_D);
  addKeyboardBinding(profile, {1, 14}, 4, SDL_SCANCODE_C);
  addKeyboardBinding(profile, {1, 14}, 5, SDL_SCANCODE_F);
  addKeyboardBinding(profile, {1, 14}, 6, SDL_SCANCODE_V);
  addKeyboardScratchBinding(
      profile, {1, 14}, input::LogicalActionKind::ScratchCounterClockwise,
      SDL_SCANCODE_LSHIFT);
  addKeyboardBinding(profile, {2, 14}, 8, SDL_SCANCODE_M);
  addKeyboardBinding(profile, {2, 14}, 9, SDL_SCANCODE_K);
  addKeyboardBinding(profile, {2, 14}, 10, SDL_SCANCODE_COMMA);
  addKeyboardBinding(profile, {2, 14}, 11, SDL_SCANCODE_L);
  addKeyboardBinding(profile, {2, 14}, 12, SDL_SCANCODE_PERIOD);
  addKeyboardBinding(profile, {2, 14}, 13, SDL_SCANCODE_SEMICOLON);
  addKeyboardBinding(profile, {2, 14}, 14, SDL_SCANCODE_SLASH);
  addKeyboardScratchBinding(profile, {2, 14},
                            input::LogicalActionKind::ScratchClockwise,
                            SDL_SCANCODE_RSHIFT);

  return profile;
}
