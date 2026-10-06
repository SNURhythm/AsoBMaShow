#include "i18n/Localization.h"
#include "i18n/PlatformLocale.h"
#include <cassert>
#include <map>
#include <string>

namespace {
struct Translation {
  std::string_view key;
  std::string_view english;
  std::string_view korean;
  std::string_view japanese;
  std::string_view simplifiedChinese;
  std::string_view traditionalChinese;
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
  setLanguage(Language::English);
  const auto originalRevision = revision();
  setLanguage(Language::English);
  assert(revision() == originalRevision);
  const auto gameplayRetry = message("result.gameplay.retry.label");
  const auto irRetry = message("result.ir.retry.label");
  const Text rawRetry("Retry");
  assert(gameplayRetry.resolve() == "Retry");
  assert(irRetry.resolve() == "Retry");
  const auto ownedMessage = [] {
    std::string seconds = "7";
    return message("settings.display.preview.countdown.other",
                   {{"seconds", seconds}});
  }();
  const auto nestedMessage = message("music_player.display_option.enabled",
      {{"name", message("settings.navigation.settings.label")}});
  setLanguage(Language::Korean);
  assert(revision() > originalRevision);
  assert(gameplayRetry.resolve() == "재도전");
  assert(irRetry.resolve() == "다시 시도");
  assert(rawRetry.resolve() == "Retry");
  assert(ownedMessage.resolve() == "7초 후 이전 설정으로 돌아갑니다");
  assert(nestedMessage.resolve() == "설정: 켜짐");
  setLanguage(Language::Japanese);
  assert(ownedMessage.resolve() == "7秒後に元の設定に戻ります");
  assert(nestedMessage.resolve() == "設定: オン");
  assert(message("settings.navigation.settings.label") ==
         message("settings.navigation.settings.label"));
  assert(!(gameplayRetry == irRetry));
  assert(Text("Settings") != message("settings.navigation.settings.label"));
  assert(isLanguagePreference("ja"));
  assert(isLanguagePreference("zh-Hans"));
  assert(isLanguagePreference("zh-Hant"));
  assert(resolveLanguage("system", {"ko-KR", "en-US"}) == Language::Korean);
  assert(resolveLanguage("system", {"ja-JP", "en-US", "ko"}) == Language::Japanese);
  assert(resolveLanguage("system", {"fr-FR", "ja_JP", "ko_KR"}) == Language::Japanese);
  assert(resolveLanguage("system", {"en-US", "ja-JP"}) == Language::English);
  assert(resolveLanguage("ja", {"en"}) == Language::Japanese);
  assert(resolveLanguage("invalid", {"ja"}) == Language::Japanese);
  assert(resolveLanguage("zh-Hans", {"zh-TW"}) == Language::SimplifiedChinese);
  assert(resolveLanguage("zh-Hant", {"zh-CN"}) == Language::TraditionalChinese);
  for (const auto locale : {"zh", "zh-CN", "zh_SG", "zh-Hans", "zh-Hans-TW", "ZH_hans_HK"})
    assert(resolveLanguage("system", {locale}) == Language::SimplifiedChinese);
  for (const auto locale : {"zh-TW", "zh_HK", "zh-MO", "zh-Hant", "zh-Hant-CN", "ZH_hant_SG"})
    assert(resolveLanguage("system", {"fr-FR", locale, "en-US"}) == Language::TraditionalChinese);
  assert(resolveLanguage("system", {"en-US", "zh-TW"}) == Language::English);
  assert(resolveLanguage("invalid", {"zh-TW"}) == Language::TraditionalChinese);
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
  setLanguage(Language::Japanese);
  for (const auto &entry : catalog) {
    assert(!entry.japanese.empty());
    assert(placeholders(entry.english) == placeholders(entry.japanese));
    assert(tr(std::string(entry.key)) == entry.japanese);
  }
  assert(std::string(tr("settings.navigation.settings.label")) == "設定");
  assert(format("settings.display.preview.countdown.other", {{"seconds", "3"}}) ==
         "3秒後に元の設定に戻ります");
  assert(format("music_player.display_option.enabled", {{"name", "Custom {seconds}"}}) ==
         "Custom {seconds}: オン");
  assert(std::string(tr("unknown.message")) == "unknown.message");
  for (const auto chinese : {Language::SimplifiedChinese, Language::TraditionalChinese}) {
    setLanguage(chinese);
    for (const auto &entry : catalog) {
      const auto translated = chinese == Language::SimplifiedChinese
          ? entry.simplifiedChinese : entry.traditionalChinese;
      assert(!translated.empty());
      assert(placeholders(entry.english) == placeholders(translated));
      assert(tr(std::string(entry.key)) == translated);
    }
    assert(ownedMessage.resolve().find('7') != std::string::npos);
    assert(nestedMessage.resolve().find(tr("settings.navigation.settings.label")) != std::string::npos);
    assert(rawRetry.resolve() == "Retry");
  }
  setLanguage(Language::English);
  for (const auto &entry : catalog) {
    assert(tr(std::string(entry.key)) == entry.english);
  }
  assert(std::string(tr("settings.navigation.settings.label")) == "Settings");
  assert(format("settings.display.preview.countdown.other", {{"seconds", "3"}}) ==
         "Reverting in 3 seconds");

  // Exercise the startup adapter with SDL's actual locale-list parsing.
  assert(SDL_SetHint(SDL_HINT_PREFERRED_LOCALES, "fr_FR,ja_JP,en_US"));
  initializePlatformLanguage("system");
  assert(language() == Language::Japanese);
  initializePlatformLanguage("invalid");
  assert(language() == Language::Japanese);
  initializePlatformLanguage("ko");
  assert(language() == Language::Korean);
  initializePlatformLanguage("en");
  assert(language() == Language::English);
  assert(SDL_SetHint(SDL_HINT_PREFERRED_LOCALES, "en_US,ja_JP"));
  initializePlatformLanguage("system");
  assert(language() == Language::English);
  initializePlatformLanguage("ja");
  assert(language() == Language::Japanese);
  assert(SDL_SetHint(SDL_HINT_PREFERRED_LOCALES, "fr_FR"));
  initializePlatformLanguage("system");
  assert(language() == Language::English);
  for (const auto locale : {"zh_CN,en_US", "zh_SG", "zh-Hans_TW", "zh_Hans"}) {
    assert(SDL_SetHint(SDL_HINT_PREFERRED_LOCALES, locale));
    initializePlatformLanguage("system");
    assert(language() == Language::SimplifiedChinese);
    initializePlatformLanguage("zh-Hant");
    assert(language() == Language::TraditionalChinese);
  }
  for (const auto locale : {"fr_FR,zh_TW,en_US", "zh_HK", "zh_MO", "zh-Hant_CN", "zh_Hant"}) {
    assert(SDL_SetHint(SDL_HINT_PREFERRED_LOCALES, locale));
    initializePlatformLanguage("system");
    assert(language() == Language::TraditionalChinese);
    initializePlatformLanguage("zh-Hans");
    assert(language() == Language::SimplifiedChinese);
  }
  SDL_ResetHint(SDL_HINT_PREFERRED_LOCALES);
}
