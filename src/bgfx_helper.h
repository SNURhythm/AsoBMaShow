#pragma once
#include <SDL3/SDL_video.h>
#include <bgfx/platform.h>

bool setup_bgfx_platform_data(bgfx::PlatformData &pd, SDL_Window *sdlWindow);
