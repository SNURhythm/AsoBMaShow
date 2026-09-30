#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace i18n {
enum class Language { English, Korean };

// The preference is device-wide. Unknown preferences behave like System.
inline bool isLanguagePreference(std::string_view preference) {
  return preference == "system" || preference == "en" || preference == "ko";
}

inline Language resolveLanguage(
    std::string_view preference,
    std::initializer_list<std::string_view> preferredLanguages) {
  if (preference == "ko") return Language::Korean;
  if (preference == "en") return Language::English;
  for (auto locale : preferredLanguages) {
    locale = locale.substr(0, locale.find_first_of("-_"));
    if (locale == "ko") return Language::Korean;
    if (locale == "en") return Language::English;
  }
  return Language::English;
}

void setLanguage(Language language);
Language language();

// Stable, semantic IDs identify application-owned messages. English copy is
// catalog data, never a lookup key. Unknown IDs are returned visibly to expose
// mistakes; a missing Korean value falls back to the catalog's English value.
const char *tr(const char *key);
std::string tr(const std::string &key);

// Named values are inserted verbatim and can be reordered by a translation.
std::string format(
    const char *key,
    std::initializer_list<std::pair<std::string_view, std::string_view>> values);
} // namespace i18n
