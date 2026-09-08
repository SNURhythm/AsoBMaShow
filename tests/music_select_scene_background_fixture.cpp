#include <atomic>
#include <cassert>
#include <initializer_list>
#include <optional>

namespace audio::diag {
void SelectAudioLog(const char *) {}
}

struct Preview {
  bool suppressed = false;
  bool playing = true;
  int resumes = 0;
  void reset() {}
  void silence() { suppressed = true; playing = false; }
  void resumeDefaultBgm() { suppressed = false; playing = true; ++resumes; }
  void switchToPreview() { if (!suppressed) playing = true; }
};

struct Skin {
  bool playing = true;
  void suspendAudio() { playing = false; }
  void resumeAudio() { playing = true; }
};

struct Input {
  void endEditing() {}
  void cancel() {}
};

struct ExternalUrlService {
  void close(int) {}
};

struct MusicSelectScene {
  struct FolderStatusLoader { void cancel() {} };
  FolderStatusLoader *folderStatusLoader_ = nullptr;
  std::optional<int> folderStatusRowsRevision_, folderStatusRetryAt_;
  struct Context { std::atomic_bool appInBackground = false; } context;
  bool sceneActive_ = true;
  bool failed_ = false;
  bool launching_ = false;
  bool reactivateSkinOnResume_ = false;
  Preview previewController_;
  Preview preview;
  Preview *previewAudio_ = &preview;
  Skin skin;
  Skin *skinSession_ = &skin;
  Input *skinTextInput_ = nullptr;
  Input skinTouchGesture_;
  int irExternalUrlGeneration_ = 0;
  ExternalUrlService *irExternalUrlService_ = nullptr;
  int preloadStops = 0;
  int selectionChanges = 0;
  int soundConfigurations = 0;
  void configureSoundServices() { ++soundConfigurations; }
  void stopPreloadWorker() { ++preloadStops; }
  void stopInputListening() {}
  void cancelDirectoryLoad() {}
  void continueDirectoryRestore() {}
  void startInputListening() {}
  void hideDecideOverlay() {}
  void syncToolbar() {}
  void reloadLibrary() {}
  void selectedBarMoved() { ++selectionChanges; }
  bool reactivateSkinAfterSettings() { return true; }
  void onPause();
  void onResume();
  void onApplicationBackgroundChanged(bool background);
};

SCENE_METHODS

int main() {
  MusicSelectScene scene;
  scene.context.appInBackground = true;
  scene.onApplicationBackgroundChanged(true);
  assert(!scene.preview.playing && scene.preview.suppressed);
  scene.preview.switchToPreview();
  assert(!scene.preview.playing);
  assert(scene.preloadStops == 0);
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  assert(!scene.skin.playing);
#endif
  scene.onApplicationBackgroundChanged(false);
  assert(scene.preview.resumes == 0);
  scene.context.appInBackground = false;
  scene.onApplicationBackgroundChanged(false);
  assert(scene.preview.playing && !scene.preview.suppressed);
  assert(scene.soundConfigurations == 1);
  assert(scene.selectionChanges == 1);
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  assert(scene.skin.playing);
#endif
  scene.preview.switchToPreview();
  scene.context.appInBackground = true;
  scene.onApplicationBackgroundChanged(true);
  assert(!scene.preview.playing);
  scene.onPause();
  assert(scene.preloadStops == 1);
  scene.context.appInBackground = false;
  scene.onApplicationBackgroundChanged(false);
  assert(!scene.preview.playing && scene.preview.resumes == 1);
  scene.context.appInBackground = true;
  scene.onResume();
  assert(!scene.preview.playing && scene.preview.suppressed);
  assert(scene.soundConfigurations == 1);
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  assert(!scene.skin.playing);
#endif
  scene.context.appInBackground = false;
  scene.onApplicationBackgroundChanged(false);
  assert(scene.preview.playing && scene.preview.resumes == 2);
  for (bool failed : {false, true}) {
    scene.onApplicationBackgroundChanged(true);
    scene.failed_ = failed;
    scene.launching_ = !failed;
    scene.onApplicationBackgroundChanged(false);
    assert(!scene.preview.playing && scene.preview.resumes == 2);
  }
  scene.failed_ = false;
  scene.launching_ = false;
  scene.onPause();
  scene.onResume();
  assert(scene.preview.playing && scene.preview.resumes == 3);
  assert(scene.soundConfigurations == 3);
}
