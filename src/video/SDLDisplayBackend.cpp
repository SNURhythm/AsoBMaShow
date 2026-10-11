#include "../platform/IOSApplicationRuntime.h"
#include "SDLDisplayBackend.h"

#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace display {
namespace {
// Settings retain zero-based display indices; SDL3 uses nonzero runtime IDs.
std::vector<SDL_DisplayID> displayIds() {
  int count = 0;
  SDL_DisplayID *ids = SDL_GetDisplays(&count);
  std::vector<SDL_DisplayID> result;
  if (ids) result.assign(ids, ids + count);
  SDL_free(ids);
  return result;
}

SDL_DisplayID displayId(int index) {
  const auto ids = displayIds();
  return index >= 0 && static_cast<std::size_t>(index) < ids.size()
             ? ids[index] : 0;
}

SDLNativeDisplayMode nativeMode(const SDL_DisplayMode &mode) {
  return {.width = mode.w, .height = mode.h,
          .refreshRateHz = static_cast<int>(std::lround(mode.refresh_rate)),
          .pixelFormat = static_cast<std::uint32_t>(mode.format)};
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
    if (!platform::isMainThread())
      return platform::onMain([&] { return displayCount(); });
    return static_cast<int>(displayIds().size());
  }

  std::string displayName(int displayIndex) const override {
    if (!platform::isMainThread())
      return platform::onMain([&] { return displayName(displayIndex); });
    if (const char *name = SDL_GetDisplayName(displayId(displayIndex))) return name;
    return {};
  }

  std::vector<SDLNativeDisplayMode>
  displayModes(int displayIndex) const override {
    if (!platform::isMainThread())
      return platform::onMain([&] { return displayModes(displayIndex); });
    std::vector<SDLNativeDisplayMode> result;
    int count = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(displayId(displayIndex), &count);
    for (int index = 0; index < count; ++index) result.push_back(nativeMode(*modes[index]));
    SDL_free(modes);
    return result;
  }

  std::optional<SDLNativeDisplayMode>
  desktopDisplayMode(int displayIndex) const override {
    if (!platform::isMainThread())
      return platform::onMain([&] { return desktopDisplayMode(displayIndex); });
    const auto *mode = SDL_GetDesktopDisplayMode(displayId(displayIndex));
    if (!mode) return std::nullopt;
    return nativeMode(*mode);
  }

  std::optional<SDLDisplayBounds>
  displayBounds(int displayIndex, std::string &errorMessage) const override {
    if (!platform::isMainThread())
      return platform::onMain([&] { return displayBounds(displayIndex, errorMessage); });
    SDL_Rect bounds{};
    if (!SDL_GetDisplayBounds(displayId(displayIndex), &bounds)) {
      errorMessage = sdlFailure("Could not read display bounds");
      return std::nullopt;
    }
    return SDLDisplayBounds{
        .x = bounds.x, .y = bounds.y, .width = bounds.w, .height = bounds.h};
  }

  SDLWindowState windowState() const override {
    if (!platform::isMainThread())
      return platform::onMain([&] { return windowState(); });
    SDLWindowState result;
    if (window == nullptr) {
      result.displayIndex = -1;
      return result;
    }
    result.windowFlags = SDL_GetWindowFlags(window);
    const auto *fullscreenMode = SDL_GetWindowFullscreenMode(window);
    result.mode = (result.windowFlags & SDL_WINDOW_FULLSCREEN) == 0
        ? player_settings::DisplayMode::Windowed
        : fullscreenMode ? player_settings::DisplayMode::ExclusiveFullscreen
                         : player_settings::DisplayMode::BorderlessFullscreen;
    result.maximized = (result.windowFlags & SDL_WINDOW_MAXIMIZED) != 0;
    const auto ids = displayIds();
    const auto found = std::ranges::find(ids, SDL_GetDisplayForWindow(window));
    result.displayIndex = found == ids.end() ? -1 : static_cast<int>(found - ids.begin());
    SDL_GetWindowSize(window, &result.width, &result.height);
    SDL_GetWindowPosition(window, &result.x, &result.y);
    if (fullscreenMode) result.requestedWindowMode = nativeMode(*fullscreenMode);
    return result;
  }

  std::optional<SDLNativeDisplayMode>
  currentDisplayMode(int displayIndex) const override {
    if (!platform::isMainThread())
      return platform::onMain([&] { return currentDisplayMode(displayIndex); });
    const auto *mode = SDL_GetCurrentDisplayMode(displayId(displayIndex));
    if (!mode) return std::nullopt;
    return nativeMode(*mode);
  }

  bool setFullscreenMode(player_settings::DisplayMode mode,
                         std::string &errorMessage) override {
    if (!platform::isMainThread())
      return platform::onMain([&] { return setFullscreenMode(mode, errorMessage); });
    if (window == nullptr ||
        !SDL_SetWindowFullscreen(window, mode != player_settings::DisplayMode::Windowed) ||
        !SDL_SyncWindow(window)) {
      errorMessage = sdlFailure("Could not change fullscreen state");
      return false;
    }
    return true;
  }

  bool clearWindowDisplayMode(std::string &errorMessage) override {
    if (!platform::isMainThread())
      return platform::onMain([&] { return clearWindowDisplayMode(errorMessage); });
    if (window == nullptr || !SDL_SetWindowFullscreenMode(window, nullptr)) {
      errorMessage = sdlFailure("Could not clear the SDL display mode");
      return false;
    }
    return true;
  }

  void setWindowSize(int width, int height) override {
    if (!platform::isMainThread())
      return platform::onMain([&] { return setWindowSize(width, height); });
    if (window != nullptr) {
      SDL_SetWindowSize(window, width, height);
      SDL_SyncWindow(window);
    }
  }

  void setWindowPosition(int x, int y) override {
    if (!platform::isMainThread())
      return platform::onMain([&] { return setWindowPosition(x, y); });
    if (window != nullptr) {
      SDL_SetWindowPosition(window, x, y);
      SDL_SyncWindow(window);
    }
  }

  bool setWindowDisplayMode(const SDLNativeDisplayMode &mode,
                            std::string &errorMessage) override {
    if (!platform::isMainThread())
      return platform::onMain([&] { return setWindowDisplayMode(mode, errorMessage); });
    if (!window) {
      errorMessage = "No SDL window is available.";
      return false;
    }
    int count = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window), &count);
    bool selected = false;
    for (int index = 0; index < count; ++index) {
      if (nativeMode(*modes[index]) == mode) {
        selected = SDL_SetWindowFullscreenMode(window, modes[index]);
        break;
      }
    }
    SDL_free(modes);
    if (!selected) errorMessage = sdlFailure("Could not select the fullscreen display mode");
    return selected;
  }

  void setWindowMaximized(bool maximized) override {
    if (!platform::isMainThread())
      return platform::onMain([&] { return setWindowMaximized(maximized); });
    if (window == nullptr) {
      return;
    }
    if (maximized) {
      SDL_MaximizeWindow(window);
    } else {
      SDL_RestoreWindow(window);
    }
    SDL_SyncWindow(window);
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
    RendererTransactionFactory beginRendererTransactionValue,
    bool allowMobileVsyncValue)
    : SDLDisplayBackend(std::make_shared<RealSDLDisplayAdapter>(window),
                        fixedMobileDisplayValue, std::move(readResetFlagsValue),
                        std::move(beginRendererTransactionValue),
                        allowMobileVsyncValue) {}

SDLDisplayBackend::SDLDisplayBackend(
    std::shared_ptr<ISDLDisplayAdapter> adapterValue,
    bool fixedMobileDisplayValue, ResetFlagsReader readResetFlagsValue,
    RendererTransactionFactory beginRendererTransactionValue,
    bool allowMobileVsyncValue)
    : adapter(std::move(adapterValue)),
      fixedMobileDisplay(fixedMobileDisplayValue),
      allowMobileVsync(allowMobileVsyncValue),
      readResetFlags(std::move(readResetFlagsValue)),
      beginRendererTransaction(std::move(beginRendererTransactionValue)) {}

std::uint32_t SDLDisplayBackend::currentResetFlags() const {
  return readResetFlags ? readResetFlags() : 0;
}

void SDLDisplayBackend::rememberWindowedGeometry(
    const SDLWindowState &state) const {
  if (state.mode != player_settings::DisplayMode::Windowed || state.maximized ||
      state.width <= 0 || state.height <= 0) {
    return;
  }
  lastWindowedGeometry = WindowedGeometry{
      .width = state.width, .height = state.height, .x = state.x, .y = state.y};
}

void SDLDisplayBackend::rememberRestoredWindowedGeometry(
    const RuntimeState &state) const {
  if (state.settings.mode != player_settings::DisplayMode::Windowed ||
      state.settings.width <= 0 || state.settings.height <= 0) {
    return;
  }
  lastWindowedGeometry = WindowedGeometry{.width = state.settings.width,
                                          .height = state.settings.height,
                                          .x = state.windowX,
                                          .y = state.windowY};
}

void SDLDisplayBackend::observeRuntimeState() const {
  // Mobile geometry is OS-owned, not immutable: resize events still update
  // the renderer. Only desktop normal/maximized restore history is polled here.
  if (fixedMobileDisplay) return;
  if (adapter) {
    rememberWindowedGeometry(adapter->windowState());
  }
}

Capabilities SDLDisplayBackend::capabilities() const {
  const bool rendererTransactionsAvailable =
      static_cast<bool>(beginRendererTransaction);
  Capabilities result{
      .canChangeMode = !fixedMobileDisplay && rendererTransactionsAvailable,
      .canSelectDisplay = !fixedMobileDisplay && rendererTransactionsAvailable,
      .canSelectResolution =
          !fixedMobileDisplay && rendererTransactionsAvailable,
      .canChangeVsync =
          (!fixedMobileDisplay || allowMobileVsync) && rendererTransactionsAvailable,
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
  rememberWindowedGeometry(state);
  result.sdlWindowFlags = state.windowFlags;
  result.settings.mode = state.mode;
  result.settings.displayIndex =
      fixedMobileDisplay ? 0 : std::max(0, state.displayIndex);
  if (state.mode == player_settings::DisplayMode::Windowed && state.maximized &&
      lastWindowedGeometry.has_value()) {
    result.settings.width = lastWindowedGeometry->width;
    result.settings.height = lastWindowedGeometry->height;
    result.windowX = lastWindowedGeometry->x;
    result.windowY = lastWindowedGeometry->y;
  } else {
    result.settings.width = state.width;
    result.settings.height = state.height;
    result.windowX = state.x;
    result.windowY = state.y;
  }
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
    bool verifySize, bool verifyPosition, int expectedX, int expectedY,
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
  if (verifySize &&
      (actual.width != expectedWidth || actual.height != expectedHeight)) {
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
                            !snapshot.windowMaximized,
                            !snapshot.windowMaximized, snapshot.windowX,
                            snapshot.windowY, errorMessage)) {
    return false;
  }
  if (adapter->windowState().maximized != snapshot.windowMaximized) {
    errorMessage = "SDL did not restore the captured maximized state.";
    return false;
  }
  rememberRestoredWindowedGeometry(snapshot);
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
  if (fixedMobileDisplay &&
      (settings.mode != previous.settings.mode || settings.displayIndex != 0 ||
       settings.width != current.width || settings.height != current.height ||
       (!allowMobileVsync && settings.vsync != previous.settings.vsync))) {
    errorMessage = "Display configuration is fixed on this platform.";
    return false;
  }
  const bool mutateWindow =
      !fixedMobileDisplay && !sameDisplayFields(settings, current);
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
  if (!fixedMobileDisplay &&
      settings.mode == player_settings::DisplayMode::ExclusiveFullscreen) {
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
  if (!verifyWindowSettings(settings, expectedMode, true, false, 0, 0,
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
  const bool mutateWindow = !fixedMobileDisplay &&
      (!sameDisplayFields(snapshot.settings, current) ||
       restorePosition || restoreMaximized);
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
  if (!fixedMobileDisplay && snapshot.settings.mode ==
          player_settings::DisplayMode::ExclusiveFullscreen &&
      snapshot.exclusiveRefreshRateHz > 0) {
    restoreMode =
        SDLNativeDisplayMode{.width = snapshot.settings.width,
                             .height = snapshot.settings.height,
                             .refreshRateHz = snapshot.exclusiveRefreshRateHz,
                             .pixelFormat = snapshot.exclusivePixelFormat};
  } else if (!fixedMobileDisplay && snapshot.settings.mode ==
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
                            !snapshot.windowMaximized,
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

  rememberRestoredWindowedGeometry(snapshot);

  // sdlWindowFlags remains diagnostic for focus/minimize/visibility and other
  // window-manager-owned bits. Maximized state is captured explicitly above.
  return RestoreStatus::Restored;
}
} // namespace display
