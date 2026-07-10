#include "SDLDisplayBackend.h"

#include <SDL2/SDL.h>
#include <bgfx/bgfx.h>

#include <algorithm>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace display {
namespace {
player_settings::DisplayMode modeFromFlags(std::uint32_t flags) {
  if ((flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ==
      SDL_WINDOW_FULLSCREEN_DESKTOP) {
    return player_settings::DisplayMode::BorderlessFullscreen;
  }
  if ((flags & SDL_WINDOW_FULLSCREEN) != 0) {
    return player_settings::DisplayMode::ExclusiveFullscreen;
  }
  return player_settings::DisplayMode::Windowed;
}

std::string sdlFailure(std::string_view operation) {
  std::string message(operation);
  const char *detail = SDL_GetError();
  if (detail != nullptr && *detail != '\0') {
    message += ": ";
    message += detail;
  }
  return message;
}

bool displayBounds(int displayIndex, SDL_Rect &bounds,
                   std::string &errorMessage) {
  if (SDL_GetDisplayBounds(displayIndex, &bounds) != 0) {
    errorMessage = sdlFailure("Could not read display bounds");
    return false;
  }
  return true;
}

bool findExclusiveMode(int displayIndex, int width, int height,
                       SDL_DisplayMode &selected) {
  bool found = false;
  const int count = SDL_GetNumDisplayModes(displayIndex);
  for (int index = 0; index < count; ++index) {
    SDL_DisplayMode candidate{};
    if (SDL_GetDisplayMode(displayIndex, index, &candidate) != 0 ||
        candidate.w != width || candidate.h != height) {
      continue;
    }
    if (!found || candidate.refresh_rate > selected.refresh_rate) {
      selected = candidate;
      found = true;
    }
  }
  return found;
}
} // namespace

SDLDisplayBackend::SDLDisplayBackend(SDL_Window *windowValue,
                                     bool fixedMobileDisplayValue,
                                     ResetFlagsReader readResetFlagsValue,
                                     ResetFlagsApplier applyResetFlagsValue,
                                     std::uint32_t initialFrameCap)
    : window(windowValue), fixedMobileDisplay(fixedMobileDisplayValue),
      readResetFlags(std::move(readResetFlagsValue)),
      applyResetFlags(std::move(applyResetFlagsValue)),
      currentFrameCap(initialFrameCap) {}

std::uint32_t SDLDisplayBackend::currentResetFlags() const {
  return readResetFlags ? readResetFlags() : 0;
}

Capabilities SDLDisplayBackend::capabilities() const {
  Capabilities result{
      .canChangeMode = !fixedMobileDisplay,
      .canSelectDisplay = !fixedMobileDisplay,
      .canSelectResolution = !fixedMobileDisplay,
      .canChangeVsync =
          !fixedMobileDisplay && static_cast<bool>(applyResetFlags),
      .canSetFrameCap = true,
  };

  const int displayCount = std::max(0, SDL_GetNumVideoDisplays());
  const int firstDisplay = fixedMobileDisplay && window != nullptr
                               ? std::max(0, SDL_GetWindowDisplayIndex(window))
                               : 0;
  const int endDisplay = fixedMobileDisplay
                             ? std::min(displayCount, firstDisplay + 1)
                             : displayCount;
  for (int displayIndex = firstDisplay; displayIndex < endDisplay;
       ++displayIndex) {
    DisplayInfo info;
    info.index = fixedMobileDisplay ? 0 : displayIndex;
    if (const char *name = SDL_GetDisplayName(displayIndex)) {
      info.name = name;
    }
    if (info.name.empty()) {
      info.name = "Display " + std::to_string(info.index + 1);
    }

    std::set<std::tuple<int, int, int>> seen;
    const int modeCount = SDL_GetNumDisplayModes(displayIndex);
    for (int modeIndex = 0; modeIndex < modeCount; ++modeIndex) {
      SDL_DisplayMode mode{};
      if (SDL_GetDisplayMode(displayIndex, modeIndex, &mode) != 0 ||
          mode.w <= 0 || mode.h <= 0) {
        continue;
      }
      if (seen.emplace(mode.w, mode.h, mode.refresh_rate).second) {
        info.resolutions.push_back({.width = mode.w,
                                    .height = mode.h,
                                    .refreshRateHz = mode.refresh_rate});
      }
    }

    if (info.resolutions.empty()) {
      SDL_DisplayMode desktop{};
      if (SDL_GetDesktopDisplayMode(displayIndex, &desktop) == 0 &&
          desktop.w > 0 && desktop.h > 0) {
        info.resolutions.push_back({.width = desktop.w,
                                    .height = desktop.h,
                                    .refreshRateHz = desktop.refresh_rate});
      } else if (window != nullptr) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);
        if (width > 0 && height > 0) {
          info.resolutions.push_back({.width = width, .height = height});
        }
      }
    }
    result.displays.push_back(std::move(info));
  }
  return result;
}

RuntimeState SDLDisplayBackend::capture() const {
  RuntimeState result;
  result.bgfxResetFlags = currentResetFlags();
  result.settings.vsync = (result.bgfxResetFlags & BGFX_RESET_VSYNC) != 0;
  result.settings.frameCap = currentFrameCap;
  if (window == nullptr) {
    return result;
  }

  result.sdlWindowFlags = SDL_GetWindowFlags(window);
  result.settings.mode = modeFromFlags(result.sdlWindowFlags);
  result.settings.displayIndex = std::max(0, SDL_GetWindowDisplayIndex(window));
  if (fixedMobileDisplay) {
    result.settings.displayIndex = 0;
  }
  SDL_GetWindowSize(window, &result.settings.width, &result.settings.height);
  SDL_GetWindowPosition(window, &result.windowX, &result.windowY);
  return result;
}

bool SDLDisplayBackend::applyWindowSettings(
    const player_settings::VideoSettings &settings, int windowX, int windowY,
    bool restoreExactPosition, std::string &errorMessage) {
  if (window == nullptr) {
    errorMessage = "Display backend has no SDL window.";
    return false;
  }

  const RuntimeState current = capture();
  if (fixedMobileDisplay) {
    if (settings.mode != current.settings.mode ||
        settings.displayIndex != current.settings.displayIndex ||
        settings.width != current.settings.width ||
        settings.height != current.settings.height ||
        settings.vsync != current.settings.vsync) {
      errorMessage = "Display configuration is fixed on this platform.";
      return false;
    }
    return true;
  }

  const int displayCount = SDL_GetNumVideoDisplays();
  if (settings.displayIndex < 0 || settings.displayIndex >= displayCount) {
    errorMessage = "The requested SDL display is unavailable.";
    return false;
  }
  if (settings.width <= 0 || settings.height <= 0) {
    errorMessage = "The requested window size is invalid.";
    return false;
  }

  if (SDL_SetWindowFullscreen(window, 0) != 0) {
    errorMessage = sdlFailure("Could not leave the current fullscreen mode");
    return false;
  }
  if (SDL_SetWindowDisplayMode(window, nullptr) != 0) {
    errorMessage = sdlFailure("Could not clear the SDL display mode");
    return false;
  }

  SDL_Rect bounds{};
  if (!displayBounds(settings.displayIndex, bounds, errorMessage)) {
    return false;
  }

  switch (settings.mode) {
  case player_settings::DisplayMode::Windowed: {
    SDL_SetWindowSize(window, settings.width, settings.height);
    const int centeredX =
        bounds.x + std::max(0, (bounds.w - settings.width) / 2);
    const int centeredY =
        bounds.y + std::max(0, (bounds.h - settings.height) / 2);
    SDL_SetWindowPosition(window, restoreExactPosition ? windowX : centeredX,
                          restoreExactPosition ? windowY : centeredY);
    break;
  }
  case player_settings::DisplayMode::BorderlessFullscreen:
    SDL_SetWindowPosition(window, bounds.x, bounds.y);
    if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
      errorMessage = sdlFailure("Could not enter borderless fullscreen");
      return false;
    }
    break;
  case player_settings::DisplayMode::ExclusiveFullscreen: {
    SDL_DisplayMode mode{};
    if (!findExclusiveMode(settings.displayIndex, settings.width,
                           settings.height, mode)) {
      errorMessage = "No matching exclusive fullscreen mode is available.";
      return false;
    }
    SDL_SetWindowPosition(window, bounds.x, bounds.y);
    SDL_SetWindowSize(window, settings.width, settings.height);
    if (SDL_SetWindowDisplayMode(window, &mode) != 0) {
      errorMessage = sdlFailure("Could not select the fullscreen display mode");
      return false;
    }
    if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN) != 0) {
      errorMessage = sdlFailure("Could not enter exclusive fullscreen");
      return false;
    }
    break;
  }
  }
  return true;
}

bool SDLDisplayBackend::verifyWindowSettings(
    const player_settings::VideoSettings &settings,
    std::string &errorMessage) const {
  if (window == nullptr) {
    errorMessage = "Display backend has no SDL window.";
    return false;
  }
  if (fixedMobileDisplay) {
    return true;
  }

  const std::uint32_t flags = SDL_GetWindowFlags(window);
  if (modeFromFlags(flags) != settings.mode) {
    errorMessage = "SDL did not enter the requested window mode.";
    return false;
  }
  if (SDL_GetWindowDisplayIndex(window) != settings.displayIndex) {
    errorMessage = "SDL placed the window on a different display.";
    return false;
  }

  int expectedWidth = settings.width;
  int expectedHeight = settings.height;
  if (settings.mode == player_settings::DisplayMode::BorderlessFullscreen) {
    SDL_Rect bounds{};
    if (!displayBounds(settings.displayIndex, bounds, errorMessage)) {
      return false;
    }
    expectedWidth = bounds.w;
    expectedHeight = bounds.h;
  }
  int actualWidth = 0;
  int actualHeight = 0;
  SDL_GetWindowSize(window, &actualWidth, &actualHeight);
  if (actualWidth != expectedWidth || actualHeight != expectedHeight) {
    std::ostringstream message;
    message << "SDL window size is " << actualWidth << "x" << actualHeight
            << ", expected " << expectedWidth << "x" << expectedHeight << ".";
    errorMessage = message.str();
    return false;
  }
  return true;
}

bool SDLDisplayBackend::apply(const player_settings::VideoSettings &settings,
                              std::string &errorMessage) {
  if (!applyWindowSettings(settings, 0, 0, false, errorMessage)) {
    return false;
  }

  std::uint32_t resetFlags = currentResetFlags();
  if (settings.vsync) {
    resetFlags |= BGFX_RESET_VSYNC;
  } else {
    resetFlags &= ~BGFX_RESET_VSYNC;
  }
  if (resetFlags != currentResetFlags()) {
    if (!applyResetFlags) {
      errorMessage = "Renderer VSync changes are unavailable.";
      return false;
    }
    if (!applyResetFlags(resetFlags, errorMessage)) {
      return false;
    }
  }
  if (!verifyWindowSettings(settings, errorMessage)) {
    return false;
  }
  currentFrameCap = settings.frameCap;
  return true;
}

bool SDLDisplayBackend::restore(const RuntimeState &snapshot,
                                std::string &errorMessage) {
  if (!applyWindowSettings(snapshot.settings, snapshot.windowX,
                           snapshot.windowY, true, errorMessage)) {
    return false;
  }
  if (snapshot.bgfxResetFlags != currentResetFlags()) {
    if (!applyResetFlags) {
      errorMessage = "Renderer reset flags cannot be restored.";
      return false;
    }
    if (!applyResetFlags(snapshot.bgfxResetFlags, errorMessage)) {
      return false;
    }
  }
  if (!verifyWindowSettings(snapshot.settings, errorMessage)) {
    return false;
  }
  currentFrameCap = snapshot.settings.frameCap;
  return true;
}
} // namespace display
