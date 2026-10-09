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
#include "../scene/play/RealtimeTouchInputRouter.h"
#include <functional>
#include <array>
#include <memory>
#include <map>
#include <optional>
#include <vector>

struct FlickState {
  float startX, startY;
  Uint64 startTime;
  bool active;
  int lastFlickDirection; // 0: none, 1: up, -1: down
  bms_parser::LongNote *activeLongNote;
};
class SDLTouchInputSource;
class RhythmInputHandler : public IInputHandler {
private:
  std::unique_ptr<SDLTouchInputSource> touchInputSource;
  std::function<void(const SDL_Event &, std::uint64_t)> touchIngressCallback;
  InputDeviceRegistry *inputDeviceRegistry = nullptr;
  std::unique_ptr<LogicalGameplayInputPipeline> logicalInputPipeline;
  std::uint64_t inputSubscriptionToken = 0;
  std::uint64_t deviceSubscriptionToken = 0;
  std::array<bool, 6> registryDeviceClassEnabled{true, true, true,
                                                  true, true, true};
  bool applicationBackground = false;
  int totalLaneCount;
  int scratchLaneCount;
  int keyMode = 7;
  float playAreaWidth = 8.0f;
  float playAreaLeftX = 0.0f;
  bool dragModeEnabled = false;
  input::PlayfieldTouchConfig touchConfig;
  std::function<std::optional<bool>(int)> longNoteHeldCallback;
  std::vector<int> chartLaneOrder;
  std::vector<int> laneOrder;
  std::optional<gameplay::RealtimeTouchLayout> touchLaneLayout;
  std::map<SDL_FingerID, int> fingerToLane;
  // Includes touches captured by the scene callback before lane ownership.
  std::map<SDL_FingerID, Vector3> activeTouchPoints;
  std::map<SDL_FingerID, bool> fingerLanePressed;
  int clampLane(int lane) const;
  bool isScratchLane(int lane) const;
  bool isLaneOccupied(int lane, SDL_FingerID exceptFinger) const;
  int touchToLaneIndex(Vector3 location) const;
  std::optional<int> playfieldTouchLane(Vector3 normalizedLocation, bool requireInside) const;
  std::optional<int> touchToLaneIfInside(Vector3 location) const;
  std::optional<int> authoredTouchLane(Vector3 normalizedLocation,
                                      bool requireInside) const;
  Vector3 normalizedTouchToRenderLocation(Vector3 normalizedLocation) const;
  void beginFingerLane(SDL_FingerID fingerIndex, int lane,
                       Vector3 normalizedLocation);
  void releaseFingerLane(SDL_FingerID fingerIndex);
  void handleScratchMove(SDL_FingerID fingerIndex,
                         Vector3 normalizedLocation);
  std::map<SDL_FingerID, FlickState> flickStates;
  std::map<SDL_FingerID, Uint64> cancelGraceExpiry;
  std::function<bool(SDL_FingerID, ReplayTouchAction, Vector3, std::uint64_t)>
      touchEventCallback;
  bool notifyTouchEvent(SDL_FingerID fingerIndex, ReplayTouchAction action,
                        Vector3 normalizedLocation);
  void onFingerCancel(SDL_FingerID fingerIndex, Vector3 normalizedLocation) override;
  void releaseExpiredCancelledTouches();
public:
  // Authored skin geometry routes into the same logical touch ownership as built-in lanes.
  [[nodiscard]] bms_parser::Note *
  applyTouchLane(int lane, bool pressed,
                 std::optional<int> scratchDirection);
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
  void setTouchIngressCallback(
      std::function<void(const SDL_Event &, std::uint64_t)> callback);
  void stopListen();
  void discardPendingTouchEvents();
  void setApplicationBackground(bool background);
  void pumpPendingTouchEvents();
  int touchToLane(Vector3 location);
  void setBindings(const InputProfile &profile,
                   std::vector<input::InputScope> activeScopes);
  void setPlayAreaWidth(float configuredPlayAreaWidth);
  void setTouchLaneOrder(const std::vector<int> &displayedLaneOrder);
  // nullopt selects built-in camera routing; an empty skin layout blocks
  // input until the presentation publishes its first valid lane geometry.
  void setTouchLaneLayout(std::optional<gameplay::RealtimeTouchLayout> layout);
  void setDragModeEnabled(bool enabled);
  void setRegistryDeviceClassEnabled(input::DeviceClass deviceClass,
                                     bool enabled);
  void setLongNoteHeldCallback(
      std::function<std::optional<bool>(int)> callback);
  void setTouchEventCallback(
      std::function<bool(SDL_FingerID, ReplayTouchAction, Vector3, std::uint64_t)> callback);
};
