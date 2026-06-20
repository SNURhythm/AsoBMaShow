//
// Created by XF on 9/5/2024.
//

#include "RhythmInputHandler.h"
#include "SDLInputSource.h"
#include "SDLTouchInputSource.h"
#include "../rendering/common.h"
#include "bx/math.h"
#include "../rendering/Camera.h"
#include "../scene/play/GameplayGeometry.h"
#include "../targets.h"
#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <memory>
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
  if (isScratchLane(lane))
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
    if (!isScratchLane(lane))
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
  sdlInputSource = std::make_unique<SDLInputSource>();
  sdlInputSource->setHandler(this);
  return sdlInputSource->startListen();
}
bool RhythmInputHandler::startListenTouch() {
  if (touchInputSource != nullptr) {
    return false;
  }
  touchInputSource = std::make_unique<SDLTouchInputSource>();
  touchInputSource->setHandler(this);
  return touchInputSource->startListen();
}
void RhythmInputHandler::stopListen() {
  if (sdlInputSource != nullptr) {
    sdlInputSource->stopListen();
    sdlInputSource.reset();
  }
  if (touchInputSource != nullptr) {
    touchInputSource->stopListen();
    touchInputSource.reset();
  }
}
void RhythmInputHandler::discardPendingTouchEvents() {
  fingerToLane.clear();
  flickStates.clear();
  cancelGraceExpiry.clear();
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  std::array<IOSRawTouchEvent, 64> pendingEvents{};
  while (IOSPopRawTouchEvents(pendingEvents.data(), pendingEvents.size()) != 0) {
  }
#endif
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
      rendering::normalizedToUiNormalized(event.normalizedX, event.normalizedY,
                                          uiNormX, uiNormY);
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

void RhythmInputHandler::setPlayAreaWidth(float configuredPlayAreaWidth) {
  if (!std::isfinite(configuredPlayAreaWidth) ||
      configuredPlayAreaWidth <= 0.001f) {
    configuredPlayAreaWidth = gameplay_geometry::kDefaultPlayAreaWidth;
  }
  playAreaWidth = configuredPlayAreaWidth;
  playAreaLeftX = gameplay_geometry::playAreaLeft(playAreaWidth);
}

int RhythmInputHandler::clampLane(int lane) const {
  if (laneOrder.empty()) {
    return 0;
  }
  const int clampedLane =
      std::clamp(lane, 0, static_cast<int>(laneOrder.size()) - 1);
  return laneOrder[clampedLane];
}
bool RhythmInputHandler::isScratchLane(int lane) const {
  return (scratchLaneCount > 0 && lane == 7) ||
         (scratchLaneCount > 1 && lane == 15);
}
int RhythmInputHandler::touchToLane(Vector3 location) {
  SDL_Log("Touch to lane: %f, %f, %f", location.x, location.y, location.z);
  if (totalLaneCount <= 0) {
    return 0;
  }
  const bx::Vec3 nearPoint = rendering::game_camera.deproject(
      location.x, location.y, rendering::game_camera.getNearClip());
  const bx::Vec3 farPoint = rendering::game_camera.deproject(
      location.x, location.y, rendering::game_camera.getFarClip());
  bx::Vec3 ray = {farPoint.x - nearPoint.x, farPoint.y - nearPoint.y,
                  farPoint.z - nearPoint.z};

  bx::Vec3 position = nearPoint;
  if (std::abs(ray.z) > 0.0001f) {
    const float t = -nearPoint.z / ray.z;
    position = {nearPoint.x + ray.x * t, nearPoint.y + ray.y * t,
                nearPoint.z + ray.z * t};
  }
  int line = static_cast<int>((position.x - playAreaLeftX) * totalLaneCount /
                              playAreaWidth);
  line = clampLane(line);
  SDL_Log("Touch to lane: %d", line);
  return line;
}
RhythmInputHandler::RhythmInputHandler(IRhythmControl *control,
                                       const bms_parser::ChartMeta &meta,
                                       float configuredPlayAreaWidth)
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
      {8,
       {// keys: ASDF, JKL;
        {SDL_KeyCode::SDLK_a, 0},
        {SDL_KeyCode::SDLK_s, 1},
        {SDL_KeyCode::SDLK_d, 2},
        {SDL_KeyCode::SDLK_f, 3},
        {SDL_KeyCode::SDLK_j, 4},
        {SDL_KeyCode::SDLK_k, 5},
        {SDL_KeyCode::SDLK_l, 6},
        {SDL_KeyCode::SDLK_SEMICOLON, 7}}},
      {6,
       {// keys: SDF, JKL
        {SDL_KeyCode::SDLK_s, 0},
        {SDL_KeyCode::SDLK_d, 1},
        {SDL_KeyCode::SDLK_f, 2},
        {SDL_KeyCode::SDLK_j, 3},
        {SDL_KeyCode::SDLK_k, 4},
        {SDL_KeyCode::SDLK_l, 5}}},
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
      {4,
       {// keys: DF, JK
        {SDL_KeyCode::SDLK_d, 0},
        {SDL_KeyCode::SDLK_f, 1},
        {SDL_KeyCode::SDLK_j, 2},
        {SDL_KeyCode::SDLK_k, 3}}},
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
  laneOrder = meta.GetTotalLaneIndices();
  totalLaneCount = static_cast<int>(laneOrder.size());
  scratchLaneCount = meta.GetScratchLaneCount();
  if (!std::isfinite(configuredPlayAreaWidth) ||
      configuredPlayAreaWidth <= 0.001f) {
    configuredPlayAreaWidth = gameplay_geometry::kDefaultPlayAreaWidth;
  }
  playAreaWidth = configuredPlayAreaWidth;
  playAreaLeftX = gameplay_geometry::playAreaLeft(playAreaWidth);
}
