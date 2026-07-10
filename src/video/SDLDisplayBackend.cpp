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

class RealSDLDisplayAdapter final : public ISDLDisplayAdapter {
public:
  explicit RealSDLDisplayAdapter(SDL_Window *windowValue)
      : window(windowValue) {}

  int displayCount() const override {
    return std::max(0, SDL_GetNumVideoDisplays());
  }

  std::string displayName(int displayIndex) const override {
    if (const char *name = SDL_GetDisplayName(displayIndex)) {
      return name;
    }
    return {};
  }

  std::vector<SDLNativeDisplayMode>
  displayModes(int displayIndex) const override {
    std::vector<SDLNativeDisplayMode> result;
    const int count = std::max(0, SDL_GetNumDisplayModes(displayIndex));
    result.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
      SDL_DisplayMode mode{};
      if (SDL_GetDisplayMode(displayIndex, index, &mode) == 0) {
        result.push_back({.width = mode.w,
                          .height = mode.h,
                          .refreshRateHz = mode.refresh_rate,
                          .pixelFormat = mode.format});
      }
    }
    return result;
  }

  std::optional<SDLNativeDisplayMode>
  desktopDisplayMode(int displayIndex) const override {
    SDL_DisplayMode mode{};
    if (SDL_GetDesktopDisplayMode(displayIndex, &mode) != 0) {
      return std::nullopt;
    }
    return SDLNativeDisplayMode{.width = mode.w,
                                .height = mode.h,
                                .refreshRateHz = mode.refresh_rate,
                                .pixelFormat = mode.format};
  }

  std::optional<SDLDisplayBounds>
  displayBounds(int displayIndex, std::string &errorMessage) const override {
    SDL_Rect bounds{};
    if (SDL_GetDisplayBounds(displayIndex, &bounds) != 0) {
      errorMessage = sdlFailure("Could not read display bounds");
      return std::nullopt;
    }
    return SDLDisplayBounds{
        .x = bounds.x, .y = bounds.y, .width = bounds.w, .height = bounds.h};
  }

  SDLWindowState windowState() const override {
    SDLWindowState result;
    if (window == nullptr) {
      result.displayIndex = -1;
      return result;
    }
    result.windowFlags = SDL_GetWindowFlags(window);
    result.mode = modeFromFlags(result.windowFlags);
    result.maximized = (result.windowFlags & SDL_WINDOW_MAXIMIZED) != 0;
    result.displayIndex = SDL_GetWindowDisplayIndex(window);
    SDL_GetWindowSize(window, &result.width, &result.height);
    SDL_GetWindowPosition(window, &result.x, &result.y);
    if (result.mode == player_settings::DisplayMode::ExclusiveFullscreen) {
      SDL_DisplayMode mode{};
      if (SDL_GetWindowDisplayMode(window, &mode) == 0) {
        result.requestedWindowMode =
            SDLNativeDisplayMode{.width = mode.w,
                                 .height = mode.h,
                                 .refreshRateHz = mode.refresh_rate,
                                 .pixelFormat = mode.format};
      }
    }
    return result;
  }

  std::optional<SDLNativeDisplayMode>
  currentDisplayMode(int displayIndex) const override {
    SDL_DisplayMode mode{};
    if (SDL_GetCurrentDisplayMode(displayIndex, &mode) != 0) {
      return std::nullopt;
    }
    return SDLNativeDisplayMode{.width = mode.w,
                                .height = mode.h,
                                .refreshRateHz = mode.refresh_rate,
                                .pixelFormat = mode.format};
  }

  bool setFullscreenMode(player_settings::DisplayMode mode,
                         std::string &errorMessage) override {
    std::uint32_t flags = 0;
    std::string_view operation = "Could not leave the current fullscreen mode";
    if (mode == player_settings::DisplayMode::BorderlessFullscreen) {
      flags = SDL_WINDOW_FULLSCREEN_DESKTOP;
      operation = "Could not enter borderless fullscreen";
    } else if (mode == player_settings::DisplayMode::ExclusiveFullscreen) {
      flags = SDL_WINDOW_FULLSCREEN;
      operation = "Could not enter exclusive fullscreen";
    }
    if (window == nullptr || SDL_SetWindowFullscreen(window, flags) != 0) {
      errorMessage = sdlFailure(operation);
      return false;
    }
    return true;
  }

  bool clearWindowDisplayMode(std::string &errorMessage) override {
    if (window == nullptr || SDL_SetWindowDisplayMode(window, nullptr) != 0) {
      errorMessage = sdlFailure("Could not clear the SDL display mode");
      return false;
    }
    return true;
  }

  void setWindowSize(int width, int height) override {
    if (window != nullptr) {
      SDL_SetWindowSize(window, width, height);
    }
  }

  void setWindowPosition(int x, int y) override {
    if (window != nullptr) {
      SDL_SetWindowPosition(window, x, y);
    }
  }

  bool setWindowDisplayMode(const SDLNativeDisplayMode &mode,
                            std::string &errorMessage) override {
    SDL_DisplayMode nativeMode{.format = mode.pixelFormat,
                               .w = mode.width,
                               .h = mode.height,
                               .refresh_rate = mode.refreshRateHz,
                               .driverdata = nullptr};
    if (window == nullptr ||
        SDL_SetWindowDisplayMode(window, &nativeMode) != 0) {
      errorMessage = sdlFailure("Could not select the fullscreen display mode");
      return false;
    }
    return true;
  }

  void setWindowMaximized(bool maximized) override {
    if (window == nullptr) {
      return;
    }
    if (maximized) {
      SDL_MaximizeWindow(window);
    } else {
      SDL_RestoreWindow(window);
    }
  }

private:
  SDL_Window *window = nullptr;
};

bool sameDisplayFields(const player_settings::VideoSettings &settings,
                       const SDLWindowState &state) {
  if (settings.mode != state.mode ||
      settings.displayIndex != state.displayIndex) {
    return false;
  }
  if (settings.mode == player_settings::DisplayMode::BorderlessFullscreen) {
    return true;
  }
  return settings.width == state.width && settings.height == state.height;
}

std::optional<SDLNativeDisplayMode>
findExclusiveMode(const ISDLDisplayAdapter &adapter, int displayIndex,
                  int width, int height) {
  std::optional<SDLNativeDisplayMode> selected;
  for (const auto &candidate : adapter.displayModes(displayIndex)) {
    if (candidate.width != width || candidate.height != height) {
      continue;
    }
    if (!selected.has_value() ||
        candidate.refreshRateHz > selected->refreshRateHz) {
      selected = candidate;
    }
  }
  return selected;
}

std::uint32_t resetFlagsForVsync(std::uint32_t current, bool vsync) {
  if (vsync) {
    return current | BGFX_RESET_VSYNC;
  }
  return current & ~BGFX_RESET_VSYNC;
}
} // namespace

SDLDisplayBackend::SDLDisplayBackend(
    SDL_Window *window, bool fixedMobileDisplayValue,
    ResetFlagsReader readResetFlagsValue,
    RendererTransactionFactory beginRendererTransactionValue)
    : SDLDisplayBackend(std::make_shared<RealSDLDisplayAdapter>(window),
                        fixedMobileDisplayValue, std::move(readResetFlagsValue),
                        std::move(beginRendererTransactionValue)) {}

SDLDisplayBackend::SDLDisplayBackend(
    std::shared_ptr<ISDLDisplayAdapter> adapterValue,
    bool fixedMobileDisplayValue, ResetFlagsReader readResetFlagsValue,
    RendererTransactionFactory beginRendererTransactionValue)
    : adapter(std::move(adapterValue)),
      fixedMobileDisplay(fixedMobileDisplayValue),
      readResetFlags(std::move(readResetFlagsValue)),
      beginRendererTransaction(std::move(beginRendererTransactionValue)) {}

std::uint32_t SDLDisplayBackend::currentResetFlags() const {
  return readResetFlags ? readResetFlags() : 0;
}

Capabilities SDLDisplayBackend::capabilities() const {
  const bool rendererTransactionsAvailable =
      static_cast<bool>(beginRendererTransaction);
  Capabilities result{
      .canChangeMode = !fixedMobileDisplay && rendererTransactionsAvailable,
      .canSelectDisplay = !fixedMobileDisplay && rendererTransactionsAvailable,
      .canSelectResolution =
          !fixedMobileDisplay && rendererTransactionsAvailable,
      .canChangeVsync = !fixedMobileDisplay && rendererTransactionsAvailable,
      .canSetFrameCap = true,
  };
  if (!adapter) {
    return result;
  }

  const int displayCount = adapter->displayCount();
  const SDLWindowState windowState = adapter->windowState();
  const int firstDisplay = fixedMobileDisplay
                               ? std::clamp(windowState.displayIndex, 0,
                                            std::max(0, displayCount - 1))
                               : 0;
  const int endDisplay = fixedMobileDisplay
                             ? std::min(displayCount, firstDisplay + 1)
                             : displayCount;
  for (int displayIndex = firstDisplay; displayIndex < endDisplay;
       ++displayIndex) {
    DisplayInfo info;
    info.index = fixedMobileDisplay ? 0 : displayIndex;
    info.name = adapter->displayName(displayIndex);
    if (info.name.empty()) {
      info.name = "Display " + std::to_string(info.index + 1);
    }

    std::set<std::tuple<int, int, int>> seen;
    for (const auto &mode : adapter->displayModes(displayIndex)) {
      if (mode.width <= 0 || mode.height <= 0 ||
          !seen.emplace(mode.width, mode.height, mode.refreshRateHz).second) {
        continue;
      }
      info.resolutions.push_back({.width = mode.width,
                                  .height = mode.height,
                                  .refreshRateHz = mode.refreshRateHz});
    }
    if (info.resolutions.empty()) {
      if (const auto desktop = adapter->desktopDisplayMode(displayIndex);
          desktop.has_value() && desktop->width > 0 && desktop->height > 0) {
        info.resolutions.push_back({.width = desktop->width,
                                    .height = desktop->height,
                                    .refreshRateHz = desktop->refreshRateHz});
      } else if (windowState.width > 0 && windowState.height > 0) {
        info.resolutions.push_back(
            {.width = windowState.width, .height = windowState.height});
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
  if (!adapter) {
    return result;
  }

  const SDLWindowState state = adapter->windowState();
  result.sdlWindowFlags = state.windowFlags;
  result.settings.mode = state.mode;
  result.settings.displayIndex =
      fixedMobileDisplay ? 0 : std::max(0, state.displayIndex);
  result.settings.width = state.width;
  result.settings.height = state.height;
  result.windowX = state.x;
  result.windowY = state.y;
  result.windowMaximized = state.maximized;
  if (state.requestedWindowMode.has_value()) {
    result.exclusiveRefreshRateHz = state.requestedWindowMode->refreshRateHz;
    result.exclusivePixelFormat = state.requestedWindowMode->pixelFormat;
  }
  return result;
}

bool SDLDisplayBackend::applyWindowSettings(
    const player_settings::VideoSettings &settings, int windowX, int windowY,
    bool restoreExactPosition, std::optional<SDLNativeDisplayMode> restoreMode,
    std::optional<SDLNativeDisplayMode> &expectedMode,
    std::string &errorMessage) {
  if (!adapter) {
    errorMessage = "Display backend has no SDL adapter.";
    return false;
  }

  const SDLWindowState current = adapter->windowState();
  if (fixedMobileDisplay) {
    if (settings.mode != current.mode || settings.displayIndex != 0 ||
        settings.width != current.width || settings.height != current.height ||
        settings.vsync != ((currentResetFlags() & BGFX_RESET_VSYNC) != 0)) {
      errorMessage = "Display configuration is fixed on this platform.";
      return false;
    }
    return true;
  }

  const int displayCount = adapter->displayCount();
  if (settings.displayIndex < 0 || settings.displayIndex >= displayCount) {
    errorMessage = "The requested SDL display is unavailable.";
    return false;
  }
  if (settings.width <= 0 || settings.height <= 0) {
    errorMessage = "The requested window size is invalid.";
    return false;
  }
  if (!adapter->setFullscreenMode(player_settings::DisplayMode::Windowed,
                                  errorMessage) ||
      !adapter->clearWindowDisplayMode(errorMessage)) {
    return false;
  }
  if (current.maximized) {
    adapter->setWindowMaximized(false);
  }

  const auto bounds =
      adapter->displayBounds(settings.displayIndex, errorMessage);
  if (!bounds.has_value()) {
    return false;
  }

  switch (settings.mode) {
  case player_settings::DisplayMode::Windowed: {
    adapter->setWindowSize(settings.width, settings.height);
    const int centeredX =
        bounds->x + std::max(0, (bounds->width - settings.width) / 2);
    const int centeredY =
        bounds->y + std::max(0, (bounds->height - settings.height) / 2);
    adapter->setWindowPosition(restoreExactPosition ? windowX : centeredX,
                               restoreExactPosition ? windowY : centeredY);
    break;
  }
  case player_settings::DisplayMode::BorderlessFullscreen:
    adapter->setWindowPosition(bounds->x, bounds->y);
    if (!adapter->setFullscreenMode(
            player_settings::DisplayMode::BorderlessFullscreen, errorMessage)) {
      return false;
    }
    break;
  case player_settings::DisplayMode::ExclusiveFullscreen:
    expectedMode = restoreMode.has_value()
                       ? std::move(restoreMode)
                       : findExclusiveMode(*adapter, settings.displayIndex,
                                           settings.width, settings.height);
    if (!expectedMode.has_value()) {
      errorMessage = "No matching exclusive fullscreen mode is available.";
      return false;
    }
    adapter->setWindowPosition(bounds->x, bounds->y);
    adapter->setWindowSize(settings.width, settings.height);
    if (!adapter->setWindowDisplayMode(*expectedMode, errorMessage) ||
        !adapter->setFullscreenMode(
            player_settings::DisplayMode::ExclusiveFullscreen, errorMessage)) {
      return false;
    }
    break;
  }
  return true;
}

bool SDLDisplayBackend::verifyWindowSettings(
    const player_settings::VideoSettings &settings,
    const std::optional<SDLNativeDisplayMode> &expectedExclusiveMode,
    bool verifyPosition, int expectedX, int expectedY,
    std::string &errorMessage) const {
  if (!adapter) {
    errorMessage = "Display backend has no SDL adapter.";
    return false;
  }
  if (fixedMobileDisplay) {
    return true;
  }

  const SDLWindowState actual = adapter->windowState();
  if (actual.mode != settings.mode) {
    errorMessage = "SDL did not enter the requested window mode.";
    return false;
  }
  if (actual.displayIndex != settings.displayIndex) {
    errorMessage = "SDL placed the window on a different display.";
    return false;
  }

  int expectedWidth = settings.width;
  int expectedHeight = settings.height;
  if (settings.mode == player_settings::DisplayMode::BorderlessFullscreen) {
    const auto bounds =
        adapter->displayBounds(settings.displayIndex, errorMessage);
    if (!bounds.has_value()) {
      return false;
    }
    expectedWidth = bounds->width;
    expectedHeight = bounds->height;
  }
  if (actual.width != expectedWidth || actual.height != expectedHeight) {
    std::ostringstream message;
    message << "SDL window size is " << actual.width << "x" << actual.height
            << ", expected " << expectedWidth << "x" << expectedHeight << ".";
    errorMessage = message.str();
    return false;
  }

  if (settings.mode == player_settings::DisplayMode::ExclusiveFullscreen) {
    const auto currentMode = adapter->currentDisplayMode(actual.displayIndex);
    if (!expectedExclusiveMode.has_value() || !currentMode.has_value() ||
        currentMode->width != expectedExclusiveMode->width ||
        currentMode->height != expectedExclusiveMode->height ||
        currentMode->refreshRateHz != expectedExclusiveMode->refreshRateHz) {
      errorMessage =
          "SDL did not activate the selected exclusive display mode.";
      return false;
    }
  }
  if (verifyPosition &&
      settings.mode == player_settings::DisplayMode::Windowed &&
      (actual.x != expectedX || actual.y != expectedY)) {
    errorMessage = "SDL did not restore the captured window position.";
    return false;
  }
  return true;
}

bool SDLDisplayBackend::restoreSDLOnly(const RuntimeState &snapshot,
                                       std::string &errorMessage) {
  std::optional<SDLNativeDisplayMode> restoreMode;
  if (snapshot.settings.mode ==
          player_settings::DisplayMode::ExclusiveFullscreen &&
      snapshot.exclusiveRefreshRateHz > 0) {
    restoreMode =
        SDLNativeDisplayMode{.width = snapshot.settings.width,
                             .height = snapshot.settings.height,
                             .refreshRateHz = snapshot.exclusiveRefreshRateHz,
                             .pixelFormat = snapshot.exclusivePixelFormat};
  } else if (snapshot.settings.mode ==
             player_settings::DisplayMode::ExclusiveFullscreen) {
    restoreMode =
        findExclusiveMode(*adapter, snapshot.settings.displayIndex,
                          snapshot.settings.width, snapshot.settings.height);
  }
  std::optional<SDLNativeDisplayMode> expectedMode = restoreMode;
  if (!applyWindowSettings(snapshot.settings, snapshot.windowX,
                           snapshot.windowY, true, std::move(restoreMode),
                           expectedMode, errorMessage)) {
    return false;
  }
  adapter->setWindowMaximized(snapshot.windowMaximized);
  if (!verifyWindowSettings(snapshot.settings, expectedMode,
                            !snapshot.windowMaximized, snapshot.windowX,
                            snapshot.windowY, errorMessage)) {
    return false;
  }
  if (adapter->windowState().maximized != snapshot.windowMaximized) {
    errorMessage = "SDL did not restore the captured maximized state.";
    return false;
  }
  return true;
}

bool SDLDisplayBackend::apply(const player_settings::VideoSettings &settings,
                              std::string &errorMessage) {
  if (!adapter) {
    errorMessage = "Display backend has no SDL adapter.";
    return false;
  }
  const RuntimeState previous = capture();
  const SDLWindowState current = adapter->windowState();
  const bool mutateWindow = !sameDisplayFields(settings, current);
  const std::uint32_t resetFlags =
      resetFlagsForVsync(previous.bgfxResetFlags, settings.vsync);
  const bool synchronize =
      mutateWindow || resetFlags != previous.bgfxResetFlags;

  std::unique_ptr<IRendererDisplayTransaction> rendererTransaction;
  if (synchronize) {
    if (!beginRendererTransaction) {
      errorMessage = "Renderer display synchronization is unavailable.";
      return false;
    }
    rendererTransaction = beginRendererTransaction(resetFlags, errorMessage);
    if (!rendererTransaction) {
      return false;
    }
  }

  std::optional<SDLNativeDisplayMode> expectedMode;
  if (settings.mode == player_settings::DisplayMode::ExclusiveFullscreen) {
    expectedMode = findExclusiveMode(*adapter, settings.displayIndex,
                                     settings.width, settings.height);
    if (!expectedMode.has_value()) {
      errorMessage = "No matching exclusive fullscreen mode is available.";
      return false;
    }
  }
  auto restoreAfterFailure = [&]() {
    if (!mutateWindow) {
      return;
    }
    std::string restoreError;
    if (!restoreSDLOnly(previous, restoreError)) {
      errorMessage += " SDL rollback also failed";
      if (!restoreError.empty()) {
        errorMessage += ": " + restoreError;
      }
    }
  };
  if (mutateWindow && !applyWindowSettings(settings, 0, 0, false, expectedMode,
                                           expectedMode, errorMessage)) {
    restoreAfterFailure();
    return false;
  }
  if (!verifyWindowSettings(settings, expectedMode, false, 0, 0,
                            errorMessage)) {
    restoreAfterFailure();
    return false;
  }
  if (synchronize &&
      !rendererTransaction->synchronize(resetFlags, errorMessage)) {
    restoreAfterFailure();
    return false;
  }
  return true;
}

RestoreStatus SDLDisplayBackend::restore(const RuntimeState &snapshot,
                                         std::string &errorMessage) {
  if (!adapter) {
    errorMessage = "Display backend has no SDL adapter.";
    return RestoreStatus::Failed;
  }
  const RuntimeState beforeRestore = capture();
  const SDLWindowState current = adapter->windowState();
  const bool restorePosition =
      snapshot.settings.mode == player_settings::DisplayMode::Windowed &&
      !snapshot.windowMaximized &&
      (current.x != snapshot.windowX || current.y != snapshot.windowY);
  const bool restoreMaximized = current.maximized != snapshot.windowMaximized;
  const bool mutateWindow = !sameDisplayFields(snapshot.settings, current) ||
                            restorePosition || restoreMaximized;
  const bool synchronize =
      mutateWindow || snapshot.bgfxResetFlags != beforeRestore.bgfxResetFlags;

  std::unique_ptr<IRendererDisplayTransaction> rendererTransaction;
  if (synchronize) {
    if (!beginRendererTransaction) {
      errorMessage = "Renderer display synchronization is unavailable.";
      return RestoreStatus::Failed;
    }
    rendererTransaction =
        beginRendererTransaction(snapshot.bgfxResetFlags, errorMessage);
    if (!rendererTransaction) {
      return RestoreStatus::RetryableFailure;
    }
  }

  std::optional<SDLNativeDisplayMode> restoreMode;
  if (snapshot.settings.mode ==
          player_settings::DisplayMode::ExclusiveFullscreen &&
      snapshot.exclusiveRefreshRateHz > 0) {
    restoreMode =
        SDLNativeDisplayMode{.width = snapshot.settings.width,
                             .height = snapshot.settings.height,
                             .refreshRateHz = snapshot.exclusiveRefreshRateHz,
                             .pixelFormat = snapshot.exclusivePixelFormat};
  } else if (snapshot.settings.mode ==
             player_settings::DisplayMode::ExclusiveFullscreen) {
    restoreMode =
        findExclusiveMode(*adapter, snapshot.settings.displayIndex,
                          snapshot.settings.width, snapshot.settings.height);
    if (!restoreMode.has_value()) {
      errorMessage = "No matching exclusive fullscreen mode is available.";
      return RestoreStatus::Failed;
    }
  }
  std::optional<SDLNativeDisplayMode> expectedMode = restoreMode;
  auto undoFailedRestore = [&]() {
    if (!mutateWindow) {
      return;
    }
    std::string undoError;
    if (!restoreSDLOnly(beforeRestore, undoError)) {
      errorMessage += " SDL recovery also failed";
      if (!undoError.empty()) {
        errorMessage += ": " + undoError;
      }
    }
  };
  if (mutateWindow &&
      !applyWindowSettings(snapshot.settings, snapshot.windowX,
                           snapshot.windowY, true, std::move(restoreMode),
                           expectedMode, errorMessage)) {
    undoFailedRestore();
    return RestoreStatus::Failed;
  }
  if (mutateWindow) {
    adapter->setWindowMaximized(snapshot.windowMaximized);
  }
  if (!verifyWindowSettings(snapshot.settings, expectedMode,
                            restorePosition && !snapshot.windowMaximized,
                            snapshot.windowX, snapshot.windowY, errorMessage) ||
      adapter->windowState().maximized != snapshot.windowMaximized) {
    if (errorMessage.empty()) {
      errorMessage = "SDL did not restore the captured maximized state.";
    }
    undoFailedRestore();
    return RestoreStatus::Failed;
  }
  if (synchronize && !rendererTransaction->synchronize(snapshot.bgfxResetFlags,
                                                       errorMessage)) {
    undoFailedRestore();
    return RestoreStatus::Failed;
  }

  // sdlWindowFlags remains diagnostic for focus/minimize/visibility and other
  // window-manager-owned bits. Maximized state is captured explicitly above.
  return RestoreStatus::Restored;
}
} // namespace display
