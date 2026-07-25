//
// Created by XF on 9/5/2024.
//

#pragma once

#include "../ReplayData.h"
#include "IInputHandler.h"
#include "../bms_parser.hpp"
#include "IRhythmControl.h"
#include "IInputSource.h"
#include "InputDeviceRegistry.h"
#include "LogicalGameplayInputAdapter.h"
#include <functional>
#include <array>
#include <memory>
#include <map>
#include <optional>
#include <vector>

struct FlickState {
  float startX, startY;
  Uint32 startTime;
  bool active;
  int lastFlickDirection; // 0: none, 1: up, -1: down
  bms_parser::LongNote *activeLongNote;
};
class RhythmInputHandler : public IInputHandler {
private:
  std::unique_ptr<IInputSource> touchInputSource;
  InputDeviceRegistry *inputDeviceRegistry = nullptr;
  std::unique_ptr<LogicalGameplayInputPipeline> logicalInputPipeline;
  std::uint64_t inputSubscriptionToken = 0;
  std::uint64_t deviceSubscriptionToken = 0;
  std::array<bool, 6> registryDeviceClassEnabled{true, true, true,
                                                  true, true, true};
  int totalLaneCount;
  int scratchLaneCount;
  int keyMode = 7;
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
  LogicalGameplayInputAdapter::AppliedTransitionCallback
      appliedTransitionCallback;
  bool notifyTouchEvent(SDL_FingerID fingerIndex, ReplayTouchAction action,
                        Vector3 normalizedLocation);
  void onFingerCancel(SDL_FingerID fingerIndex, Vector3 normalizedLocation);
  void releaseExpiredCancelledTouches();
  void notifyTouchLaneApplied(int lane, bool pressed,
                              std::optional<int> scratchDirection =
                                  std::nullopt);

public:
  IRhythmControl *control;
  RhythmInputHandler(
      IRhythmControl *control, const bms_parser::ChartMeta &meta,
      InputDeviceRegistry &registry, const InputProfile &profile,
      std::vector<input::InputScope> activeScopes,
      LogicalGameplayInputAdapter::CommandCallback commandCallback = {},
      float playAreaWidth = 8.0f,
      LogicalGameplayRegistryPolicy registryPolicy = {},
      LogicalGameplayInputAdapter::AppliedTransitionCallback
          appliedTransitionCallback = {});
  ~RhythmInputHandler() override;
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
  void setRegistryDeviceClassEnabled(input::DeviceClass deviceClass,
                                     bool enabled);
  void setTouchEventCallback(
      std::function<bool(SDL_FingerID, ReplayTouchAction, Vector3)> callback);
};
