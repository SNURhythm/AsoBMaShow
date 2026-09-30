#pragma once

#include "Localization.h"
#include <SDL2/SDL.h>

namespace i18n {
// Resolve device preferences at startup and after a saved language change.
// Retained scenes refresh their presentation using the language revision.
inline void initializePlatformLanguage(std::string_view preference) {
  Language resolved = resolveLanguage(preference, {});
  if (preference != "en" && preference != "ko" && preference != "ja") {
    SDL_Locale *locales = SDL_GetPreferredLocales();
    if (locales != nullptr) {
      for (const SDL_Locale *locale = locales; locale->language; ++locale) {
        const std::string_view code(locale->language);
        if (code == "en" || code == "ko" || code == "ja") {
          resolved = resolveLanguage("system", {code});
          break;
        }
      }
      SDL_free(locales);
    }
  }
  setLanguage(resolved);
}
} // namespace i18n
