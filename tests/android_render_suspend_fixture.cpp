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
bool submittedFrameSuspended = false;
bool pendingPresent = true;
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
  // frame() waits for the prior submit, then hands off the next one. The
  // prior active frame's present can still be pending when the first suspend
  // frame is handed off; only waiting for that suspended submit retires it.
  if (submittedFrameSuspended) pendingPresent = false;
  if (!suspended) pendingPresent = true;
  submittedFrameSuspended = suspended;
  ++frames;
}
}
struct TestScene {
  bool playing = false, paused = false;
  int overflows = 0;
  void onInputQueueOverflow() { ++overflows; }
  bool continuesAudioInBackground() const { return playing && !paused; }
  void onApplicationBackgroundChanged(bool) {}
};
namespace scene_event_routing {
void dispatchApplicationBackgroundChange(TestScene *scene, bool background) {
  if (scene) scene->onApplicationBackgroundChanged(background);
}
}
struct Context {
  struct Registry { void reconcileSdlDevices() {} void clearSdlInputState() {} } inputDeviceRegistry;
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
int surfacePauseAcknowledgements = 0;
void NotifyAndroidExternalActivityRenderPaused() {
  assert(!bgfx::pendingPresent &&
         "external activity must not destroy the surface before the last present");
  ++pauseAcknowledgements;
}
void NotifyAndroidSurfaceRenderPaused() {
  assert(!bgfx::pendingPresent &&
         "surface destruction must wait for the last present");
  ++surfacePauseAcknowledgements;
}

bool surfacePauseRequested = false, externalPauseRequested = false;
bool IsAndroidSurfaceRenderPauseRequested() { return surfacePauseRequested; }
bool IsAndroidExternalActivityRenderPauseRequested() { return externalPauseRequested; }
namespace platform {
bool active = true;
bool applicationActive() { return active; }
void windowSize(void *, int *width, int *height) { *width = 2400; *height = 1080; }
}
int main() {
  Context context;
  bool androidRenderSuspended = false, androidResumeResizePending = false;
  uint32_t s_bgfxResetFlags = 128, activeBgfxResetFlags = 128;
  bool nativeWindowReady = true;
  auto refreshAndroidBgfxPlatformData = [&]() { return nativeWindowReady; };
  auto retireAndroidPreviousWindow = []() {};
  PRODUCTION_SUSPEND;
  bool androidSystemSuspended = false;
  PRODUCTION_SYNC;
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
  assert(androidRenderSuspended && bgfx::suspended && !bgfx::pendingPresent);
  assert(pauseAcknowledgements == 1 && surfacePauseAcknowledgements == 1);
  assert(applyAndroidRenderSuspend(true));
  assert(pauseAcknowledgements == 2 && surfacePauseAcknowledgements == 2);
  assert(bgfx::resets == 1 && bgfx::frames == 2);
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
  assert(bgfx::resets == 2 && bgfx::frames == 3 && bgfx::pendingPresent);

  context.replayVideoExportActive = true;
  assert(!applyAndroidRenderSuspend(true));
  assert(pauseAcknowledgements == 2 && surfacePauseAcknowledgements == 2 &&
         bgfx::pendingPresent);
  context.replayVideoExportActive = false;
  assert(applyAndroidRenderSuspend(true));
  assert(pauseAcknowledgements == 3 && surfacePauseAcknowledgements == 3 &&
         !bgfx::pendingPresent);
  assert(bgfx::resets == 3 && bgfx::frames == 5);
  // Overflow supplies a synthetic focus loss without a real OS lifecycle change.
  const bool pressureRecovery = true;
  void *s_window = nullptr;
  int resizedWidth = 0, resizedHeight = 0;
  auto deferWindowResize = [&](int width, int height) { resizedWidth = width; resizedHeight = height; };
  auto recoverOverflow = [&] {
    PRODUCTION_RECOVERY
  };
  androidSystemSuspended = true;
  setAppBackground(true);
  recoverOverflow();
  assert(scene.overflows == 1 && "recovery must notify the scene before reopening input");
  assert(!syncAndroidRenderSuspend() && !androidRenderSuspended &&
         "foreground event overflow must not leave Android rendering suspended");
  assert(!context.appInBackground && resizedWidth == 2400 && resizedHeight == 1080);
  for (bool surface : {false, true}) {
    surfacePauseRequested = surface;
    externalPauseRequested = !surface;
    androidSystemSuspended = true;
    recoverOverflow();
    assert(syncAndroidRenderSuspend() && "overflow must preserve explicit native pause requests");
  }
  surfacePauseRequested = externalPauseRequested = false;
  platform::active = false;
  recoverOverflow();
  assert(syncAndroidRenderSuspend() && context.appInBackground);
  std::cout << "Android renderer suspend drain and resume target restoration passed\n";
}
