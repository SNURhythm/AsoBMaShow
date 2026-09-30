#pragma once

#include "Localization.h"
#include <SDL2/SDL.h>

namespace i18n {
// Resolve once at application startup. A preference change is saved for the
// next launch, keeping retained scenes and in-flight tasks in one language.
inline void initializePlatformLanguage(std::string_view preference) {
  Language resolved = resolveLanguage(preference, {});
  if (preference != "en" && preference != "ko") {
    SDL_Locale *locales = SDL_GetPreferredLocales();
    if (locales != nullptr) {
      for (const SDL_Locale *locale = locales; locale->language; ++locale) {
        const std::string_view code(locale->language);
        if (code == "en" || code == "ko") {
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
