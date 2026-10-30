#pragma once
#include "../targets.h"

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
// Temporary Firebase experiment. The request is consumed before SDL_Init.
bool ArmIOSNativeMetalProbe(bool useSDLSurface = false);
void RunIOSNativeMetalProbeIfRequested();
bool RunIOSSDLMetalProbeIfRequested(void *metalLayer);
#endif
