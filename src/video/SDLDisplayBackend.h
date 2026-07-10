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
  bool maximized = false;
  std::optional<SDLNativeDisplayMode> requestedWindowMode;
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
  virtual std::optional<SDLNativeDisplayMode>
  currentDisplayMode(int displayIndex) const = 0;
  virtual bool setFullscreenMode(player_settings::DisplayMode,
                                 std::string &errorMessage) = 0;
  virtual bool clearWindowDisplayMode(std::string &errorMessage) = 0;
  virtual void setWindowSize(int width, int height) = 0;
  virtual void setWindowPosition(int x, int y) = 0;
  virtual bool setWindowDisplayMode(const SDLNativeDisplayMode &,
                                    std::string &errorMessage) = 0;
  virtual void setWindowMaximized(bool maximized) = 0;
};

class IRendererDisplayTransaction {
public:
  virtual ~IRendererDisplayTransaction() = default;
  // The transaction owns renderer access from construction through
  // destruction. A false result must occur before renderer state mutation so
  // the backend can repair SDL while the same reservation is still held.
  virtual bool synchronize(std::uint32_t resetFlags,
                           std::string &errorMessage) = 0;
};

class SDLDisplayBackend final : public IDisplayBackend {
public:
  using ResetFlagsReader = std::function<std::uint32_t()>;
  using RendererTransactionFactory =
      std::function<std::unique_ptr<IRendererDisplayTransaction>(
          std::uint32_t, std::string &errorMessage)>;

  SDLDisplayBackend(SDL_Window *window, bool fixedMobileDisplay,
                    ResetFlagsReader readResetFlags,
                    RendererTransactionFactory beginRendererTransaction);
  SDLDisplayBackend(std::shared_ptr<ISDLDisplayAdapter>,
                    bool fixedMobileDisplay, ResetFlagsReader readResetFlags,
                    RendererTransactionFactory beginRendererTransaction);

  Capabilities capabilities() const override;
  RuntimeState capture() const override;
  bool apply(const player_settings::VideoSettings &,
             std::string &errorMessage) override;
  RestoreStatus restore(const RuntimeState &,
                        std::string &errorMessage) override;

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
  RendererTransactionFactory beginRendererTransaction;
};
} // namespace display
