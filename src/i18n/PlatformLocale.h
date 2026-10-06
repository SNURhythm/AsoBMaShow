#pragma once

#include "Localization.h"
#include "../targets.h"
#include <SDL2/SDL.h>
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#endif

namespace i18n {
// Resolve device preferences at startup and after a saved language change.
// Retained scenes refresh their presentation using the language revision.
inline void initializePlatformLanguage(std::string_view preference) {
  Language resolved = resolveLanguage(preference, {});
  if (!isLanguagePreference(preference) || preference == "system") {
#if TARGET_OS_ANDROID
    // SDL's Android backend exposes language/region but drops script subtags.
    const std::string tags = GetAndroidPreferredLanguageTags();
    if (!tags.empty()) {
      std::string_view remaining(tags);
      while (!remaining.empty()) {
        const auto separator = remaining.find(',');
        if (const auto supported = languageForLocale(remaining.substr(0, separator))) {
          resolved = *supported;
          break;
        }
        if (separator == std::string_view::npos) break;
        remaining.remove_prefix(separator + 1);
      }
      setLanguage(resolved);
      return;
    }
#endif
    SDL_Locale *locales = SDL_GetPreferredLocales();
    if (locales != nullptr) {
      for (const SDL_Locale *locale = locales; locale->language; ++locale) {
        std::string code(locale->language);
        if (locale->country && locale->country[0] != '\0') {
          code += '-';
          code += locale->country;
        }
        if (const auto supported = languageForLocale(code)) {
          resolved = *supported;
          break;
        }
      }
      SDL_free(locales);
    }
  }
  setLanguage(resolved);
}
} // namespace i18n
