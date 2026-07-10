#pragma once

#include "DisplaySettingsManager.h"

#include <cstdint>
#include <functional>
#include <string>

struct SDL_Window;

namespace display {
class SDLDisplayBackend final : public IDisplayBackend {
public:
  using ResetFlagsReader = std::function<std::uint32_t()>;
  using ResetFlagsApplier =
      std::function<bool(std::uint32_t, std::string &errorMessage)>;

  SDLDisplayBackend(SDL_Window *window, bool fixedMobileDisplay,
                    ResetFlagsReader readResetFlags,
                    ResetFlagsApplier applyResetFlags,
                    std::uint32_t initialFrameCap = 0);

  Capabilities capabilities() const override;
  RuntimeState capture() const override;
  bool apply(const player_settings::VideoSettings &,
             std::string &errorMessage) override;
  bool restore(const RuntimeState &, std::string &errorMessage) override;

private:
  bool applyWindowSettings(const player_settings::VideoSettings &, int windowX,
                           int windowY, bool restoreExactPosition,
                           std::string &errorMessage);
  bool verifyWindowSettings(const player_settings::VideoSettings &,
                            std::string &errorMessage) const;
  std::uint32_t currentResetFlags() const;

  SDL_Window *window = nullptr;
  bool fixedMobileDisplay = false;
  ResetFlagsReader readResetFlags;
  ResetFlagsApplier applyResetFlags;
  std::uint32_t currentFrameCap = 0;
};
} // namespace display
