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
#include <utility>
#include <vector>

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
#include <SDL_uikit_rawtouch.h>
#endif

namespace {
constexpr Uint32 kCancelledTouchGraceMs = 50;

bool hasActiveLongNote(FlickState &flickState) {
  if (flickState.activeLongNote == nullptr) {
    return false;
  }
  if (flickState.activeLongNote->IsHolding) {
    return true;
  }
  flickState.activeLongNote = nullptr;
  return false;
}
} // namespace

bool RhythmInputHandler::notifyTouchEvent(SDL_FingerID fingerIndex,
                                          ReplayTouchAction action,
                                          Vector3 normalizedLocation) {
  if (touchEventCallback != nullptr) {
    return touchEventCallback(fingerIndex, action, normalizedLocation);
  }
  return false;
}

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

Vector3 RhythmInputHandler::normalizedTouchToRenderLocation(
    Vector3 normalizedLocation) const {
  return {normalizedLocation.x * rendering::render_width,
          normalizedLocation.y * rendering::render_height,
          normalizedLocation.z};
}

bool RhythmInputHandler::isLaneOccupied(int lane,
                                        SDL_FingerID exceptFinger) const {
  for (const auto &[fingerId, fingerLane] : fingerToLane) {
    if (fingerId != exceptFinger && fingerLane == lane) {
      return true;
    }
  }
  return false;
}

void RhythmInputHandler::beginFingerLane(SDL_FingerID fingerIndex, int lane,
                                         Vector3 normalizedLocation) {
  fingerToLane[fingerIndex] = lane;
  fingerLanePressed[fingerIndex] = false;
  if (isScratchLane(lane)) {
    flickStates[fingerIndex] = FlickState{normalizedLocation.x,
                                          normalizedLocation.y,
                                          SDL_GetTicks(),
                                          true,
                                          0,
                                          nullptr};
    return;
  }

  flickStates.erase(fingerIndex);
  control->pressLane(lane);
  fingerLanePressed[fingerIndex] = true;
}

void RhythmInputHandler::releaseFingerLane(SDL_FingerID fingerIndex) {
  const auto laneIt = fingerToLane.find(fingerIndex);
  if (laneIt == fingerToLane.end()) {
    fingerLanePressed.erase(fingerIndex);
    flickStates.erase(fingerIndex);
    return;
  }

  const int lane = laneIt->second;
  const auto pressedIt = fingerLanePressed.find(fingerIndex);
  const bool shouldRelease =
      isScratchLane(lane) ||
      (pressedIt != fingerLanePressed.end() && pressedIt->second);
  fingerToLane.erase(laneIt);
  fingerLanePressed.erase(fingerIndex);
  flickStates.erase(fingerIndex);
  if (shouldRelease) {
    control->releaseLane(lane);
  }
}

void RhythmInputHandler::handleScratchMove(SDL_FingerID fingerIndex,
                                           Vector3 normalizedLocation) {
  if (!flickStates.contains(fingerIndex)) {
    return;
  }
  const auto laneIt = fingerToLane.find(fingerIndex);
  if (laneIt == fingerToLane.end() || !isScratchLane(laneIt->second)) {
    return;
  }

  const int lane = laneIt->second;
  FlickState &flickState = flickStates[fingerIndex];
  if (!flickState.active) {
    return;
  }
  float dx = normalizedLocation.x - flickState.startX;
  float dy = normalizedLocation.y - flickState.startY;
  float distance = sqrtf(dx * dx + dy * dy);
  flickState.startX = normalizedLocation.x;
  flickState.startY = normalizedLocation.y;
  const auto pressedIt = fingerLanePressed.find(fingerIndex);
  const bool hasActiveScratchPress =
      pressedIt != fingerLanePressed.end() && pressedIt->second;
  const bool hasActiveLongScratchNote = hasActiveLongNote(flickState);
  const float flickThreshold =
      flickState.lastFlickDirection == 0 ? 0.001f
                                         : (hasActiveLongScratchNote ? 0.01f
                                                                     : 0.002f);
  if (distance > flickThreshold) {
    int direction = dy < 0 ? 1 : -1;
    if (direction != flickState.lastFlickDirection) {
      SDL_Log("Distance: %f, Direction: %d", distance, direction);
      flickState.lastFlickDirection = direction;
      if (hasActiveScratchPress) {
        control->releaseLane(lane);
        fingerLanePressed[fingerIndex] = false;
      }
      auto *note = control->pressLane(lane);
      flickState.activeLongNote =
          note != nullptr && note->IsLongNote()
              ? static_cast<bms_parser::LongNote *>(note)
              : nullptr;
      fingerLanePressed[fingerIndex] = true;
    }
  }
}

void RhythmInputHandler::onFingerDown(SDL_FingerID fingerIndex,
                                      Vector3 normalizedLocation) {
  cancelGraceExpiry.erase(fingerIndex);
  if (notifyTouchEvent(fingerIndex, ReplayTouchAction::Down,
                       normalizedLocation)) {
    return;
  }

  const Vector3 renderLocation =
      normalizedTouchToRenderLocation(normalizedLocation);
  const std::optional<int> lane =
      dragModeEnabled ? touchToLaneIfInside(renderLocation)
                      : std::optional<int>(touchToLane(renderLocation));
  if (!lane.has_value() || isLaneOccupied(*lane, fingerIndex)) {
    return;
  }

  beginFingerLane(fingerIndex, *lane, normalizedLocation);
}
void RhythmInputHandler::onFingerUp(SDL_FingerID fingerIndex,
                                    Vector3 normalizedLocation) {
  cancelGraceExpiry.erase(fingerIndex);
  if (notifyTouchEvent(fingerIndex, ReplayTouchAction::Up,
                       normalizedLocation)) {
    return;
  }
  SDL_Log("FingerUp: %lld, (%f, %f, %f)", static_cast<long long>(fingerIndex),
          normalizedLocation.x, normalizedLocation.y, normalizedLocation.z);
  if (flickStates.contains(fingerIndex)) {
    flickStates.erase(fingerIndex);
  }
  if (fingerToLane.contains(fingerIndex)) {
    releaseFingerLane(fingerIndex);
  } else {
    fingerLanePressed.erase(fingerIndex);
  }
}
void RhythmInputHandler::onFingerMove(SDL_FingerID fingerIndex,
                                      Vector3 normalizedLocation) {
  if (notifyTouchEvent(fingerIndex, ReplayTouchAction::Move,
                       normalizedLocation)) {
    return;
  }
  //  SDL_Log("FingerMove: %d, (%f, %f, %f)", fingerIndex, normalizedLocation.x,
  //  normalizedLocation.y,
  //          normalizedLocation.z);
  if (dragModeEnabled) {
    const Vector3 renderLocation =
        normalizedTouchToRenderLocation(normalizedLocation);
    const std::optional<int> targetLane =
        touchToLaneIfInside(renderLocation);
    const auto currentLaneIt = fingerToLane.find(fingerIndex);
    if (currentLaneIt == fingerToLane.end()) {
      if (targetLane.has_value() &&
          !isLaneOccupied(*targetLane, fingerIndex)) {
        beginFingerLane(fingerIndex, *targetLane, normalizedLocation);
      }
      return;
    }

    const int currentLane = currentLaneIt->second;
    if (!targetLane.has_value()) {
      releaseFingerLane(fingerIndex);
      return;
    }
    if (*targetLane == currentLane) {
      if (isScratchLane(currentLane)) {
        handleScratchMove(fingerIndex, normalizedLocation);
      }
      return;
    }

    releaseFingerLane(fingerIndex);
    if (!isLaneOccupied(*targetLane, fingerIndex)) {
      beginFingerLane(fingerIndex, *targetLane, normalizedLocation);
    }
    return;
  }

  if (fingerToLane.contains(fingerIndex)) {
    const int lane = fingerToLane[fingerIndex];
    if (isScratchLane(lane)) {
      handleScratchMove(fingerIndex, normalizedLocation);
    }
  }
}
void RhythmInputHandler::onFingerCancel(SDL_FingerID fingerIndex,
                                        Vector3 normalizedLocation) {
  if (notifyTouchEvent(fingerIndex, ReplayTouchAction::Cancel,
                       normalizedLocation)) {
    return;
  }
  (void)normalizedLocation;
  if (!fingerToLane.contains(fingerIndex)) {
    flickStates.erase(fingerIndex);
    fingerLanePressed.erase(fingerIndex);
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
    releaseFingerLane(fingerId);
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
  fingerToLane.clear();
  fingerLanePressed.clear();
  flickStates.clear();
  cancelGraceExpiry.clear();
}
void RhythmInputHandler::discardPendingTouchEvents() {
  fingerToLane.clear();
  fingerLanePressed.clear();
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

void RhythmInputHandler::setTouchEventCallback(
    std::function<bool(SDL_FingerID, ReplayTouchAction, Vector3)> callback) {
  touchEventCallback = std::move(callback);
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
int RhythmInputHandler::touchToLaneIndex(Vector3 location) const {
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
  return static_cast<int>(
      std::floor((position.x - playAreaLeftX) * totalLaneCount /
                 playAreaWidth));
}

std::optional<int> RhythmInputHandler::touchToLaneIfInside(
    Vector3 location) const {
  const int line = touchToLaneIndex(location);
  if (totalLaneCount <= 0 || line < 0 || line >= totalLaneCount ||
      laneOrder.empty()) {
    return std::nullopt;
  }
  return laneOrder[line];
}

int RhythmInputHandler::touchToLane(Vector3 location) {
  SDL_Log("Touch to lane: %f, %f, %f", location.x, location.y, location.z);
  const int line = touchToLaneIndex(location);
  const int lane = clampLane(line);
  SDL_Log("Touch to lane: %d", lane);
  return lane;
}

void RhythmInputHandler::setDragModeEnabled(bool enabled) {
  dragModeEnabled = enabled;
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
