#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <stop_token>
#include <vector>

constexpr const char *kSkinSoundAssetRoot = "assets";
int previewCreated = 0;
int previewAlive = 0;
int systemAlive = 0;
int scopesClosed = 0;
void SDL_Log(const char *, ...) {}
std::string fspath_to_utf8(const std::filesystem::path &path) { return path.string(); }
void *StartIOSSecurityScopedResource(const std::string &, const std::string &,
                                    std::string &, std::string &) { return &scopesClosed; }
void StopIOSSecurityScopedResource(void *) {
  assert(previewAlive == 0 && systemAlive == 0);
  ++scopesClosed;
}
struct MusicSelectPreviewAudioService {
  std::filesystem::path path;
  MusicSelectPreviewAudioService(int, const std::filesystem::path &value) : path(value) {
    ++previewCreated;
    ++previewAlive;
  }
  ~MusicSelectPreviewAudioService() { --previewAlive; }
  void silenceAndWait() {}
};
template <typename Play, typename Stop>
int musicSelectPreviewAudioPort(int, const std::filesystem::path &, Play, Stop) { return 0; }
int musicSelectSkinSoundPlayback(int) { return 0; }
namespace skin {
enum class MusicSelectSystemSound { Select };
std::optional<std::filesystem::path> musicSelectSystemSoundPath(
    const std::vector<std::filesystem::path> &roots, MusicSelectSystemSound) {
  return roots.front() / "select.wav";
}
struct SkinSystemSoundService {
  std::vector<std::filesystem::path> roots;
  SkinSystemSoundService(const std::vector<std::filesystem::path> &value, int) : roots(value) {
    ++systemAlive;
  }
  ~SkinSystemSoundService() { --systemAlive; }
};
}
struct MusicSelectScene {
  struct Context {
    int systemAudio() { return 0; }
    struct Settings { std::string skinSelectSoundSetPath, skinSelectSoundSetBookmark; } settings;
    struct Jukebox {
      struct Rate { int percent = 100; bool neutral() const { return percent == 100; } } rate;
      struct Result { bool success = true; };
      int stops = 0;
      Rate playbackRate() const { return rate; }
      Result stop() { ++stops; return {}; }
      bool setPlaybackRate(Rate value, std::string &) { rate = value; return true; }
    } jukebox;
    std::atomic_bool appInBackground = false;
    std::atomic_bool storageMaintenanceActive = false;
  } context;
  struct Controller { void reset() {} } previewController_;
  std::unique_ptr<MusicSelectPreviewAudioService> previewAudio_;
  std::unique_ptr<skin::SkinSystemSoundService> systemSound_;
  std::string soundSetPath_, soundSetBookmark_;
  void *soundSetFolderAccessHandle_ = nullptr;
  void onApplicationBackgroundChanged(bool) {}
  struct { void Stop() {} } chartPreviewPlayer_;
  bool playChartPreview(const std::filesystem::path &, bool,
                        const std::shared_ptr<std::atomic_bool> &, std::stop_token) { return false; }
  void configureSoundServices();
};
SCENE_METHODS
int main() {
  MusicSelectScene scene;
  scene.context.settings = {" first ", "bookmark1"};
  scene.context.jukebox.rate.percent = 150;
  scene.configureSoundServices();
  assert(scene.previewAudio_->path == std::filesystem::path("first/select.wav"));
  assert(scene.context.jukebox.rate.percent == 150 && scene.context.jukebox.stops == 0 &&
         "system sound setup does not reset or stop gameplay");
  assert(scene.systemSound_->roots.front() == "first");
  scene.configureSoundServices();
  assert(previewCreated == 1 && "unchanged settings must preserve the services");
  scene.context.jukebox.rate.percent = 200;
  scene.configureSoundServices();
  assert(scene.context.jukebox.rate.percent == 200 && scene.context.jukebox.stops == 0 &&
         previewCreated == 1 && "resuming sound services is independent of retained gameplay rate");
  scene.context.settings = {"second", "bookmark2"};
  scene.configureSoundServices();
  assert(previewCreated == 2 && previewAlive == 1 && systemAlive == 1);
  assert(scene.previewAudio_->path == std::filesystem::path("second/select.wav"));
  assert(scene.systemSound_->roots.front() == "second");
  scene.context.settings.skinSelectSoundSetBookmark = "renewed";
  scene.configureSoundServices();
  assert(previewCreated == 3);
  scene.context.settings = {};
  scene.configureSoundServices();
  assert(scene.previewAudio_->path == std::filesystem::path("assets/select.wav"));
  assert(scene.systemSound_->roots.size() == 1 && scene.systemSound_->roots.front() == "assets");
#if TARGET_OS_IOS
  assert(scopesClosed == 3);
#endif
}
