#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <chrono>
#include <optional>
#define TARGET_OS_ANDROID 1

constexpr uint32_t BGFX_RESET_SUSPEND = 0x80000000;
namespace rendering { int render_width = 2400, render_height = 1080; }
namespace bgfx {
bool suspended = false;
bool bgaTargetsBound = true;
int resets = 0, frames = 0;
void reset(int width, int height, uint32_t flags) {
  assert(width == 2400 && height == 1080);
  // Context::reset clears every view's framebuffer even at unchanged dimensions.
  bgaTargetsBound = false;
  suspended = (flags & BGFX_RESET_SUSPEND) != 0;
  ++resets;
}
void frame() {
  assert((suspended || bgaTargetsBound) &&
         "first resumed frame must render BGA into its restored offscreen target");
  ++frames;
}
}
struct TestScene {
  bool playing = false, paused = false;
  bool continuesAudioInBackground() const { return playing && !paused; }
  void onApplicationBackgroundChanged(bool) {}
};
namespace scene_event_routing {
void dispatchApplicationBackgroundChange(TestScene *scene, bool background) {
  if (scene) scene->onApplicationBackgroundChanged(background);
}
}
struct Context {
  struct Display {
    struct Message { bool empty() const { return true; } std::string resolve() const { return {}; } };
    struct Result { Message message; };
    std::optional<Result> onFocusLost() { return {}; }
  };
  Display *displaySettingsManager = nullptr;
  std::atomic_bool appInBackground{false};
  struct Jukebox {
    bool suspended = false, visualsSuspended = false;
    long long time = 0, visualTime = -1;
    Jukebox &audioRuntime() { return *this; }
    void setApplicationSuspended(bool value) { suspended = value; }
    void setVisualsSuspended(bool value) { visualsSuspended = value; }
    long long getTimeMicros() const { return time; }
    void seekVisualsToSongTime(long long value) { visualTime = value; }
  } jukebox;
  struct Pacer { void reset(std::chrono::steady_clock::time_point) {} } framePacer;
  void setIrApplicationActive(bool) {}

  std::atomic_bool replayVideoExportActive{false};
  std::atomic_uint32_t bgfxResetFlags{0};
  std::mutex bgfxRenderMutex;
  void restoreGameplayRenderViews() { bgfx::bgaTargetsBound = true; }
};
void SDL_Log(const char *, ...) {}
int pauseAcknowledgements = 0;
void NotifyAndroidExternalActivityRenderPaused() { ++pauseAcknowledgements; }

int main() {
  Context context;
  bool androidRenderSuspended = false, androidResumeResizePending = false;
  uint32_t s_bgfxResetFlags = 128, activeBgfxResetFlags = 128;
  bool nativeWindowReady = true;
  auto refreshAndroidBgfxPlatformData = [&]() { return nativeWindowReady; };
  PRODUCTION_SUSPEND;
  TestScene scene;
  struct { TestScene *currentScene; } sceneManager{&scene};
  auto lastFrameTime = std::chrono::steady_clock::now();
  PRODUCTION_LIFECYCLE;
  setAppBackground(true);
  assert(context.jukebox.suspended && context.jukebox.visualsSuspended);
  setAppBackground(false);
  scene.playing = true;
  setAppBackground(true);
  assert(!context.jukebox.suspended && context.jukebox.visualsSuspended);
  context.jukebox.time = 42'000'000;
  setAppBackground(false);
  assert(!context.jukebox.suspended && !context.jukebox.visualsSuspended &&
         context.jukebox.visualTime == 42'000'000);
  scene.paused = true;
  setAppBackground(true);
  assert(context.jukebox.suspended && scene.paused);
  setAppBackground(false);
  assert(!context.jukebox.suspended && scene.paused);


  assert(applyAndroidRenderSuspend(true));
  assert(androidRenderSuspended && bgfx::suspended);
  assert(pauseAcknowledgements == 1);
  assert(applyAndroidRenderSuspend(true));
  assert(bgfx::resets == 1 && bgfx::frames == 1);
  nativeWindowReady = false;
  assert(!applyAndroidRenderSuspend(false));
  assert(androidRenderSuspended && bgfx::resets == 1);
  nativeWindowReady = true;
  context.replayVideoExportActive = true;
  assert(!applyAndroidRenderSuspend(false));
  assert(androidRenderSuspended && bgfx::resets == 1);
  context.replayVideoExportActive = false;
  // Home/resume did not resize the window, so resize cannot repair the targets.
  assert(applyAndroidRenderSuspend(false));
  assert(!androidRenderSuspended && !bgfx::suspended);
  assert(androidResumeResizePending && bgfx::bgaTargetsBound);
  assert(context.bgfxResetFlags == 128 && bgfx::resets == 2);
  assert(applyAndroidRenderSuspend(false));
  assert(bgfx::resets == 2 && bgfx::frames == 2);
  std::cout << "Android renderer resume target restoration passed\n";
}
