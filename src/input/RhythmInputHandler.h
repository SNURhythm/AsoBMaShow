//
// Created by XF on 9/5/2024.
//

#pragma once

#include "IInputHandler.h"
#include "../bms_parser.hpp"
#include "IRhythmControl.h"
#include "IInputSource.h"
#include <map>
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
  IInputSource *sdlInputSource = nullptr;
  IInputSource *touchInputSource = nullptr;
  int totalLaneCount;
  int scratchLaneCount;
  float playAreaWidth = 8.0f;
  float playAreaLeftX = 0.0f;
  std::vector<int> laneOrder;
  std::map<SDL_FingerID, int> fingerToLane;
  int clampLane(int lane) const;
  bool isScratchLane(int lane) const;
  std::map<SDL_FingerID, FlickState> flickStates;
  std::map<SDL_FingerID, Uint32> cancelGraceExpiry;
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
  std::map<SDL_Keycode, int> keyMap;
};
