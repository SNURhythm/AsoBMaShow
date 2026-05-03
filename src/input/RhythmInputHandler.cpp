//
// Created by XF on 9/5/2024.
//

#include "RhythmInputHandler.h"
#include "SDLInputSource.h"
#include "SDLTouchInputSource.h"
#include "../rendering/common.h"
#include "bx/math.h"
#include "../rendering/Camera.h"
#include "../targets.h"
#include <array>
#include <map>
#include <cmath>
#include <vector>

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
#include <SDL_uikit_rawtouch.h>
#endif

namespace {
constexpr Uint32 kCancelledTouchGraceMs = 50;
} // namespace

void RhythmInputHandler::onKeyDown(int keyCode, KeySource keySource) {

  auto normalizedKeyCode = InputNormalizer::normalize(keyCode, keySource);
  SDL_Log("KeyDown: %d (%d)", keyCode, normalizedKeyCode);
  if (keyMap.contains(normalizedKeyCode)) {
    auto lane = keyMap[normalizedKeyCode];
    control->pressLane(lane);
  }
}
void RhythmInputHandler::onKeyUp(int keyCode, KeySource keySource) {
  SDL_Log("KeyUp: %d", keyCode);
  auto normalizedKeyCode = InputNormalizer::normalize(keyCode, keySource);
  if (keyMap.contains(normalizedKeyCode)) {
    auto lane = keyMap[normalizedKeyCode];
    control->releaseLane(lane);
  }
}
void RhythmInputHandler::onFingerDown(SDL_FingerID fingerIndex,
                                      Vector3 normalizedLocation) {
  cancelGraceExpiry.erase(fingerIndex);

  int lane = touchToLane({normalizedLocation.x * rendering::render_width,
                          normalizedLocation.y * rendering::render_height,
                          normalizedLocation.z});
  for (auto &[index, fingerLane] : fingerToLane) {
    if (fingerLane == lane)
      return;
  }
  fingerToLane[fingerIndex] = lane;
  if (!flickStates.contains(fingerIndex)) {
    flickStates[fingerIndex] = FlickState{normalizedLocation.x,
                                          normalizedLocation.y,
                                          SDL_GetTicks(),
                                          true,
                                          0,
                                          false};
  }
  if (lane == 7 || lane == 15)
    return;

  control->pressLane(lane);
}
void RhythmInputHandler::onFingerUp(SDL_FingerID fingerIndex,
                                    Vector3 normalizedLocation) {
  cancelGraceExpiry.erase(fingerIndex);
  SDL_Log("FingerUp: %lld, (%f, %f, %f)", static_cast<long long>(fingerIndex),
          normalizedLocation.x, normalizedLocation.y, normalizedLocation.z);
  if (flickStates.contains(fingerIndex)) {
    flickStates.erase(fingerIndex);
  }
  if (fingerToLane.contains(fingerIndex)) {
    int lane = fingerToLane[fingerIndex];
    fingerToLane.erase(fingerIndex);
    control->releaseLane(lane);
  }
}
void RhythmInputHandler::onFingerMove(SDL_FingerID fingerIndex,
                                      Vector3 normalizedLocation) {
  //  SDL_Log("FingerMove: %d, (%f, %f, %f)", fingerIndex, normalizedLocation.x,
  //  normalizedLocation.y,
  //          normalizedLocation.z);
  if (fingerToLane.contains(fingerIndex)) {
    int lane = fingerToLane[fingerIndex];
    // is scratch lane
    if (!(lane == 7 || lane == 15))
      return;
    if (!flickStates.contains(fingerIndex))
      return;
    FlickState &flickState = flickStates[fingerIndex];
    if (!flickState.active)
      return;
    float dx = normalizedLocation.x - flickState.startX;
    float dy = normalizedLocation.y - flickState.startY;
    float distance = sqrtf(dx * dx + dy * dy);
    flickState.startX = normalizedLocation.x;
    flickState.startY = normalizedLocation.y;
    float flickThreshold;
    if (flickState.isLongNote) {
      flickThreshold = 0.01;
    } else {
      if (flickState.lastFlickDirection == 0) {
        // make first flick more sensitive
        flickThreshold = 0.001;
      } else {
        flickThreshold = 0.002;
      }
    }
    if (distance > flickThreshold) {
      int direction = dy < 0 ? 1 : -1;
      if (direction != flickState.lastFlickDirection) {
        SDL_Log("Distance: %f, Direction: %d", distance, direction);
        flickState.lastFlickDirection = direction;
        if (flickState.isLongNote) {
          control->releaseLane(lane);
          return;
        }
        auto note = control->pressLane(lane);
        if (note != nullptr) {
          flickState.isLongNote = note->IsLongNote();
        } else {
          flickState.isLongNote = false;
        }
        if (!flickState.isLongNote) {
          control->releaseLane(lane);
        }
      }
    }
  }
}
void RhythmInputHandler::onFingerCancel(SDL_FingerID fingerIndex,
                                        Vector3 normalizedLocation) {
  (void)normalizedLocation;
  if (!fingerToLane.contains(fingerIndex)) {
    flickStates.erase(fingerIndex);
    cancelGraceExpiry.erase(fingerIndex);
    return;
  }
  cancelGraceExpiry[fingerIndex] = SDL_GetTicks() + kCancelledTouchGraceMs;
}

void RhythmInputHandler::releaseExpiredCancelledTouches() {
  if (cancelGraceExpiry.empty()) {
    return;
  }

  const Uint32 now = SDL_GetTicks();
  std::vector<SDL_FingerID> expiredFingers;
  expiredFingers.reserve(cancelGraceExpiry.size());
  for (const auto &[fingerId, expiry] : cancelGraceExpiry) {
    if (SDL_TICKS_PASSED(now, expiry)) {
      expiredFingers.push_back(fingerId);
    }
  }

  for (const auto fingerId : expiredFingers) {
    cancelGraceExpiry.erase(fingerId);
    if (flickStates.contains(fingerId)) {
      flickStates.erase(fingerId);
    }
    if (fingerToLane.contains(fingerId)) {
      const int lane = fingerToLane[fingerId];
      fingerToLane.erase(fingerId);
      control->releaseLane(lane);
    }
  }
}

bool RhythmInputHandler::startListenSDL() {
  if (sdlInputSource != nullptr) {
    return false;
  }
  sdlInputSource = new SDLInputSource();
  sdlInputSource->setHandler(this);
  return sdlInputSource->startListen();
}
bool RhythmInputHandler::startListenTouch() {
  if (touchInputSource != nullptr) {
    return false;
  }
  touchInputSource = new SDLTouchInputSource();
  touchInputSource->setHandler(this);
  return touchInputSource->startListen();
}
void RhythmInputHandler::stopListen() {
  for (auto &input : {&sdlInputSource, &touchInputSource}) {
    if (*input != nullptr) {
      (*input)->stopListen();
      delete *input;
      *input = nullptr;
    }
  }
}
void RhythmInputHandler::pumpPendingTouchEvents() {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  std::array<IOSRawTouchEvent, 64> pendingEvents{};
  while (true) {
    const size_t count =
        IOSPopRawTouchEvents(pendingEvents.data(), pendingEvents.size());
    if (count == 0) {
      break;
    }

    for (size_t i = 0; i < count; ++i) {
      const auto &event = pendingEvents[i];
      float uiNormX = 0.0f;
      float uiNormY = 0.0f;
      rendering::normalizedToUiNormalized(event.normalizedX,
                                          event.normalizedY, uiNormX, uiNormY);
      const Vector3 location(uiNormX, uiNormY, 0.0f);
      const SDL_FingerID fingerId = static_cast<SDL_FingerID>(event.fingerId);
      switch (event.phase) {
      case IOSRawTouchPhaseBegan:
        onFingerDown(fingerId, location);
        break;
      case IOSRawTouchPhaseMoved:
        onFingerMove(fingerId, location);
        break;
      case IOSRawTouchPhaseEnded:
        onFingerUp(fingerId, location);
        break;
      case IOSRawTouchPhaseCancelled:
        onFingerCancel(fingerId, location);
        break;
      }
    }
  }
#endif
  releaseExpiredCancelledTouches();
}
int RhythmInputHandler::clampLane(int lane) const {
  if (lane < 0) {
    return 7; // left scratch
  }
  if (lane >= keyLaneCount) {
    return isDP ? 15 : 7; // right scratch
  }
  if (lane >= 7 && keyLaneCount == 14) {
    // 14Keys: 7 is scratch, so we should map 7~13 to 8~14
    lane += 1;
  }
  if (lane >= 5 && keyLaneCount == 10) {
    // 10Keys: 5,6 is empty and 7 is scratch, so we should map 5~9 to 8~12
    lane += 3;
  }
  return lane;
}
int RhythmInputHandler::touchToLane(Vector3 location) {
  SDL_Log("Touch to lane: %f, %f, %f", location.x, location.y, location.z);
  auto lookAt = rendering::game_camera.getLookAt();
  auto eye = rendering::game_camera.getEye();
  float distance = rendering::game_camera.getDistanceFromEye(lookAt);
  float z = distance - sin(atan2(lookAt.y - eye.y, lookAt.z - eye.z)) * 2;
  bx::Vec3 position =
      rendering::game_camera.deproject(location.x, location.y, z);
  int line = (int)(position.x * totalLaneCount / 8.0f) - 1;
  line = clampLane(line);
  SDL_Log("Touch to lane: %d", line);
  return line;
}
RhythmInputHandler::RhythmInputHandler(IRhythmControl *control,
                                       const bms_parser::ChartMeta &meta)
    : control(control) {
  std::map<int, std::map<SDL_Keycode, int>> DefaultKeyMap = {
      {7,
       {// keys: SDF, SPACE, JKL
        {SDL_KeyCode::SDLK_s, 0},
        {SDL_KeyCode::SDLK_d, 1},
        {SDL_KeyCode::SDLK_f, 2},
        {SDL_KeyCode::SDLK_SPACE, 3},
        {SDL_KeyCode::SDLK_j, 4},
        {SDL_KeyCode::SDLK_k, 5},
        {SDL_KeyCode::SDLK_l, 6},
        // scratch: LShift, RShift
        {SDL_KeyCode::SDLK_LSHIFT, 7},
        {SDL_KeyCode::SDLK_RSHIFT, 7}}},
      {5,
       {// keys: DF, SPACE, JK
        {SDL_KeyCode::SDLK_d, 0},
        {SDL_KeyCode::SDLK_f, 1},
        {SDL_KeyCode::SDLK_SPACE, 2},
        {SDL_KeyCode::SDLK_j, 3},
        {SDL_KeyCode::SDLK_k, 4},
        // scratch: LShift, RShift
        {SDL_KeyCode::SDLK_LSHIFT, 7},
        {SDL_KeyCode::SDLK_RSHIFT, 7}}},
      {14,
       {// keys: ZSXDCFV and MK,L.;/
        {SDL_KeyCode::SDLK_z, 0},
        {SDL_KeyCode::SDLK_s, 1},
        {SDL_KeyCode::SDLK_x, 2},
        {SDL_KeyCode::SDLK_d, 3},
        {SDL_KeyCode::SDLK_c, 4},
        {SDL_KeyCode::SDLK_f, 5},
        {SDL_KeyCode::SDLK_v, 6},
        {SDL_KeyCode::SDLK_m, 8},
        {SDL_KeyCode::SDLK_k, 9},
        {SDL_KeyCode::SDLK_COMMA, 10},
        {SDL_KeyCode::SDLK_l, 11},
        {SDL_KeyCode::SDLK_PERIOD, 12},
        {SDL_KeyCode::SDLK_SEMICOLON, 13},
        {SDL_KeyCode::SDLK_SLASH, 14},
        // Lscratch: LShift
        {SDL_KeyCode::SDLK_LSHIFT, 7},
        // Rscratch: RShift
        {SDL_KeyCode::SDLK_RSHIFT, 15}}},
      {10,
       {// keys: ZSXDC and ,l.;/
        {SDL_KeyCode::SDLK_z, 0},
        {SDL_KeyCode::SDLK_s, 1},
        {SDL_KeyCode::SDLK_x, 2},
        {SDL_KeyCode::SDLK_d, 3},
        {SDL_KeyCode::SDLK_c, 4},
        {SDL_KeyCode::SDLK_COMMA, 8},
        {SDL_KeyCode::SDLK_l, 9},
        {SDL_KeyCode::SDLK_PERIOD, 10},
        {SDL_KeyCode::SDLK_SEMICOLON, 11},
        {SDL_KeyCode::SDLK_SLASH, 12},
        // Lscratch: LShift
        {SDL_KeyCode::SDLK_LSHIFT, 7},
        // Rscratch: RShift
        {SDL_KeyCode::SDLK_RSHIFT, 15}}}};
  keyMap = DefaultKeyMap[meta.KeyMode];
  keyLaneCount = meta.GetKeyLaneCount();
  totalLaneCount = meta.GetTotalLaneCount();
  isDP = meta.IsDP;
}
