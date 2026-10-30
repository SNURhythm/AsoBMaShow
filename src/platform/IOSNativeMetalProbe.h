#pragma once
#include "../targets.h"

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
// Temporary Firebase experiment. The request is consumed before SDL_Init.
enum class IOSNativeMetalProbeMode {
  NativeWindow = 1,
  SDLSurface = 2,
  SDLMetalDisplayLink = 3,
};
bool ArmIOSNativeMetalProbe(
    IOSNativeMetalProbeMode mode = IOSNativeMetalProbeMode::NativeWindow);
void RunIOSNativeMetalProbeIfRequested();
bool RunIOSSDLMetalProbeIfRequested(void *metalLayer);
#endif
