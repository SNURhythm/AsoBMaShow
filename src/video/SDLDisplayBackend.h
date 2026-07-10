#pragma once

#include "DisplaySettingsManager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace display {
struct SDLNativeDisplayMode {
  int width = 0;
  int height = 0;
  int refreshRateHz = 0;
  std::uint32_t pixelFormat = 0;
  bool operator==(const SDLNativeDisplayMode &) const = default;
};

struct SDLDisplayBounds {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool operator==(const SDLDisplayBounds &) const = default;
};

struct SDLWindowState {
  player_settings::DisplayMode mode = player_settings::DisplayMode::Windowed;
  int displayIndex = 0;
  int width = 0;
  int height = 0;
  int x = 0;
  int y = 0;
  std::uint32_t windowFlags = 0;
  std::optional<SDLNativeDisplayMode> effectiveDisplayMode;
};

class ISDLDisplayAdapter {
public:
  virtual ~ISDLDisplayAdapter() = default;
  virtual int displayCount() const = 0;
  virtual std::string displayName(int displayIndex) const = 0;
  virtual std::vector<SDLNativeDisplayMode>
  displayModes(int displayIndex) const = 0;
  virtual std::optional<SDLNativeDisplayMode>
  desktopDisplayMode(int displayIndex) const = 0;
  virtual std::optional<SDLDisplayBounds>
  displayBounds(int displayIndex, std::string &errorMessage) const = 0;
  virtual SDLWindowState windowState() const = 0;
  virtual bool setFullscreenMode(player_settings::DisplayMode,
                                 std::string &errorMessage) = 0;
  virtual bool clearWindowDisplayMode(std::string &errorMessage) = 0;
  virtual void setWindowSize(int width, int height) = 0;
  virtual void setWindowPosition(int x, int y) = 0;
  virtual bool setWindowDisplayMode(const SDLNativeDisplayMode &,
                                    std::string &errorMessage) = 0;
};

class SDLDisplayBackend final : public IDisplayBackend {
public:
  using ResetFlagsReader = std::function<std::uint32_t()>;
  // Preflight is side-effect free. A false synchronizer result must likewise
  // occur before renderer mutation; a true result commits the renderer state.
  using RendererPreflight =
      std::function<bool(std::uint32_t, std::string &errorMessage)>;
  using RendererSynchronizer =
      std::function<bool(std::uint32_t, std::string &errorMessage)>;

  SDLDisplayBackend(SDL_Window *window, bool fixedMobileDisplay,
                    ResetFlagsReader readResetFlags,
                    RendererPreflight preflightRenderer,
                    RendererSynchronizer synchronizeRenderer);
  SDLDisplayBackend(std::shared_ptr<ISDLDisplayAdapter>,
                    bool fixedMobileDisplay, ResetFlagsReader readResetFlags,
                    RendererPreflight preflightRenderer,
                    RendererSynchronizer synchronizeRenderer);

  Capabilities capabilities() const override;
  RuntimeState capture() const override;
  bool apply(const player_settings::VideoSettings &,
             std::string &errorMessage) override;
  bool restore(const RuntimeState &, std::string &errorMessage) override;

private:
  bool applyWindowSettings(const player_settings::VideoSettings &, int windowX,
                           int windowY, bool restoreExactPosition,
                           std::optional<SDLNativeDisplayMode> restoreMode,
                           std::optional<SDLNativeDisplayMode> &expectedMode,
                           std::string &errorMessage);
  bool verifyWindowSettings(const player_settings::VideoSettings &,
                            const std::optional<SDLNativeDisplayMode> &,
                            bool verifyPosition, int expectedX, int expectedY,
                            std::string &errorMessage) const;
  bool restoreSDLOnly(const RuntimeState &, std::string &errorMessage);
  std::uint32_t currentResetFlags() const;

  std::shared_ptr<ISDLDisplayAdapter> adapter;
  bool fixedMobileDisplay = false;
  ResetFlagsReader readResetFlags;
  RendererPreflight preflightRenderer;
  RendererSynchronizer synchronizeRenderer;
};
} // namespace display
