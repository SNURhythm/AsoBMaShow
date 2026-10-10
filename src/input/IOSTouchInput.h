#pragma once

// Pass the UIView returned by SDL_Metal_CreateView. Call on UIKit's main thread.
bool InstallIOSGameplayTouchInput(void *nativeView);
void UninstallIOSGameplayTouchInput();
// Main-thread lifecycle/queue-pressure gate. Closing cancels admitted contacts.
void SetIOSGameplayTouchInputEnabled(bool enabled);
bool IOSGameplayTouchInputInstalled();
