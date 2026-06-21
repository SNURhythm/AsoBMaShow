#pragma once
#include <SDL2/SDL_syswm.h>
#include <bgfx/platform.h>

void setup_bgfx_platform_data(bgfx::PlatformData &pd, const SDL_SysWMinfo &wmi, SDL_Window* sdlWindow);
