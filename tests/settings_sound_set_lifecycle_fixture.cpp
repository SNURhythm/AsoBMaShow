#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 1
#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct View {
  inline static std::map<const View *, unsigned> live;
  inline static unsigned serial = 0;
  unsigned generation = ++serial;
  std::string text;
  View() { live[this] = generation; }
  ~View() { live.erase(this); }
  void setText(const std::string &value) { text = value; }
  void setEditingText(const std::string &value) { text = value; }
  float getScrollOffset() { return 0; }
  void setScrollOffset(float) {}
};
struct TrackedInput {
  View *pointer = nullptr;
  unsigned generation = 0;
  inline static int staleAccesses = 0;
  TrackedInput &operator=(View *value) {
    pointer = value;
    generation = value ? value->generation : 0;
    return *this;
  }
  bool operator==(std::nullptr_t) const { return pointer == nullptr; }
  View *operator->() const {
    const auto found = View::live.find(pointer);
    if (found == View::live.end() || found->second != generation) {
      ++staleAccesses;
      static View rejected;
      return &rejected;
    }
    return pointer;
  }
};
struct Pick { bool succeed; std::string path, bookmark; };
struct Picker {
  std::promise<void> gate;
  std::future<void> released = gate.get_future();
  std::future<Pick> result;
  std::atomic_bool entered = false;
  void request(bool cancelled) {
    result = std::async(std::launch::async, [this, cancelled] {
      entered = true;
      released.wait();
      return Pick{!cancelled, cancelled ? "" : "chosen/sounds", "bookmark"};
    });
    while (!entered) std::this_thread::yield();
  }
  bool active() { return result.valid() && result.wait_for(std::chrono::seconds(0)) != std::future_status::ready; }
  std::optional<Pick> consume() {
    return result.valid() ? std::optional<Pick>(result.get()) : std::nullopt;
  }
  void release() { gate.set_value(); result.wait(); }
};
struct SafeAreaInsets { int top = 0, left = 0, bottom = 0, right = 0; };
SafeAreaInsets getSafeAreaInsetsUi() { return {}; }
namespace rendering { int window_width = 1280, window_height = 720; }
struct SettingsScene {
  struct {
    struct { std::string skinSelectSoundSetPath = "original", skinSelectSoundSetBookmark; } settings;
  } context;
  VIEW_FIELDS
  TrackedInput skinSelectSoundSetInput;
  std::vector<View *> views;
  std::unique_ptr<Picker> soundSetFolderPicker = std::make_unique<Picker>();
  bool gameplaySkinControlsBuiltDisabled = false;
  std::string gameplaySkinUiMessage;
  int lastLayoutWidth = -1, lastLayoutHeight = -1;
  int lastSafeTop = 0, lastSafeLeft = 0, lastSafeBottom = 0, lastSafeRight = 0;
  int activeTab = 1, lastLaidOutTab = -1;
  int saves = 0;
  void persistSettings() { ++saves; }
  void initView() {
    rootLayout = new View;
    views.push_back(rootLayout);
    if (activeTab == 1) {
      auto *input = new View;
      views.push_back(input);
      input->setEditingText(context.settings.skinSelectSoundSetPath);
      skinSelectSoundSetInput = input;
      gameplaySkinUiMessageText = new View;
      views.push_back(gameplaySkinUiMessageText);
    }
  }
  void resetViewState();
  void ensureLayoutUpToDate();
  void applyPendingSoundSetFolderPick();
};
SCENE_METHODS

int failures = 0;
void expect(bool condition, const char *message) {
  if (!condition) { ++failures; std::cerr << message << '\n'; }
}
int main() {
  for (bool sameTab : {false, true}) {
    for (bool cancelled : {false, true}) {
      SettingsScene scene;
      scene.ensureLayoutUpToDate();
      const auto deletedGeneration = scene.skinSelectSoundSetInput.generation;
      scene.soundSetFolderPicker->request(cancelled);
      scene.applyPendingSoundSetFolderPick();
      expect(scene.saves == 0, "active picker must not publish before its gate opens");
      scene.activeTab = sameTab ? 1 : 0;
      scene.lastLayoutWidth = -1;
      scene.ensureLayoutUpToDate();
      if (sameTab) {
        expect(scene.skinSelectSoundSetInput.generation != deletedGeneration,
               "same-tab rebuild must bind a new live input");
      } else {
        expect(scene.skinSelectSoundSetInput == nullptr,
               "real reset must clear the destroyed sound-set view pointer");
      }
      scene.soundSetFolderPicker->release();
      scene.applyPendingSoundSetFolderPick();
      expect(scene.context.settings.skinSelectSoundSetPath == (cancelled ? "original" : "chosen/sounds"),
             "late picker success must persist settings even without the skin tab");
      expect(scene.context.settings.skinSelectSoundSetBookmark == (cancelled ? "" : "bookmark"),
             "late picker must preserve its bookmark without touching dead controls");
      expect(scene.saves == (cancelled ? 0 : 1), "cancellation must not persist changes");
      if (sameTab) {
        expect(scene.skinSelectSoundSetInput->text == (cancelled ? "original" : "chosen/sounds"),
               "late result must update only the rebuilt live control");
      }
      expect(TrackedInput::staleAccesses == 0, "late picker must never dereference a deleted view");
      scene.resetViewState();
      expect(scene.skinSelectSoundSetInput == nullptr, "teardown must invalidate the sound-set view pointer");
    }
  }
  return failures == 0 ? 0 : 1;
}
