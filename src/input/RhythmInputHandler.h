//
// Created by XF on 9/5/2024.
//

#pragma once

#include "../ReplayData.h"
#include "IInputHandler.h"
#include "../bms_parser.hpp"
#include "IRhythmControl.h"
#include "IInputSource.h"
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <vector>

struct FlickState {
  float startX, startY;
  Uint32 startTime;
  bool active;
  int lastFlickDirection; // 0: none, 1: up, -1: down
  bool isLongNote;
};
class RhythmInputHandler : public IInputHandler {
private:
  std::unique_ptr<IInputSource> sdlInputSource;
  std::unique_ptr<IInputSource> touchInputSource;
  int totalLaneCount;
  int scratchLaneCount;
  float playAreaWidth = 8.0f;
  float playAreaLeftX = 0.0f;
  bool dragModeEnabled = false;
  std::vector<int> laneOrder;
  std::map<SDL_FingerID, int> fingerToLane;
  std::map<SDL_FingerID, bool> fingerLanePressed;
  int clampLane(int lane) const;
  bool isScratchLane(int lane) const;
  bool isLaneOccupied(int lane, SDL_FingerID exceptFinger) const;
  int touchToLaneIndex(Vector3 location) const;
  std::optional<int> touchToLaneIfInside(Vector3 location) const;
  Vector3 normalizedTouchToRenderLocation(Vector3 normalizedLocation) const;
  void beginFingerLane(SDL_FingerID fingerIndex, int lane,
                       Vector3 normalizedLocation);
  void releaseFingerLane(SDL_FingerID fingerIndex);
  void handleScratchMove(SDL_FingerID fingerIndex,
                         Vector3 normalizedLocation);
  std::map<SDL_FingerID, FlickState> flickStates;
  std::map<SDL_FingerID, Uint32> cancelGraceExpiry;
  std::function<bool(SDL_FingerID, ReplayTouchAction, Vector3)>
      touchEventCallback;
  bool notifyTouchEvent(SDL_FingerID fingerIndex, ReplayTouchAction action,
                        Vector3 normalizedLocation);
  void onFingerCancel(SDL_FingerID fingerIndex, Vector3 normalizedLocation);
  void releaseExpiredCancelledTouches();

public:
  IRhythmControl *control;
  RhythmInputHandler(IRhythmControl *control,
                     const bms_parser::ChartMeta &meta,
                     float playAreaWidth = 8.0f);
  void onKeyDown(int keyCode, KeySource keySource) override;
  void onKeyUp(int KeyCode, KeySource Source) override;
  void onFingerDown(SDL_FingerID fingerIndex,
                    Vector3 normalizedLocation) override;
  void onFingerUp(SDL_FingerID fingerIndex, Vector3 normalizedLocation) override;
  void onFingerMove(SDL_FingerID fingerIndex,
                    Vector3 normalizedLocation) override;
  bool startListenSDL();
  bool startListenTouch();
  void stopListen();
  void discardPendingTouchEvents();
  void pumpPendingTouchEvents();
  int touchToLane(Vector3 location);
  void setPlayAreaWidth(float configuredPlayAreaWidth);
  void setDragModeEnabled(bool enabled);
  void setTouchEventCallback(
      std::function<bool(SDL_FingerID, ReplayTouchAction, Vector3)> callback);
  std::map<SDL_Keycode, int> keyMap;
};
