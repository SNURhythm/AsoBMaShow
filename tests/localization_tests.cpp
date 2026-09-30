#include "i18n/Localization.h"
#include <cassert>
#include <map>
#include <string>

namespace {
struct Translation {
  std::string_view key;
  std::string_view english;
  std::string_view korean;
};
constexpr Translation catalog[] = {
#include "i18n/Messages.inc"
};

std::map<std::string_view, int> placeholders(std::string_view text) {
  std::map<std::string_view, int> result;
  for (std::size_t start = 0;
       (start = text.find('{', start)) != std::string_view::npos;) {
    const auto end = text.find('}', start);
    assert(end != std::string_view::npos);
    ++result[text.substr(start + 1, end - start - 1)];
    start = end + 1;
  }
  return result;
}
} // namespace

int main() {
  using namespace i18n;
  assert(resolveLanguage("system", {"ko-KR", "en-US"}) == Language::Korean);
  assert(resolveLanguage("system", {"ja-JP", "en-US", "ko"}) == Language::English);
  assert(resolveLanguage("system", {"ja-JP", "ko_KR"}) == Language::Korean);
  assert(resolveLanguage("system", {}) == Language::English);
  assert(resolveLanguage("en", {"ko"}) == Language::English);
  assert(resolveLanguage("ko", {"en"}) == Language::Korean);
  setLanguage(Language::Korean);
  std::string_view previous;
  for (const auto &entry : catalog) {
    assert(previous.empty() || previous < entry.key);
    assert(!entry.korean.empty());
    assert(placeholders(entry.english) == placeholders(entry.korean));
    assert(tr(std::string(entry.key)) == entry.korean);
    previous = entry.key;
  }
  assert(std::string(tr("settings.navigation.settings.label")) == "설정");
  assert(std::string(tr("settings.audio.test_sound.label")) == "소리 테스트");
  assert(std::string(tr("unknown.message")) == "unknown.message");
  // English display text is not accepted as a translation key.
  assert(std::string(tr("Settings")) == "Settings");
  assert(std::string(tr("result.gameplay.retry.label")) == "재도전");
  assert(std::string(tr("result.ir.retry.label")) == "다시 시도");
  assert(format("settings.display.preview.countdown.other", {{"seconds", "3"}}) ==
         "3초 후 이전 설정으로 돌아갑니다");
  assert(format("music_player.display_option.enabled", {{"name", "Settings {seconds}"}}) ==
         "Settings {seconds}: 켜짐");
  setLanguage(Language::English);
  for (const auto &entry : catalog) {
    assert(tr(std::string(entry.key)) == entry.english);
  }
  assert(std::string(tr("settings.navigation.settings.label")) == "Settings");
  assert(format("settings.display.preview.countdown.other", {{"seconds", "3"}}) ==
         "Reverting in 3 seconds");
}
