#include "Localization.h"

#include <algorithm>
#include <atomic>
#include <iterator>

namespace i18n {
namespace {
std::atomic<Language> currentLanguage{Language::English};
} // namespace

void setLanguage(Language language) { currentLanguage.store(language); }
Language language() { return currentLanguage.load(); }

namespace detail {
struct Translation {
  std::string_view key;
  const char *english;
  const char *korean;
};
// Sorted by stable ID so lookup allocates nothing. Display text is never used
// as an identifier; equal English values can have distinct contextual IDs.
constexpr Translation messages[] = {
#include "Messages.inc"
};
} // namespace detail

const char *tr(const char *key) {
  const auto found = std::lower_bound(
      std::begin(detail::messages), std::end(detail::messages), std::string_view(key),
      [](const detail::Translation &entry, std::string_view value) {
        return entry.key < value;
      });
  if (found == std::end(detail::messages) || found->key != key) return key;
  return language() == Language::Korean && found->korean[0] != '\0'
             ? found->korean : found->english;
}

std::string tr(const std::string &key) {
  return tr(key.c_str());
}

// Named placeholders allow each language to reorder complete sentences.
// Replacements are copied verbatim and never interpreted as format strings.
std::string format(
    const char *key,
    std::initializer_list<std::pair<std::string_view, std::string_view>> values) {
  const std::string_view pattern(tr(key));
  std::string result;
  for (std::size_t position = 0; position < pattern.size();) {
    if (pattern[position] == '{') {
      const auto end = pattern.find('}', position + 1);
      if (end != std::string_view::npos) {
        const auto name = pattern.substr(position + 1, end - position - 1);
        const auto value = std::find_if(values.begin(), values.end(),
            [name](const auto &entry) { return entry.first == name; });
        if (value != values.end()) {
          result.append(value->second);
          position = end + 1;
          continue;
        }
      }
    }
    result += pattern[position++];
  }
  return result;
}
} // namespace i18n
