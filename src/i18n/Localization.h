#pragma once

#include <initializer_list>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace i18n {
enum class Language { English, Korean, Japanese, SimplifiedChinese, TraditionalChinese };

// The preference is device-wide. Unknown preferences behave like System.
inline bool isLanguagePreference(std::string_view preference) {
  return preference == "system" || preference == "en" ||
         preference == "ko" || preference == "ja" ||
         preference == "zh-Hans" || preference == "zh-Hant";
}

inline std::optional<Language> languageForLocale(std::string_view locale) {
  std::string normalized(locale);
  for (char &character : normalized) {
    if (character == '_') character = '-';
    else if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
  }
  locale = normalized;
  const auto base = locale.substr(0, locale.find('-'));
  if (base == "en") return Language::English;
  if (base == "ko") return Language::Korean;
  if (base == "ja") return Language::Japanese;
  if (base != "zh") return std::nullopt;

  // Explicit script takes priority over region (for example zh-Hans-TW).
  bool traditionalRegion = false;
  auto separator = locale.find('-');
  while (separator != std::string_view::npos) {
    locale.remove_prefix(separator + 1);
    separator = locale.find('-');
    const auto component = locale.substr(0, separator);
    if (component == "hans") return Language::SimplifiedChinese;
    if (component == "hant") return Language::TraditionalChinese;
    if (component == "tw" || component == "hk" || component == "mo")
      traditionalRegion = true;
  }
  return traditionalRegion ? Language::TraditionalChinese : Language::SimplifiedChinese;
}

inline Language resolveLanguage(
    std::string_view preference,
    std::initializer_list<std::string_view> preferredLanguages) {
  if (isLanguagePreference(preference) && preference != "system")
    return *languageForLocale(preference);
  for (const auto locale : preferredLanguages) {
    if (const auto resolved = languageForLocale(locale)) return *resolved;
  }
  return Language::English;
}

void setLanguage(Language language);
Language language();
std::uint64_t revision();

namespace detail { struct Message; }

// Presentation text retains its identity across language changes. Raw values
// (including user input and chart metadata) never become translation keys.
class Text {
public:
  Text() = default;
  Text(const char *literal) : literal_(literal) {}
  Text(std::string literal) : literal_(std::move(literal)) {}
  Text(std::string_view literal) : literal_(literal) {}

  [[nodiscard]] std::string resolve() const;
  [[nodiscard]] bool empty() const { return resolve().empty(); }
  [[nodiscard]] bool isLocalized() const { return message_ != nullptr; }
  friend bool operator==(const Text &left, const Text &right);

private:
  std::string literal_;
  std::shared_ptr<const detail::Message> message_;
  friend Text message(const char *key,
      std::initializer_list<std::pair<std::string_view, Text>> values);
};

// Named arguments are owned, and can themselves be localized messages.
Text message(const char *key,
    std::initializer_list<std::pair<std::string_view, Text>> values = {});

// Stable, semantic IDs identify application-owned messages. English copy is
// catalog data, never a lookup key. Unknown IDs are returned visibly to expose
// mistakes; a missing translation falls back to the catalog's English value.
const char *tr(const char *key);
std::string tr(const std::string &key);

// Named values are inserted verbatim and can be reordered by a translation.
std::string format(
    const char *key,
    std::initializer_list<std::pair<std::string_view, std::string_view>> values);
} // namespace i18n
