//
// Created by XF on 9/4/2024.
//

#pragma once
#include <SDL2/SDL.h>
/**
 *
 */
enum KeySource { VirtualKey, ScanCode, SDLKey };

namespace InputNormalizer {
SDL_Keycode normalize(int keyCode, KeySource keySource);
SDL_Scancode normalizeScancode(int keyCode, KeySource keySource);
}
