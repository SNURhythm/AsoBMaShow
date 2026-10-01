// Compiled through settings_audio_video_scene_extract.py. Only rendering and
// the Test Sound backend are replaced; callback, status fields and methods are real.
#include "audio/AudioDeviceManager.h"
#include "video/DisplaySettingsManager.h"
#include "i18n/Localization.h"
#include <functional>
#include <stdexcept>
#include <string>

namespace {
struct SDL_Color { unsigned char r, g, b, a; };
struct StatusView {
  i18n::Text text;
  void setText(const std::string &value) { text = value; }
  void setLocalizedText(const i18n::Text &value) { text = value; }
  void setColor(SDL_Color) {}
};
struct TestSoundSession {
  bool played = true;
  bool playTestSound() { return played; }
};
struct Button {
  std::function<void()> click;
  void setOnClickListener(std::function<void()> callback) { click = std::move(callback); }
};
class SettingsScene {
public:
  // STATUS_FIELDS
  StatusView audio, display, preview;
  StatusView *audioStatusText = &audio;
  StatusView *displayStatusText = &display;
  StatusView *displayPreviewStatusText = &preview;
  SDL_Color audioStatusColor{}, displayStatusColor{};
  TestSoundSession session;
  TestSoundSession *audioVideoSession = &session;
  Button button;
  void bindTestSound() {
    auto *testSoundButton = &button;
    // TEST_SOUND_CALLBACK
  }
};
// STATUS_METHODS

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void testTestSoundSurvivesLanguageSwitchAndTabRebuild() {
  for (bool played : {true, false}) {
    i18n::setLanguage(i18n::Language::English);
    SettingsScene scene;
    scene.session.played = played;
    scene.bindTestSound();
    scene.button.click();
    const char *key = played ? "settings.audio.sound_played.message"
                             : "settings.audio.could_not_play_sound.message";
    require(scene.audio.text.resolve() == i18n::tr(key), "initial Test Sound status");
    for (auto language : {i18n::Language::Korean, i18n::Language::Japanese}) {
      i18n::setLanguage(language);
      require(scene.audio.text.resolve() == i18n::tr(key),
              "visible Test Sound status must follow language changes");
      StatusView rebuilt;
      rebuilt.setLocalizedText(scene.audioStatusMessage);
      require(rebuilt.text.resolve() == i18n::tr(key),
              "returning to Audio must rebuild status in current language");
    }
  }
}

void testApplyStatusesAndRawDiagnosticsKeepTheirIdentity() {
  i18n::setLanguage(i18n::Language::English);
  SettingsScene scene;
  audio::ApplyResult audioResult;
  audioResult.status = audio::ApplyStatus::Unsupported;
  display::ApplyResult displayResult;
  displayResult.status = display::ApplyStatus::PreviewPending;
  scene.setAudioStatus(audioApplyMessage(audioResult), {});
  scene.setDisplayStatus(displayApplyMessage(displayResult), {});
  i18n::setLanguage(i18n::Language::Japanese);
  require(scene.audio.text.resolve() == i18n::tr("settings.audio_video.audio_option_unavailable.message"),
          "audio apply fallback retains language identity");
  require(scene.display.text.resolve() == i18n::tr("settings.audio_video.confirm_within_15_seconds.message"),
          "display status retains language identity");
  require(scene.preview.text.resolve() == scene.display.text.resolve(),
          "preview and display show the same translated status");
  const std::string diagnostic = "Settings: ネイティブ 장치 /tmp/device";
  audioResult.message = diagnostic;
  displayResult.message = diagnostic;
  scene.setAudioStatus(audioApplyMessage(audioResult), {});
  scene.setDisplayStatus(displayApplyMessage(displayResult), {});
  audioResult.message = {};
  displayResult.message = {};
  i18n::setLanguage(i18n::Language::Korean);
  require(scene.audio.text.resolve() == diagnostic && scene.preview.text.resolve() == diagnostic,
          "provider diagnostics remain owned verbatim text");
}
} // namespace
int main() {
  testTestSoundSurvivesLanguageSwitchAndTabRebuild();
  testApplyStatusesAndRawDiagnosticsKeepTheirIdentity();
  i18n::setLanguage(i18n::Language::English);
}
