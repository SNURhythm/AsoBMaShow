//
// Created by XF on 9/4/2024.
//

#pragma once
#include "InputNormalizer.h"
#include "../math/Vector3.h"
class IInputHandler {
public:
  virtual ~IInputHandler() = default;
  virtual void onKeyDown(int keyCode, KeySource keySource) = 0;
  virtual void onKeyUp(int keyCode, KeySource keySource) = 0;
  virtual void onFingerDown(SDL_FingerID fingerIndex, Vector3 location) = 0;
  virtual void onFingerUp(SDL_FingerID fingerIndex, Vector3 location) = 0;
  virtual void onFingerMove(SDL_FingerID fingerIndex, Vector3 location) = 0;
};
