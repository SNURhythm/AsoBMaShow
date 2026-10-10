#pragma once

// Pass the UIView returned by SDL_Metal_CreateView. Call on UIKit's main thread.
bool InstallIOSGameplayTouchInput(void *nativeView);
void UninstallIOSGameplayTouchInput();
bool IOSGameplayTouchInputInstalled();
