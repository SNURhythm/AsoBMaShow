#include "Localization.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <vector>

namespace i18n {
namespace {
std::atomic<Language> currentLanguage{Language::English};
std::atomic<std::uint64_t> languageRevision{0};
} // namespace

void setLanguage(Language language) {
  if (currentLanguage.exchange(language) != language) {
    languageRevision.fetch_add(1);
  }
}
Language language() { return currentLanguage.load(); }
std::uint64_t revision() { return languageRevision.load(); }

namespace detail {
struct Message {
  std::string key;
  std::vector<std::pair<std::string, Text>> arguments;
};

struct Translation {
  std::string_view key;
  const char *english;
  const char *korean;
  const char *japanese;
  const char *simplifiedChinese;
  const char *traditionalChinese;
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
  const char *translated = found->english;
  switch (language()) {
  case Language::Korean: translated = found->korean; break;
  case Language::Japanese: translated = found->japanese; break;
  case Language::SimplifiedChinese: translated = found->simplifiedChinese; break;
  case Language::TraditionalChinese: translated = found->traditionalChinese; break;
  case Language::English: break;
  }
  return translated[0] != '\0' ? translated : found->english;
}

std::string tr(const std::string &key) {
  return tr(key.c_str());
}

Text message(const char *key,
    std::initializer_list<std::pair<std::string_view, Text>> values) {
  auto owned = std::make_shared<detail::Message>();
  owned->key = key;
  owned->arguments.reserve(values.size());
  for (const auto &[name, value] : values) {
    owned->arguments.emplace_back(name, value);
  }
  Text result;
  result.message_ = std::move(owned);
  return result;
}

bool operator==(const Text &left, const Text &right) {
  if (left.message_ == right.message_) return left.literal_ == right.literal_;
  if (!left.message_ || !right.message_) return false;
  return left.message_->key == right.message_->key &&
         left.message_->arguments == right.message_->arguments;
}

std::string Text::resolve() const {
  if (!message_) return literal_;
  const std::string_view pattern(tr(message_->key.c_str()));
  std::string result;
  for (std::size_t position = 0; position < pattern.size();) {
    if (pattern[position] == '{') {
      const auto end = pattern.find('}', position + 1);
      if (end != std::string_view::npos) {
        const auto name = pattern.substr(position + 1, end - position - 1);
        const auto value = std::find_if(
            message_->arguments.begin(), message_->arguments.end(),
            [name](const auto &entry) { return entry.first == name; });
        if (value != message_->arguments.end()) {
          result += value->second.resolve();
          position = end + 1;
          continue;
        }
      }
    }
    result += pattern[position++];
  }
  return result;
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
