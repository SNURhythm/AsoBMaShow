#include "rendering/PortraitPlayfieldFraming.h"
#include "rendering/UiSafeArea.h"
#include "settings/PresentationOrientationState.h"
#include "perf/LatencyTelemetry.h"
#include "targets.h"
#include "AppDatabaseInitializer.h"
#include "ApplicationResultRecovery.h"
#include "ApplicationStartup.h"
#include "DifficultyTableImporter.h"
#include "input/InputLifecycle.h"
#include "replay/ReplayFileReconciler.h"
#include "bgfx_helper.h"
#include "rendering/BgfxInitLimits.h"
#include "rendering/ShaderManager.h"
#include "./audio/decoder.h"
#include "bx/math.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_video.h>
#if __APPLE__
#include <SDL3/SDL_metal.h>
#endif
#include "main.h"
#include "path.h"
#include "scene/MainMenuScene.h"
#include "scene/IntroScene.h"
#include "scene/SceneEventRouting.h"
#include "scene/play/GameplayGeometry.h"
#include "scene/SettingsScene.h"
#include "scene/SceneManager.h"
#include "view/TextInputBox.h"
#include "view/FontCacheSession.h"
#include "view/ImageView.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>
#include <bgfx/platform.h>
#include <bx/platform.h>
#include "rendering/common.h"
#include "rendering/PostProcessPipeline.h"
#include "rendering/BlurPass.h"
#include "rendering/UniformCache.h"
#include "context.h"
#include "audio/AudioWrapper.h"
#include "video/SDLDisplayBackend.h"
#ifdef _WIN32
#include <windows.h>

#elif __APPLE__

#include "TargetConditionals.h"
#if TARGET_OS_IPHONE
#include "iOSNatives.hpp"
#include "video/IOSPresentationPacing.h"
#include "input/IOSTouchInput.h"
#include "input/NativeRawTouchInput.h"
#include <SDL3/SDL_uikit_rawtouch.h>
// define something for iphone
#include <dirent.h>
#include <sys/stat.h>
#else
// define something for OSX
#include "MacNatives.h"
#include <dirent.h>
#include <sys/stat.h>
#endif
#elif defined(__ANDROID__)
#include "AndroidNatives.h"
#include "input/AndroidInputHints.h"
#include "input/AndroidInputTimestamp.h"
#include "input/NativeRawTouchInput.h"
#include <dirent.h>
#include <sys/system_properties.h>
#include <sys/stat.h>
#elif __linux
// linux
#include <dirent.h>
#include <sys/stat.h>
#elif __unix // all unices not caught above
// Unix
#elif __posix
// POSIX
#endif
#include "rendering/Camera.h"
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

// CMake defines this for desktop and Android targets. The iOS app is built by
// its generated Xcode project, so keep telemetry opt-in when that build system
// does not provide the definition.
#ifndef ASOBMASHOW_ENABLE_PERF_TELEMETRY
#define ASOBMASHOW_ENABLE_PERF_TELEMETRY 0
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

#if defined(DEBUG) || defined(_DEBUG)
#define APP_DEBUG_LOG(...) SDL_Log(__VA_ARGS__)
#else
#define APP_DEBUG_LOG(...) ((void)0)
#endif

bgfx::VertexLayout rendering::PosColorVertex::ms_decl;
bgfx::VertexLayout rendering::PosTexVertex::ms_decl;
bgfx::VertexLayout rendering::PosTexCoord0Vertex::ms_decl;

static SDL_Window *s_window = nullptr;
#if TARGET_OS_ANDROID
static AndroidNativeWindowOwner s_androidNativeWindow;
static AndroidNativeWindowOwner s_androidPreviousWindow;
#endif
static rendering::PostProcessPipeline s_postProcess;
static rendering::BlurPass *s_blurPass = nullptr;
static float s_renderScale = 1.0f;
static uint32_t s_bgfxResetFlags = 0;
#if TARGET_OS_IPHONE
static void *s_iosMetalLayer = nullptr;
#endif

namespace {

constexpr float kDefaultRenderScale = 1.0f;

float resolveRenderScale() { return kDefaultRenderScale; }

uint32_t parseMsaaFlag(int samples) {
  switch (samples) {
  case 0:
    return BGFX_RESET_NONE;
  case 2:
    return BGFX_RESET_MSAA_X2;
  case 4:
    return BGFX_RESET_MSAA_X4;
  case 8:
    return BGFX_RESET_MSAA_X8;
  case 16:
    return BGFX_RESET_MSAA_X16;
  default:
    return BGFX_RESET_NONE;
  }
}

std::filesystem::path
absolutePathOrOriginal(const std::filesystem::path &path) {
  std::error_code error;
  const std::filesystem::path absolutePath =
      std::filesystem::absolute(path, error);
  return error ? path : absolutePath;
}

void changeWorkingDirectoryToExecutableDir(
    const std::filesystem::path &exePath) {
  if (exePath.empty()) {
    return;
  }

  std::filesystem::path exeDir = exePath.parent_path();
#if TARGET_OS_OSX
  // SDL3 IO no longer searches bundle resources implicitly. SDL_GetBasePath
  // resolves Resources for a .app and the executable directory otherwise.
  if (const char *basePath = SDL_GetBasePath()) exeDir = basePath;
#endif
  if (exeDir.empty()) {
    return;
  }

  std::error_code error;
  const bool dirExists = std::filesystem::exists(exeDir, error);
  if (error || !dirExists) {
    if (error) {
      APP_DEBUG_LOG("Could not inspect executable directory %s: %s",
                    fspath_to_utf8(exeDir).c_str(), error.message().c_str());
    }
    return;
  }

  std::filesystem::current_path(exeDir, error);
  if (error) {
    APP_DEBUG_LOG("Could not change working directory to %s: %s",
                  fspath_to_utf8(exeDir).c_str(), error.message().c_str());
    return;
  }

  APP_DEBUG_LOG("Changed working directory to: %s",
                fspath_to_utf8(exeDir).c_str());
}

uint32_t resolveResetFlags() {
#if TARGET_OS_OSX || TARGET_OS_ANDROID
  // Avoid a multisampled native-resolution backbuffer and its resolve on
  // Android. Text and skin images retain their own texture filtering.
  constexpr int msaaSamples = 0;
#else
  constexpr int msaaSamples = 2;
#endif
  uint32_t flags = parseMsaaFlag(msaaSamples);

  if (TARGET_PLATFORM == iOS) {
    // Single-threaded Metal must submit the freshly encoded frame now.
    flags |= BGFX_RESET_VSYNC | BGFX_RESET_FLIP_AFTER_RENDER;
  }
  if (TARGET_PLATFORM == Android) {
    flags |= BGFX_RESET_VSYNC;
  }
  return flags;
}

int scaledDimension(int logicalSize) {
  return std::max(
      1, static_cast<int>(std::lround(static_cast<double>(logicalSize) *
                                      static_cast<double>(s_renderScale))));
}

void getWindowDrawableSize(SDL_Window *window, int logicalW, int logicalH,
                           int &renderW, int &renderH) {
  renderW = 0;
  renderH = 0;
  if (window != nullptr) {
    SDL_GetWindowSizeInPixels(window, &renderW, &renderH);
  }
  if (renderW <= 0 || renderH <= 0) {
    renderW = scaledDimension(logicalW);
    renderH = scaledDimension(logicalH);
  }
}

class CallbackRendererDisplayTransaction final
    : public display::IRendererDisplayTransaction {
public:
  using Synchronizer =
      std::function<bool(std::uint32_t, std::string &errorMessage)>;

  CallbackRendererDisplayTransaction(
      std::shared_ptr<display::RendererAccessCoordinator::DisplayReservation>
          reservationValue,
      Synchronizer synchronizeValue)
      : reservation(std::move(reservationValue)),
        synchronizeCallback(std::move(synchronizeValue)) {}

  bool synchronize(std::uint32_t resetFlags,
                   std::string &errorMessage) override {
    return synchronizeCallback(resetFlags, errorMessage);
  }

private:
  std::shared_ptr<display::RendererAccessCoordinator::DisplayReservation>
      reservation;
  Synchronizer synchronizeCallback;
};

#if TARGET_OS_IPHONE
void getIOSMetalDrawableSize(SDL_Window *window, int logicalW, int logicalH,
                             int &renderW, int &renderH) {
  renderW = 0;
  renderH = 0;
  if (window != nullptr) {
    SDL_GetWindowSizeInPixels(window, &renderW, &renderH);
  }
  if (renderW <= 0 || renderH <= 0) {
    getWindowDrawableSize(window, logicalW, logicalH, renderW, renderH);
    return;
  }

  // iPad Display Zoom can expose a larger fullscreen display mode than SDL's
  // native-scale Metal drawable. Render at that mode to avoid compositor
  // upscaling during screenshots and app-focus transitions.
  int preferredW = 0;
  int preferredH = 0;
  if (s_iosMetalLayer != nullptr &&
      GetIOSPreferredFullscreenDrawableSize(renderW, renderH, logicalW,
                                            logicalH, preferredW, preferredH) &&
      SetIOSMetalLayerDrawableSize(s_iosMetalLayer, preferredW, preferredH)) {
    APP_DEBUG_LOG("iOS display-mode drawable size: %d x %d (SDL: %d x %d)",
                  preferredW, preferredH, renderW, renderH);
    renderW = preferredW;
    renderH = preferredH;
  }
}
#endif

} // namespace

// static rendering::PosColorVertex cubeVertices[] = {
//     {-1.0f, 1.0f, 1.0f, 0xff000000},   {1.0f, 1.0f, 1.0f, 0xff0000ff},
//     {-1.0f, -1.0f, 1.0f, 0xff00ff00},  {1.0f, -1.0f, 1.0f, 0xff00ffff},
//     {-1.0f, 1.0f, -1.0f, 0xffff0000},  {1.0f, 1.0f, -1.0f, 0xffff00ff},
//     {-1.0f, -1.0f, -1.0f, 0xffffff00}, {1.0f, -1.0f, -1.0f, 0xffffffff},
// };
//
// static const uint16_t cubeTriList[] = {
//     0, 1, 2, 1, 3, 2, 4, 6, 5, 5, 6, 7, 0, 2, 4, 4, 2, 6,
//     1, 5, 3, 5, 7, 3, 0, 4, 1, 4, 5, 1, 2, 3, 6, 6, 3, 7,
// };
int rendering::window_width = rendering::design_width;
int rendering::window_height = rendering::design_height;
int rendering::render_width = 1;
int rendering::render_height = 1;
float rendering::widthScale = 1.0f;
float rendering::heightScale = 1.0f;
float rendering::ui_scale_x = 1.0f;
float rendering::ui_scale_y = 1.0f;
int rendering::ui_offset_x = 0;
int rendering::ui_offset_y = 0;
int rendering::ui_view_width = rendering::design_width;
int rendering::ui_view_height = rendering::design_height;
Camera *rendering::main_camera = nullptr;
Camera rendering::game_camera{rendering::main_view};
void rendering::updateUIScale(int renderW, int renderH) {
  if (renderW <= 0 || renderH <= 0) {
    return;
  }
  render_width = renderW;
  render_height = renderH;
  window_width = renderW < renderH ? design_height : design_width;
  ui_scale_x = static_cast<float>(renderW) / static_cast<float>(window_width);
  ui_scale_y = ui_scale_x;
  window_height = static_cast<int>(renderH / ui_scale_y);
  ui_view_width = renderW;
  ui_view_height = renderH;
  ui_offset_x = 0;
  ui_offset_y = 0;
}

#include <deque>
#include <algorithm>

class FPSCounter {
public:
  void addFrame(float deltaTime) {
    if (deltaTime <= 0.0f) {
      return;
    }
    // Treat very large deltas as discontinuities (e.g. resize/app switch).
    if (deltaTime > MAX_SAMPLE_DELTA) {
      frameTimes.clear();
      totalTime = 0.0f;
      return;
    }
    frameTimes.push_back(deltaTime);
    totalTime += deltaTime;

    // Remove old frames outside the window
    while (totalTime > WINDOW_SIZE && !frameTimes.empty()) {
      totalTime -= frameTimes.front();
      frameTimes.pop_front();
    }
  }

  float getAverageFPS() const {
    if (frameTimes.empty() || totalTime <= 0.0f)
      return 0.0f;
    return static_cast<float>(frameTimes.size()) / totalTime;
  }

  float get1PercentLowFPS() const {
    if (frameTimes.empty()) {
      return 0.0f;
    }

    std::vector<float> samples(frameTimes.begin(), frameTimes.end());
    size_t worstCount = std::max<size_t>(1, samples.size() / 100);
    worstCount = std::min(worstCount, samples.size());
    std::nth_element(samples.begin(), samples.begin() + (worstCount - 1),
                     samples.end(), std::greater<float>());

    double worstFrameTimeSum = 0.0;
    for (size_t i = 0; i < worstCount; ++i) {
      worstFrameTimeSum += samples[i];
    }
    const double avgWorstFrameTime =
        worstFrameTimeSum / static_cast<double>(worstCount);
    if (avgWorstFrameTime <= 0.000001)
      return 0.0f;

    return static_cast<float>(1.0 / avgWorstFrameTime);
  }

private:
  std::deque<float> frameTimes;
  float totalTime = 0.0f;
  static constexpr float WINDOW_SIZE = 5.0f;       // 5 second window
  static constexpr float MAX_SAMPLE_DELTA = 0.25f; // drop discontinuities
};

#if TARGET_OS_ANDROID
static std::string getAndroidSystemProperty(const char *name) {
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get(name, value) <= 0) {
    return {};
  }
  return value;
}

static bool isAndroidEmulator() {
  const std::string qemu = getAndroidSystemProperty("ro.kernel.qemu");
  if (qemu == "1") {
    return true;
  }

  const std::string hardware = getAndroidSystemProperty("ro.hardware");
  return hardware == "ranchu" || hardware == "goldfish";
}

static int getAndroidSdkVersion() {
  const std::string sdk = getAndroidSystemProperty("ro.build.version.sdk");
  if (sdk.empty()) {
    return 0;
  }
  char *end = nullptr;
  const long value = std::strtol(sdk.c_str(), &end, 10);
  if (end == sdk.c_str() || value <= 0 || value > 1000) {
    return 0;
  }
  return static_cast<int>(value);
}

#endif

static uint32_t withoutMsaaResetFlags(uint32_t flags) {
  constexpr uint32_t msaaMask = BGFX_RESET_MSAA_X2 | BGFX_RESET_MSAA_X4 |
                                BGFX_RESET_MSAA_X8 | BGFX_RESET_MSAA_X16;
  return flags & ~msaaMask;
}

static int runApplication(const bgfx::Init &bgfxInit) {
  SDL_Log("bgfx_init: %d x %d", bgfxInit.resolution.width,
          bgfxInit.resolution.height);
  std::vector<bgfx::RendererType::Enum> rendererCandidates;
#if TARGET_OS_ANDROID
  const bool androidEmulator = isAndroidEmulator();
  const int androidSdkVersion = getAndroidSdkVersion();
  const bool skipAndroidEmulatorVulkan =
      androidEmulator && androidSdkVersion >= 33;
  if (skipAndroidEmulatorVulkan) {
    SDL_Log("Android emulator API %d detected; skipping Vulkan stub renderer",
            androidSdkVersion);
  } else {
    rendererCandidates.push_back(bgfx::RendererType::Vulkan);
  }
  rendererCandidates.push_back(bgfx::RendererType::OpenGLES);
#elif __APPLE__
  rendererCandidates.push_back(bgfx::RendererType::Metal);
#else
  rendererCandidates.push_back(bgfx::RendererType::Count);
#endif

  bgfx::Init selectedInit = bgfxInit;
  for (const auto rendererType : rendererCandidates) {
    selectedInit.type = rendererType;
#if TARGET_OS_ANDROID
    selectedInit.resolution.formatColor =
        rendererType == bgfx::RendererType::Vulkan ? bgfx::TextureFormat::RGBA8
                                                   : bgfx::TextureFormat::BGRA8;
#endif
    SDL_Log("Trying bgfx renderer: %s",
            rendererType == bgfx::RendererType::Count
                ? "auto"
                : bgfx::getRendererName(rendererType));
    if (bgfx::init(selectedInit)) {
      SDL_Log("bgfx renderer: %s",
              bgfx::getRendererName(bgfx::getRendererType()));
      // Keep debug rendering disabled in normal runtime to avoid perturbing
      // frame pacing and post-process output.
      // bgfx::setDebug(BGFX_DEBUG_TEXT);

#if TARGET_OS_ANDROID
      SetAndroidRendererActive(true);
#endif
      const int runExitCode = run();
      rendering::ShaderManager::getInstance().release();
      rendering::UniformCache::getInstance().destroyAll();
      bgfx::shutdown();
#if TARGET_OS_ANDROID
      SetAndroidRendererActive(false);
#endif
      return runExitCode;
    }
    SDL_Log("bgfx::init failed for renderer: %s",
            rendererType == bgfx::RendererType::Count
                ? "auto"
                : bgfx::getRendererName(rendererType));
  }
  return EXIT_FAILURE;
}

int main(int argv, char **args) {
  // Set working directory to executable's directory
  std::filesystem::path exePath;
#ifdef _WIN32
  char exePathBuf[MAX_PATH];
  DWORD len = GetModuleFileNameA(nullptr, exePathBuf, MAX_PATH);
  if (len > 0 && len < MAX_PATH) {
    exePath = std::filesystem::path(exePathBuf);
  } else {
    // Fallback to args[0] if GetModuleFileName fails
    if (argv > 0 && args[0] != nullptr) {
      exePath = std::filesystem::path(args[0]);
      if (!exePath.is_absolute()) {
        exePath = absolutePathOrOriginal(exePath);
      }
    }
  }
#elif TARGET_OS_IPHONE
  // iOS: executable is in the app bundle, use bundle path
  // For iOS, we typically want the Documents directory, not the executable path
  // So we'll skip changing directory on iOS
#elif TARGET_OS_OSX
  // macOS: use _NSGetExecutablePath
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> exePathBuf(size);
  if (_NSGetExecutablePath(exePathBuf.data(), &size) == 0) {
    exePath = std::filesystem::path(exePathBuf.data());
  } else {
    // Fallback to args[0]
    if (argv > 0 && args[0] != nullptr) {
      char resolved[PATH_MAX];
      if (realpath(args[0], resolved) != nullptr) {
        exePath = std::filesystem::path(resolved);
      } else {
        exePath = std::filesystem::path(args[0]);
        if (!exePath.is_absolute()) {
          exePath = absolutePathOrOriginal(exePath);
        }
      }
    }
  }
#elif defined(__ANDROID__)
  // Android assets and app-private storage are resolved through SDL and
  // AndroidNatives; do not chdir into /proc/self/exe.
#elif __linux__
  // Linux: use /proc/self/exe
  char exePathBuf[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", exePathBuf, PATH_MAX - 1);
  if (len != -1) {
    exePathBuf[len] = '\0';
    exePath = std::filesystem::path(exePathBuf);
  } else {
    // Fallback to args[0]
    if (argv > 0 && args[0] != nullptr) {
      char resolved[PATH_MAX];
      if (realpath(args[0], resolved) != nullptr) {
        exePath = std::filesystem::path(resolved);
      } else {
        exePath = std::filesystem::path(args[0]);
        if (!exePath.is_absolute()) {
          exePath = absolutePathOrOriginal(exePath);
        }
      }
    }
  }
#endif

  changeWorkingDirectoryToExecutableDir(exePath);

#ifdef _WIN32
  // search dll in ./lib
  SetDllDirectoryA("lib");
#endif
  // set QoS class for macOS, for best performance
#if TARGET_OS_OSX
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  rendering::main_camera = &rendering::game_camera;
  SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
  SDL_SetHint(SDL_HINT_ORIENTATIONS,
              "Portrait PortraitUpsideDown LandscapeLeft LandscapeRight");
#if TARGET_OS_IPHONE
  // UIKit exposes a physical trackpad as a mouse. SDL otherwise mirrors every
  // mouse press as a synthetic finger press, which would activate UI controls
  // twice when the application accepts native touch input too.
  SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "ambient");
#endif
#if TARGET_OS_ANDROID
  input::android::configurePointerHints();
  // Keep the CPU gameplay tick alive; the main loop suspends bgfx explicitly.
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif
  // print bgfx version
  APP_DEBUG_LOG("bgfx version: %d OSX:%d", BGFX_API_VERSION, BX_PLATFORM_OSX);
  // print libsdl version
  const int linked = SDL_GetVersion();
  APP_DEBUG_LOG("SDL compile version: %d.%d.%d", SDL_MAJOR_VERSION,
                SDL_MINOR_VERSION, SDL_MICRO_VERSION);
  APP_DEBUG_LOG("SDL link version: %d.%d.%d", SDL_VERSIONNUM_MAJOR(linked),
                SDL_VERSIONNUM_MINOR(linked), SDL_VERSIONNUM_MICRO(linked));

#if TARGET_OS_OSX
  setSmoothScrolling(true);
#endif
  using std::cerr;
  using std::endl;

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    cerr << "SDL_Init Error: " << SDL_GetError() << endl;
    return EXIT_FAILURE;
  }
#if TARGET_OS_ANDROID
  SDL_SetEventFilter(&input::android::timestampFilter, nullptr);
#endif
  s_renderScale = resolveRenderScale();
  s_bgfxResetFlags = resolveResetFlags();
#if TARGET_OS_ANDROID
  if (isAndroidEmulator()) {
    const uint32_t adjustedResetFlags = withoutMsaaResetFlags(s_bgfxResetFlags);
    if (adjustedResetFlags != s_bgfxResetFlags) {
      SDL_Log("Android emulator detected; disabling bgfx MSAA reset flags");
    }
    s_bgfxResetFlags = adjustedResetFlags;
  }
#endif
  SDL_Log("Render scale: %.2f | bgfx reset flags: 0x%08x", s_renderScale,
          s_bgfxResetFlags);

  int windowCreateWidth = 1280;
  int windowCreateHeight = 720;
  SDL_WindowFlags windowFlags = SDL_WINDOW_RESIZABLE;
  if (TARGET_PLATFORM == iOS || TARGET_PLATFORM == Android) {
    // Use the current screen size in either launch orientation. An exclusive
    // mode based on the initial landscape dimensions can fail in portrait.
    windowFlags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS;
  }
  if (TARGET_PLATFORM == iOS || TARGET_PLATFORM == MacOS) {
    windowFlags |= SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  } else if (TARGET_PLATFORM == Android) {
    windowFlags |= SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  }
  SDL_Window *win = SDL_CreateWindow("AsoBMaShow", windowCreateWidth,
                                     windowCreateHeight, windowFlags);
  if (win == nullptr) {
    cerr << "SDL_CreateWindow Error: " << SDL_GetError() << endl;
    TextInputBox::releaseCachedCursors();
    SDL_Quit();
    return EXIT_FAILURE;
  }
  s_window = win;
  int windowLogicalWidth = 0;
  int windowLogicalHeight = 0;
  SDL_GetWindowSize(win, &windowLogicalWidth, &windowLogicalHeight);
  if (windowLogicalWidth <= 0 || windowLogicalHeight <= 0) {
    windowLogicalWidth = windowCreateWidth;
    windowLogicalHeight = windowCreateHeight;
  }
  APP_DEBUG_LOG("Window size (logical): %d x %d", windowLogicalWidth,
                windowLogicalHeight);

#if TARGET_OS_IPHONE || TARGET_OS_ANDROID
  int rw = 0, rh = 0;
  getWindowDrawableSize(win, windowLogicalWidth, windowLogicalHeight, rw, rh);
  rendering::widthScale =
      static_cast<float>(rw) / static_cast<float>(windowLogicalWidth);
  rendering::heightScale =
      static_cast<float>(rh) / static_cast<float>(windowLogicalHeight);
  APP_DEBUG_LOG("Drawable size: %d x %d", rw, rh);
  APP_DEBUG_LOG("Drawable scale: %f x %f", rendering::widthScale,
                rendering::heightScale);
  rendering::updateUIScale(rw, rh);
#else
  int initialRenderW = 0;
  int initialRenderH = 0;
  getWindowDrawableSize(win, windowLogicalWidth, windowLogicalHeight,
                        initialRenderW, initialRenderH);
  rendering::widthScale = static_cast<float>(initialRenderW) /
                          static_cast<float>(windowLogicalWidth);
  rendering::heightScale = static_cast<float>(initialRenderH) /
                           static_cast<float>(windowLogicalHeight);
  rendering::updateUIScale(initialRenderW, initialRenderH);
  APP_DEBUG_LOG("Render size: %d x %d (logical: %d x %d, scale %.2f)",
                initialRenderW, initialRenderH, windowLogicalWidth,
                windowLogicalHeight, s_renderScale);
#endif
  bgfx::PlatformData pd{};
  SdlMetalViewOwner metalView;
#if TARGET_OS_ANDROID
  s_androidNativeWindow = acquireAndroidNativeWindow(win);
  pd.nwh = s_androidNativeWindow.get();
  if (pd.nwh == nullptr) {
#else
  if (!setup_bgfx_platform_data(pd, win, metalView)) {
#endif
    SDL_Log("Could not obtain native rendering window: %s", SDL_GetError());
    SDL_DestroyWindow(win);
    s_window = nullptr;
    TextInputBox::releaseCachedCursors();
    SDL_Quit();
    return EXIT_FAILURE;
  }
#if TARGET_OS_IPHONE
  if (!InstallIOSGameplayTouchInput(metalView.get())) {
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                "Could not install direct UIKit gameplay touch input; retaining SDL raw input");
  }
  s_iosMetalLayer = pd.nwh;
  int metalDrawableW = 0;
  int metalDrawableH = 0;
  getIOSMetalDrawableSize(win, windowLogicalWidth, windowLogicalHeight,
                          metalDrawableW, metalDrawableH);
  if (metalDrawableW > 0 && metalDrawableH > 0 &&
      (metalDrawableW != rendering::render_width ||
       metalDrawableH != rendering::render_height)) {
    rendering::widthScale = static_cast<float>(metalDrawableW) /
                            static_cast<float>(windowLogicalWidth);
    rendering::heightScale = static_cast<float>(metalDrawableH) /
                             static_cast<float>(windowLogicalHeight);
    rendering::updateUIScale(metalDrawableW, metalDrawableH);
    APP_DEBUG_LOG("Metal drawable size: %d x %d", metalDrawableW,
                  metalDrawableH);
  }
#endif

  bgfx::Init bgfx_init;
  bgfx_init.type = bgfx::RendererType::Count;
  bgfx_init.resolution.width = rendering::render_width;
  bgfx_init.resolution.height = rendering::render_height;
  bgfx_init.resolution.reset = s_bgfxResetFlags;
#if !TARGET_OS_IPHONE
  // Bound GPU work queued ahead of visible input feedback on Android as well.
  bgfx_init.resolution.maxFrameLatency = 2;
#endif
  bgfx_init.platformData = pd;
  rendering::applyBgfxTransientBufferLimits(bgfx_init.limits);
#if TARGET_OS_IPHONE
  // Metal owns a CAMetalLayer supplied by UIKit. Register this thread as the
  // render thread before init so bgfx does not mutate that layer from its
  // internal worker. In single-threaded mode bgfx::frame() drives rendering.
  bgfx::renderFrame();
  SDL_Log("Using bgfx single-threaded mode on iOS");
#else
  SDL_Log("Using bgfx internal multithreaded mode");
#endif

  int appExitCode = runApplication(bgfx_init);

#if TARGET_OS_ANDROID
  s_androidPreviousWindow.reset();
  s_androidNativeWindow.reset();
#endif
#if TARGET_OS_IPHONE
  UninstallIOSGameplayTouchInput();
#endif
  metalView.reset();
  SDL_DestroyWindow(win);
  s_window = nullptr;
#if TARGET_OS_IPHONE
  s_iosMetalLayer = nullptr;
#endif
  TextInputBox::releaseCachedCursors();
  SDL_Quit();
  APP_DEBUG_LOG("SDL quit");

  return appExitCode;
}

static void reportStartupFailure(const ApplicationContext &context,
                                 const application_startup::Result &result) {
  switch (result.failure) {
  case application_startup::Failure::ProfileInitialization:
    SDL_Log("Application profile initialization failed: %s",
            context.profileInitializationResult.message.empty()
                ? "no diagnostic available"
                : context.profileInitializationResult.message.c_str());
    break;
  case application_startup::Failure::DatabaseInitialization:
    if (result.databaseStatus) {
      const auto &status = *result.databaseStatus;
      SDL_Log("Application database initialization failed: chart=%d score=%d "
              "replay=%d music=%d",
              status.chart ? 1 : 0, status.score ? 1 : 0, status.replay ? 1 : 0,
              status.music ? 1 : 0);
    }
    break;
  case application_startup::Failure::None:
    SDL_Log("Application startup reported an unspecified fatal failure");
    break;
  }

  if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "AsoBMaShow Startup Error",
                               result.userMessage.c_str(), s_window)) {
    SDL_Log("Unable to show the startup error dialog: %s", SDL_GetError());
  }
}

static void reportResultRecoveryWarning(
    const replay::ChartReplayRecoverySummary &) {
  if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING,
                               "AsoBMaShow Result Recovery",
                               replay::chartReplayRecoveryUserMessage().data(),
                               s_window)) {
    SDL_Log("Unable to show the result recovery warning: %s", SDL_GetError());
  }
}

static void
runReadyApplicationAfterResultRecovery(ApplicationContext &context) {
  player_settings::PresentationOrientationState presentationOrientation;
  presentationOrientation.updateViewport(rendering::render_width, rendering::render_height);
  bool orientationLocked = false;
  auto appliedOrientation = context.settings.screenOrientation;
  context.setGameplayOrientationLocked = [&](bool locked) {
    if (locked && !orientationLocked) {
      int logicalWidth = 0;
      int logicalHeight = 0;
      SDL_GetWindowSize(s_window, &logicalWidth, &logicalHeight);
      presentationOrientation.updateViewport(logicalWidth, logicalHeight);
    }
    orientationLocked = locked;
    presentationOrientation.setGameplayLocked(locked);
    if (context.sceneManager)
      context.sceneManager->setPresentationOrientation(presentationOrientation.orientation());
    appliedOrientation = context.settings.screenOrientation;
    screen_orientation::apply(appliedOrientation, locked);
  };
  screen_orientation::apply(appliedOrientation, false);
  context.bgfxResetFlags.store(s_bgfxResetFlags, std::memory_order_relaxed);
  if (context.chartLibraryTasks) {
    context.chartLibraryTasks->start();
    context.chartLibraryTasks->enqueue(
        {.kind = chart_library_tasks::TaskKind::RefreshLibrary,
         .title = i18n::message("menu.refresh_library.label")});
  }
  // Use depth-sorted main view for stable layering without sequential mode.
  bgfx::setViewMode(rendering::main_view, bgfx::ViewMode::DepthAscending);
  bgfx::setViewMode(rendering::ui_view, bgfx::ViewMode::Sequential);
  bgfx::setViewMode(rendering::readback_view, bgfx::ViewMode::Sequential);
  SceneManager sceneManager(context);
  sceneManager.setPresentationOrientation(presentationOrientation.orientation());
  sceneManager.registerScene("Intro", std::make_unique<IntroScene>(context));
  sceneManager.registerScene("MainMenu",
                             std::make_unique<MainMenuScene>(context));
  sceneManager.registerScene("Settings",
                             std::make_unique<SettingsScene>(context));
  sceneManager.changeScene("Intro");

  // SDL_RenderClear(ren);
  // SDL_RenderTexture(ren, tex, nullptr, nullptr);
  // SDL_RenderPresent(ren);
  SDL_Event e;

  auto lastFrameTime = std::chrono::steady_clock::now();

  // Initialize bgfx
  rendering::PosColorVertex::init();
  rendering::PosTexVertex::init();
  rendering::PosTexCoord0Vertex::init();
  s_postProcess.init(rendering::render_width, rendering::render_height);
  s_blurPass = s_postProcess.addBlurPass();
  s_blurPass->setInputViews(
      std::vector<bgfx::ViewId>(rendering::kGameplayBgaInputViews.begin(),
                                rendering::kGameplayBgaInputViews.end()));
  s_blurPass->setCompositeEnabled(false);
  s_blurPass->setBlurStrength(context.settings.bgaBlurStrength);
  // Example: s_blurPass->setCompositeEnabled(true);

  // We will use this to reference where we're drawing
  // This is set once to determine the clear color to use on starting a new
  // frame
  bgfx::setViewClear(rendering::clear_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                     0x00000000);
  bgfx::setViewClear(rendering::ui_view, BGFX_CLEAR_DEPTH, 0x00000000);
  bgfx::setViewClear(rendering::bga_view, BGFX_CLEAR_COLOR, 0x00000000);
  bgfx::setViewClear(rendering::bga_layer_view, BGFX_CLEAR_NONE, 0x00000000);

  bgfx::setViewClear(rendering::main_view, BGFX_CLEAR_DEPTH, 0x00000000, 1.0f,
                     0);
  bgfx::setViewClear(s_blurPass->blurViewH(), BGFX_CLEAR_COLOR, 0x00000000,
                     1.0f, 0);
  bgfx::setViewClear(s_blurPass->blurViewV(), BGFX_CLEAR_COLOR, 0x00000000,
                     1.0f, 0);
  bgfx::setViewClear(s_blurPass->finalView(), BGFX_CLEAR_COLOR, 0x00000000,
                     1.0f, 0);

  context.restoreGameplayRenderViews = [&context]() {
    if (s_blurPass == nullptr) {
      return;
    }
    for (const auto view : rendering::kGameplayOutputViews) {
      bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
    }
    bgfx::setViewFrameBuffer(rendering::readback_view, BGFX_INVALID_HANDLE);
    s_blurPass->setInputViews(
        std::vector<bgfx::ViewId>(rendering::kGameplayBgaInputViews.begin(),
                                  rendering::kGameplayBgaInputViews.end()));
    resetViewTransform(s_blurPass->sceneWidth(), s_blurPass->sceneHeight(),
                       s_blurPass->blurViewH(), s_blurPass->blurViewV(),
                       s_blurPass->finalView(), context.settings);
    rendering::applyViewOrder(s_blurPass->blurViewH(), s_blurPass->blurViewV(),
                              s_blurPass->finalView());
  };
  context.restoreGameplayRenderViews();

  uint64_t rawEventsInWindow = 0;
  uint64_t processedEventsInWindow = 0;
  uint64_t coalescedMouseMotionInWindow = 0;
  uint64_t coalescedFingerMotionInWindow = 0;
  uint64_t coalescedResizeInWindow = 0;
  // Pinned gdx-backend-lwjgl's LwjglGraphics.updateTime() starts with a
  // zero fps value, then publishes the prior whole-second frame count before
  // counting the current frame in the next window.
  std::optional<std::chrono::steady_clock::time_point> fpsWindowStart;
  int renderedFramesInWindow = 0;
  float appliedLaneAngleDegrees = context.settings.presentation().laneAngleDegrees;
  float appliedLaneLength = context.settings.presentation().laneLength;
  bool hasDeferredRenderResize = false;
  int deferredRenderResizeW = 0;
  int deferredRenderResizeH = 0;
  uint32_t activeBgfxResetFlags = s_bgfxResetFlags;
  context.displayBackend = std::make_unique<display::SDLDisplayBackend>(
      s_window, TARGET_PLATFORM == iOS || TARGET_PLATFORM == Android,
      [&activeBgfxResetFlags]() { return activeBgfxResetFlags; },
      [&context, &activeBgfxResetFlags, &presentationOrientation](
          std::uint32_t /*resetFlags*/, std::string &errorMessage)
          -> std::unique_ptr<display::IRendererDisplayTransaction> {
        auto reservation =
            context.rendererAccess.tryAcquireDisplay(errorMessage);
        if (!reservation.has_value()) {
          return nullptr;
        }
        auto lifetime = std::make_shared<
            display::RendererAccessCoordinator::DisplayReservation>(
            std::move(*reservation));
        return std::make_unique<CallbackRendererDisplayTransaction>(
            std::move(lifetime),
            [&context, &activeBgfxResetFlags, &presentationOrientation](
                std::uint32_t resetFlags, std::string &syncError) {
              int logicalWidth = 0;
              int logicalHeight = 0;
              int renderWidth = 0;
              int renderHeight = 0;
              SDL_GetWindowSize(s_window, &logicalWidth, &logicalHeight);
              getWindowDrawableSize(s_window, logicalWidth, logicalHeight,
                                    renderWidth, renderHeight);
              if (logicalWidth <= 0 || logicalHeight <= 0 || renderWidth <= 0 ||
                  renderHeight <= 0) {
                syncError = "The display produced an invalid drawable size.";
                return false;
              }
              rendering::widthScale = static_cast<float>(renderWidth) /
                                      static_cast<float>(logicalWidth);
              rendering::heightScale = static_cast<float>(renderHeight) /
                                       static_cast<float>(logicalHeight);
              rendering::updateUIScale(renderWidth, renderHeight);
              // Startup VSync may observe the OS rotation before its resize
              // event. Publish the matching profile while skins can prepare
              // in the menu, before gameplay locks the current orientation.
              presentationOrientation.updateViewport(renderWidth, renderHeight);
              context.sceneManager->setPresentationOrientation(
                  presentationOrientation.orientation());
              activeBgfxResetFlags = resetFlags;
              s_bgfxResetFlags = resetFlags;
              context.bgfxResetFlags.store(resetFlags,
                                           std::memory_order_relaxed);
              bgfx::reset(rendering::render_width, rendering::render_height,
                          resetFlags);
              s_postProcess.resize(rendering::render_width,
                                   rendering::render_height);
              context.restoreGameplayRenderViews();
              context.framePacer.reset(std::chrono::steady_clock::now());
              return true;
            });
      }, TARGET_PLATFORM == Android);
#if TARGET_OS_ANDROID
  // Android owns native geometry; only apply the persisted renderer preference.
  auto startupVideo = context.displayBackend->capture().settings;
  startupVideo.vsync = context.settings.audioVideo.video.vsync;
  std::string startupVsyncError;
  if (!context.displayBackend->apply(startupVideo, startupVsyncError)) {
    SDL_Log("Could not apply Android VSync preference: %s",
            startupVsyncError.c_str());
  }
#endif
  context.displaySettingsManager =
      std::make_unique<display::DisplaySettingsManager>(
          *context.displayBackend, context.framePacer,
          context.settings.audioVideo.video);
  const auto startupDisplayResult =
      context.displaySettingsManager->applySafeStartupIntent();
  if (!startupDisplayResult.message.empty()) {
    SDL_Log("%s", startupDisplayResult.message.resolve().c_str());
  }
  context.framePacer.reset(lastFrameTime);
  bool pacingExportActive =
      context.replayVideoExportActive.load(std::memory_order_acquire);
#if TARGET_OS_IPHONE
  FramePacer iosPresentationPacer;
#endif
  constexpr int kBackgroundEventWaitTimeoutMs = 1000;
  auto isAppBackgroundEvent = [](const SDL_Event &event) {
    return input::isBackgroundLifecycleEvent(event);
  };
  auto isAppForegroundEvent = [](const SDL_Event &event) {
    return input::isForegroundLifecycleEvent(event);
  };
  auto setAppBackground = [&](bool background) {
    if (background && context.displaySettingsManager) {
      if (const auto rollbackResult =
              context.displaySettingsManager->onFocusLost();
          rollbackResult.has_value() && !rollbackResult->message.empty()) {
        SDL_Log("%s", rollbackResult->message.resolve().c_str());
      }
    }
    const bool previous =
        context.appInBackground.exchange(background, std::memory_order_acq_rel);
    if (previous == background) {
      return;
    }
#if TARGET_OS_ANDROID
    // The miniaudio device is independent of SDL's paused audio devices.
    const bool gameplayContinues = sceneManager.currentScene != nullptr &&
        sceneManager.currentScene->continuesAudioInBackground();
    context.jukebox.audioRuntime().setApplicationSuspended(
        background && !gameplayContinues);
#endif
    scene_event_routing::dispatchApplicationBackgroundChange(
        sceneManager.currentScene, background);
    context.setIrApplicationActive(!background);
    context.jukebox.setVisualsSuspended(background);
    if (!background) {
      lastFrameTime = std::chrono::steady_clock::now();
      context.framePacer.reset(lastFrameTime);
#if TARGET_OS_IPHONE
      iosPresentationPacer.reset(lastFrameTime);
#endif
      context.jukebox.seekVisualsToSongTime(context.jukebox.getTimeMicros());
    }
  };
#if TARGET_OS_ANDROID
  bool androidSystemSuspended = false;
  bool androidRenderSuspended = false;
  bool androidResumeResizePending = false;
#endif
  while (!context.quitFlag) {
    if (!orientationLocked &&
        appliedOrientation != context.settings.screenOrientation) {
      appliedOrientation = context.settings.screenOrientation;
      screen_orientation::apply(appliedOrientation, false);
    }
#if TARGET_OS_ANDROID
    if (!context.appInBackground.load(std::memory_order_acquire))
#endif
    context.pollGameplaySkinCommits();
    if (context.chartLibraryFolderActions) {
      context.chartLibraryFolderActions->poll();
    }

    auto currentFrameTime = std::chrono::steady_clock::now();
    context.applicationUptimeMillis.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            currentFrameTime - context.bootTime)
            .count(),
        std::memory_order_release);
    const bool exportActiveForPacing =
        context.replayVideoExportActive.load(std::memory_order_acquire);
    if (exportActiveForPacing != pacingExportActive) {
      pacingExportActive = exportActiveForPacing;
      context.framePacer.reset(currentFrameTime);
#if TARGET_OS_IPHONE
      iosPresentationPacer.reset(currentFrameTime);
#endif
    }
    if (context.displaySettingsManager) {
      if (const auto previewResult =
              context.displaySettingsManager->tick(currentFrameTime)) {
        if (!previewResult->message.empty()) {
          SDL_Log("%s", previewResult->message.resolve().c_str());
        }
      }
    }
    float deltaTime =
        std::chrono::duration<float, std::chrono::seconds::period>(
            currentFrameTime - lastFrameTime)
            .count();
    lastFrameTime = currentFrameTime;

    SDL_Event pendingMouseMotion{};
    std::vector<SDL_Event> pendingFingerMotions;
    SDL_Event pendingResizeEvent{};
    uint32_t pendingMouseMotionCount = 0;
    uint32_t pendingFingerMotionCount = 0;
    uint32_t pendingResizeCount = 0;
    bool hasPendingMouseMotion = false;
    bool hasPendingResize = false;

    auto deferWindowResize = [&](int logicalW, int logicalH) {
      deferredRenderResizeW = logicalW;
      deferredRenderResizeH = logicalH;
      hasDeferredRenderResize = true;
    };

    auto applyWindowResize = [&](int logicalW, int logicalH) {
#if TARGET_OS_ANDROID
      if (context.appInBackground.load(std::memory_order_acquire)) return false;
#endif
      if (logicalW <= 0 || logicalH <= 0) {
        return true;
      }
#if TARGET_OS_IPHONE
      int targetRenderW = 0;
      int targetRenderH = 0;
      getIOSMetalDrawableSize(s_window, logicalW, logicalH, targetRenderW,
                              targetRenderH);
#else
      int targetRenderW = 0;
      int targetRenderH = 0;
      getWindowDrawableSize(s_window, logicalW, logicalH, targetRenderW,
                            targetRenderH);
#endif
      if (targetRenderW <= 0 || targetRenderH <= 0) {
        return true;
      }
      if (targetRenderW == rendering::render_width &&
          targetRenderH == rendering::render_height) {
        presentationOrientation.updateViewport(targetRenderW, targetRenderH);
        sceneManager.setPresentationOrientation(
            presentationOrientation.orientation());
        return true;
      }

      if (context.replayVideoExportActive.load(std::memory_order_acquire)) {
        return false;
      }
      std::unique_lock<std::mutex> bgfxLock(context.bgfxRenderMutex,
                                            std::try_to_lock);
      if (!bgfxLock.owns_lock()) {
        return false;
      }

      rendering::widthScale =
          static_cast<float>(targetRenderW) / static_cast<float>(logicalW);
      rendering::heightScale =
          static_cast<float>(targetRenderH) / static_cast<float>(logicalH);
      rendering::updateUIScale(targetRenderW, targetRenderH);
      presentationOrientation.updateViewport(targetRenderW, targetRenderH);
      sceneManager.setPresentationOrientation(presentationOrientation.orientation());

      // set bgfx resolution
      bgfx::reset(rendering::render_width, rendering::render_height,
                  activeBgfxResetFlags);
      context.bgfxResetFlags.store(activeBgfxResetFlags,
                                   std::memory_order_relaxed);
      APP_DEBUG_LOG("Render size: %d x %d (logical: %d x %d, scale %.2f)",
                    rendering::render_width, rendering::render_height, logicalW,
                    logicalH, s_renderScale);
      s_postProcess.resize(rendering::render_width, rendering::render_height);
      context.restoreGameplayRenderViews();
      context.framePacer.reset(std::chrono::steady_clock::now());
      return true;
    };

#if TARGET_OS_ANDROID
    auto refreshAndroidBgfxPlatformData = [&]() {
      if (s_window == nullptr) {
        return false;
      }
      bgfx::PlatformData pd{};
      auto window = acquireAndroidNativeWindow(s_window);
      pd.nwh = window.get();
      if (pd.nwh == nullptr) {
        SDL_Log("Android window handle is not ready yet");
        return false;
      }
      bgfx::setPlatformData(pd);
      s_androidPreviousWindow = std::move(s_androidNativeWindow);
      s_androidNativeWindow = std::move(window);
      return true;
    };

    auto retireAndroidPreviousWindow = [&]() {
      if (s_androidPreviousWindow) {
        // The first resumed submission installs the new native window. Wait
        // until it has been consumed before releasing our old window owner.
        bgfx::frame();
        s_androidPreviousWindow.reset();
      }
    };

    auto applyAndroidRenderSuspend = [&](bool suspend) {
      if (androidRenderSuspended == suspend) {
        if (suspend) {
          NotifyAndroidExternalActivityRenderPaused();
          NotifyAndroidSurfaceRenderPaused();
        }
        return true;
      }

      if (context.replayVideoExportActive.load(std::memory_order_acquire)) {
        return false;
      }

      std::unique_lock<std::mutex> bgfxLock(context.bgfxRenderMutex);
      if (!suspend && !refreshAndroidBgfxPlatformData()) {
        return false;
      }
      activeBgfxResetFlags =
          suspend ? (s_bgfxResetFlags | BGFX_RESET_SUSPEND) : s_bgfxResetFlags;
      context.bgfxResetFlags.store(activeBgfxResetFlags,
                                   std::memory_order_relaxed);
      bgfx::reset(rendering::render_width, rendering::render_height,
                  activeBgfxResetFlags);
      // Reset clears view framebuffer bindings even when the drawable size
      // stays unchanged, so the next frame must restore the BGA input targets.
      context.restoreGameplayRenderViews();
      bgfx::frame();
      if (suspend) {
        // frame() returns before the previous frame's final present. Passing
        // a second suspended frame retires that present before Java releases
        // the native window in SDLSurface.surfaceDestroyed().
        bgfx::frame();
      } else {
        retireAndroidPreviousWindow();
      }
      androidRenderSuspended = suspend;
      SDL_Log("Android rendering %s", suspend ? "suspended" : "resumed");
      if (suspend) {
        NotifyAndroidExternalActivityRenderPaused();
        NotifyAndroidSurfaceRenderPaused();
      } else {
        androidResumeResizePending = true;
      }
      return true;
    };

    auto syncAndroidRenderSuspend = [&]() {
      const bool shouldSuspend =
          androidSystemSuspended ||
          IsAndroidSurfaceRenderPauseRequested() ||
          IsAndroidExternalActivityRenderPauseRequested();
      if (!applyAndroidRenderSuspend(shouldSuspend)) {
        return androidRenderSuspended || shouldSuspend;
      }
      return androidRenderSuspended;
    };
#endif

#if TARGET_OS_IPHONE
    auto restoreIOSViewportAfterKeyboardFocus = [&]() {
      RestoreIOSViewportAfterKeyboardFocus();

      int logicalW = 0;
      int logicalH = 0;
      if (s_window != nullptr) {
        SDL_GetWindowSize(s_window, &logicalW, &logicalH);
      }
      if (logicalW > 0 && logicalH > 0 &&
          !applyWindowResize(logicalW, logicalH)) {
        deferWindowResize(logicalW, logicalH);
      }
    };
#endif

    auto processEvent = [&](SDL_Event event) {
      if constexpr (ASOBMASHOW_ENABLE_PERF_TELEMETRY) {
        ++processedEventsInWindow;
      }
      if (event.type == SDL_EVENT_QUIT) {
        context.quitFlag = true;
      }

      if (event.type == SDL_EVENT_LOW_MEMORY) {
        ImageView::evictDecodedImageCache();
        context.jukebox.handleMemoryPressure();
        SDL_Log("Released evictable resources after a low-memory warning");
      }

      if (isAppBackgroundEvent(event)) {
        setAppBackground(true);
      }

      if (isAppForegroundEvent(event)) {
        setAppBackground(false);
      }

#if TARGET_OS_ANDROID
      if (isAppBackgroundEvent(event)) {
        androidSystemSuspended = true;
        syncAndroidRenderSuspend();
      }

      if (isAppForegroundEvent(event)) {
        androidSystemSuspended = false;
        androidResumeResizePending = true;
        syncAndroidRenderSuspend();
      }
#endif

#if TARGET_OS_IPHONE
      if (isAppForegroundEvent(event)) {
        restoreIOSViewportAfterKeyboardFocus();
      }
#endif

      // on window resize
      if ((event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) &&
          (event.type == SDL_EVENT_WINDOW_RESIZED ||
           event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)) {
        int logicalW = 0, logicalH = 0;
        SDL_GetWindowSize(s_window, &logicalW, &logicalH);
        if (!applyWindowResize(logicalW, logicalH)) {
          deferWindowResize(logicalW, logicalH);
        }
      }

      if (scene_event_routing::shouldDispatchToScene(event) &&
          !(TARGET_OS_ANDROID &&
            context.appInBackground.load(std::memory_order_acquire) &&
            (event.type < SDL_EVENT_WINDOW_FIRST || event.type > SDL_EVENT_WINDOW_LAST))) {
        auto result = sceneManager.handleEvents(event);
        if (result.quit) {
          context.quitFlag = true;
        }
      }


    };

    auto flushPendingResize = [&]() {
      if (!hasPendingResize) return;
      processEvent(pendingResizeEvent);
      if (pendingResizeCount > 1) {
        if constexpr (ASOBMASHOW_ENABLE_PERF_TELEMETRY) {
          coalescedResizeInWindow += (pendingResizeCount - 1);
        }
      }
      hasPendingResize = false;
      pendingResizeCount = 0;
    };

    auto waitForBackgroundEvent = [&]() {
      int timeoutMs = kBackgroundEventWaitTimeoutMs;
#if TARGET_OS_ANDROID
      if (sceneManager.currentScene != nullptr &&
          sceneManager.currentScene->continuesAudioInBackground()) {
        sceneManager.currentScene->updateWhileBackgrounded();
        timeoutMs = 16;
      }
#endif
      SDL_Event waitEvent{};
      if (SDL_WaitEventTimeout(&waitEvent, timeoutMs)) {
        if constexpr (ASOBMASHOW_ENABLE_PERF_TELEMETRY) {
          ++rawEventsInWindow;
        }
        context.inputDeviceRegistry.handleSdlEventAndDispatch(waitEvent);
        processEvent(waitEvent);
      }
    };

    while (SDL_PollEvent(&e)) {
      if constexpr (ASOBMASHOW_ENABLE_PERF_TELEMETRY) {
        ++rawEventsInWindow;
      }
      // A later Start/touch callback may enter gameplay and lock orientation.
      // Apply earlier viewport changes before either input dispatch path.
      const bool resizeEvent = (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) &&
          (e.type == SDL_EVENT_WINDOW_RESIZED ||
           e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);
      if (!resizeEvent) flushPendingResize();
      context.inputDeviceRegistry.handleSdlEventAndDispatch(e);

      if (e.type == SDL_EVENT_MOUSE_MOTION) {
        pendingMouseMotion = e;
        hasPendingMouseMotion = true;
        ++pendingMouseMotionCount;
        continue;
      }
      if (e.type == SDL_EVENT_FINGER_MOTION) {
        auto existing = std::find_if(
            pendingFingerMotions.begin(), pendingFingerMotions.end(),
            [&](const SDL_Event &pending) {
              return pending.tfinger.touchID == e.tfinger.touchID &&
                     pending.tfinger.fingerID == e.tfinger.fingerID;
            });
        if (existing != pendingFingerMotions.end()) {
          *existing = e;
        } else {
          pendingFingerMotions.push_back(e);
        }
        ++pendingFingerMotionCount;
        continue;
      }
      if ((e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) &&
          (e.type == SDL_EVENT_WINDOW_RESIZED ||
           e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)) {
        pendingResizeEvent = e;
        hasPendingResize = true;
        ++pendingResizeCount;
        continue;
      }

      processEvent(e);
    }

    flushPendingResize();
    if (!pendingFingerMotions.empty()) {
      for (const auto &pendingFingerMotion : pendingFingerMotions) {
        processEvent(pendingFingerMotion);
      }
      if (pendingFingerMotionCount > pendingFingerMotions.size()) {
        if constexpr (ASOBMASHOW_ENABLE_PERF_TELEMETRY) {
          coalescedFingerMotionInWindow +=
              pendingFingerMotionCount - pendingFingerMotions.size();
        }
      }
    }
    if (hasPendingMouseMotion) {
      processEvent(pendingMouseMotion);
      if (pendingMouseMotionCount > 1) {
        if constexpr (ASOBMASHOW_ENABLE_PERF_TELEMETRY) {
          coalescedMouseMotionInWindow += (pendingMouseMotionCount - 1);
        }
      }
    }
#if TARGET_OS_IPHONE || TARGET_OS_ANDROID
    // Gameplay-owned gestures bypass SDL on the native input thread. Their
    // presentation events still traverse the ordinary registry and scene UI
    // here, without SDL event watches or a second gameplay delivery.
    input::native_touch::UiTouchEvent uiTouch;
    for (std::size_t count = 0;
         count < input::native_touch::kUiTouchQueueCapacity &&
         input::native_touch::RawTouchRegistration::pollUiEvent(uiTouch); ++count) {
      if (!input::native_touch::RawTouchRegistration::isCurrentEpoch(uiTouch.epoch)) {
        continue;
      }
      using input::native_touch::TouchPhase;
      SDL_Event touchEvent{};
      touchEvent.type = uiTouch.touch.phase == TouchPhase::Down ? SDL_EVENT_FINGER_DOWN
          : uiTouch.touch.phase == TouchPhase::Up ? SDL_EVENT_FINGER_UP
          : uiTouch.touch.phase == TouchPhase::Cancel ? SDL_EVENT_FINGER_CANCELED
                                                    : SDL_EVENT_FINGER_MOTION;
      const auto nowMicros = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      const auto ageNanos = static_cast<Uint64>(std::max<std::int64_t>(
          0, nowMicros - uiTouch.touch.steadyTimestampMicros)) * 1000;
      const auto nowNanos = SDL_GetTicksNS();
      touchEvent.tfinger.timestamp = ageNanos < nowNanos ? nowNanos - ageNanos : 1;
      touchEvent.tfinger.touchID = 1;
      touchEvent.tfinger.fingerID = static_cast<SDL_FingerID>(uiTouch.touch.pointerId) + 1;
      touchEvent.tfinger.x = uiTouch.touch.x;
      touchEvent.tfinger.y = uiTouch.touch.y;
      touchEvent.tfinger.pressure = uiTouch.touch.phase == TouchPhase::Up ||
          uiTouch.touch.phase == TouchPhase::Cancel ? 0.0f : 1.0f;
      touchEvent.tfinger.windowID = SDL_GetWindowID(s_window);
      context.inputDeviceRegistry.handleSdlEventAndDispatch(touchEvent);
      if (input::native_touch::RawTouchRegistration::isCurrentEpoch(uiTouch.epoch)) {
        processEvent(touchEvent);
      }
    }
#endif
    if (hasDeferredRenderResize &&
        applyWindowResize(deferredRenderResizeW, deferredRenderResizeH)) {
      hasDeferredRenderResize = false;
    }
#if TARGET_OS_ANDROID
    if (syncAndroidRenderSuspend()) {
      if (context.appInBackground.load(std::memory_order_acquire)) {
        waitForBackgroundEvent();
      } else {
        SDL_Delay(16);
      }
      context.inputDeviceRegistry.pump();
      context.currentFrame++;
      continue;
    }
    if (androidResumeResizePending) {
      int logicalW = 0;
      int logicalH = 0;
      if (s_window != nullptr) {
        SDL_GetWindowSize(s_window, &logicalW, &logicalH);
      }
      if (logicalW > 0 && logicalH > 0) {
        if (!applyWindowResize(logicalW, logicalH)) {
          deferWindowResize(logicalW, logicalH);
        }
      }
      androidResumeResizePending = false;
    }
#endif
    if (context.appInBackground.load(std::memory_order_acquire) &&
        !context.replayVideoExportActive.load(std::memory_order_acquire)) {
      waitForBackgroundEvent();
      context.inputDeviceRegistry.pump();
      context.currentFrame++;
      continue;
    }
    context.inputDeviceRegistry.pump();
    sceneManager.update(deltaTime);
    s_blurPass->setBlurStrength(context.settings.bgaBlurStrength);
    context.jukebox.setBgaDisplayMode(context.settings.bgaDisplayMode);
    const bool laneTransformChanged =
        std::abs(appliedLaneAngleDegrees - context.settings.presentation().laneAngleDegrees) >
            0.001f ||
        std::abs(appliedLaneLength - context.settings.presentation().laneLength) > 0.001f;
    if (laneTransformChanged &&
        !context.replayVideoExportActive.load(std::memory_order_acquire) &&
        !context.rendererAccess.exportRequested()) {
      std::unique_lock<std::mutex> bgfxLock(context.bgfxRenderMutex,
                                            std::try_to_lock);
      if (bgfxLock.owns_lock()) {
        appliedLaneAngleDegrees = context.settings.presentation().laneAngleDegrees;
        appliedLaneLength = context.settings.presentation().laneLength;
        context.restoreGameplayRenderViews();
      }
    }

    //    bgfx::reset(rendering::window_width, rendering::window_height);
    // SDL_Log("Window size: %d x %d", rendering::window_width,
    //         rendering::window_height);
    // clear color

    bool renderedFrame = false;
    const bool replayExportActive =
        context.replayVideoExportActive.load(std::memory_order_acquire);
    const bool replayExportRequested = context.rendererAccess.exportRequested();
    const bool replayExportUiFrameRequested =
        context.replayVideoExportUiFrameRequested.load(
            std::memory_order_acquire);
    if ((!replayExportActive && !replayExportRequested) ||
        replayExportUiFrameRequested) {
      std::unique_lock<std::mutex> bgfxLock(context.bgfxRenderMutex,
                                            std::try_to_lock);
      if (bgfxLock.owns_lock() &&
          context.replayVideoExportActive.load(std::memory_order_acquire) &&
          context.replayVideoExportUiFrameRequested.load(
              std::memory_order_acquire)) {
        bgfx::touch(rendering::clear_view);
        bgfx::touch(rendering::ui_view);
        context.uiBatchRenderer.beginFrame();
        sceneManager.render();
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
        const auto submitStarted = perf::latency::nowMicros();
#endif
        bgfx::frame();
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
        perf::latency::record(perf::latency::Stage::FrameSubmit,
                              perf::latency::nowMicros() - submitStarted);
#endif
        context.replayVideoExportUiFrameSerial.fetch_add(
            1, std::memory_order_release);
        context.replayVideoExportUiFrameRequested.store(
            false, std::memory_order_release);
        renderedFrame = true;
      } else if (bgfxLock.owns_lock() &&
                 !context.replayVideoExportActive.load(
                     std::memory_order_acquire) &&
                 !context.rendererAccess.exportRequested()) {
        const bool hasActiveVisuals = context.jukebox.hasActiveVisuals();

        bgfx::touch(rendering::clear_view);
        bgfx::touch(rendering::ui_view);
        context.uiBatchRenderer.beginFrame();
        sceneManager.render();

        const GameplayBgaCompositeState &bgaCompositeState =
            context.gameplayBgaCompositeState;
        const bool submitPreparedFullscreen =
            bgaCompositeState.mode ==
                GameplayBgaCompositeMode::FullscreenBuiltIn &&
            bgaCompositeState.prepared.has_value();
        const bool submitLegacyFullscreen =
            bgaCompositeState.mode ==
                GameplayBgaCompositeMode::FullscreenBuiltIn &&
            bgaCompositeState.frameSerial == 0 &&
            !bgaCompositeState.prepared.has_value() && hasActiveVisuals;
        const bool compositeFullscreenBga =
            submitPreparedFullscreen || submitLegacyFullscreen;
        if (compositeFullscreenBga) {
          bgfx::touch(rendering::bga_view);
          bgfx::touch(rendering::bga_layer_view);
          bgfx::touch(s_blurPass->finalView());
          bgfx::touch(s_blurPass->blurViewH());
          bgfx::touch(s_blurPass->blurViewV());
          const bool ignoreBgaPostOptions =
              context.ignoreBgaPostOptions.load(std::memory_order_acquire);
          if (submitPreparedFullscreen) {
            context.jukebox.submitFullscreen(*bgaCompositeState.prepared);
          } else {
            context.jukebox.render();
          }
          s_blurPass->setBlurStrength(
              ignoreBgaPostOptions ? 0.0f : context.settings.bgaBlurStrength);
          s_postProcess.apply();
          rendering::renderFullscreenTextureTint(
              s_blurPass->outputTexture(), s_blurPass->finalView(),
              ignoreBgaPostOptions
                  ? 1.0f
                  : static_cast<float>(context.settings.bgaBrightnessPercent) /
                        100.0f);
        }
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
        const auto submitStarted = perf::latency::nowMicros();
#endif
        bgfx::frame();
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
        perf::latency::record(perf::latency::Stage::FrameSubmit,
                              perf::latency::nowMicros() - submitStarted);
#endif
        renderedFrame = true;
      }
    }

    if (!fpsWindowStart.has_value()) {
      fpsWindowStart = currentFrameTime;
    } else if (currentFrameTime - *fpsWindowStart >=
               std::chrono::seconds(1)) {
      context.currentFramesPerSecond.store(renderedFramesInWindow,
                                           std::memory_order_release);
      renderedFramesInWindow = 0;
      fpsWindowStart = currentFrameTime;
    }
    if (renderedFrame) {
      ++renderedFramesInWindow;
    }

#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
    {
      constexpr float kTelemetryLogIntervalSec = 5.0f;
      static FPSCounter fpsCounter;
      static float telemetryLogInterval = 0.0f;
      fpsCounter.addFrame(deltaTime);
      telemetryLogInterval += deltaTime;
      if (telemetryLogInterval >= kTelemetryLogIntervalSec) {
        telemetryLogInterval = 0.0f;
        const float currentFps = deltaTime > 0 ? 1.0f / deltaTime : 0.0f;
        const float avgFps = fpsCounter.getAverageFPS();
        const float low1Fps = fpsCounter.get1PercentLowFPS();

        const double avgDeltaTime = context.jukebox.getAvgDeltaTime();
        const double freq = avgDeltaTime > 0.0 ? 1000000.0 / avgDeltaTime : 0.0;
        SDL_Log(
            "FPS %.1f | Avg %.1f | 1%% Low %.1f | Scheduler %.2f us (%.2f Hz) | "
            "Events raw %llu proc %llu coalesced M/F/R %llu/%llu/%llu",
            currentFps, avgFps, low1Fps, avgDeltaTime, freq,
            static_cast<unsigned long long>(rawEventsInWindow),
            static_cast<unsigned long long>(processedEventsInWindow),
            static_cast<unsigned long long>(coalescedMouseMotionInWindow),
            static_cast<unsigned long long>(coalescedFingerMotionInWindow),
            static_cast<unsigned long long>(coalescedResizeInWindow));
        const auto audioState = context.jukebox.audioRuntime().runtimeState();
        SDL_Log("Audio native callback %u frames at %u Hz | output timestamp %s | "
                "underflow reporting %s (%llu) | physical display presentation unknown",
                audioState.effectiveBufferFrames, audioState.effectiveCallbackSampleRate,
                audioState.outputTimestampKnown ? "available" : "unknown",
                audioState.outputUnderflowKnown ? "available" : "unknown",
                static_cast<unsigned long long>(audioState.outputUnderflowCount));
        for (unsigned stage = 0; stage < static_cast<unsigned>(perf::latency::Stage::Count); ++stage) {
          const auto summary = perf::latency::snapshot(static_cast<perf::latency::Stage>(stage));
          if (summary.count == 0) continue;
          SDL_Log("Latency stage %s | n %llu | p50/p95/p99 <= %llu/%llu/%llu us | max %llu us",
                  perf::latency::names[stage], static_cast<unsigned long long>(summary.count),
                  static_cast<unsigned long long>(summary.p50), static_cast<unsigned long long>(summary.p95),
                  static_cast<unsigned long long>(summary.p99), static_cast<unsigned long long>(summary.maximum));
        }
        rawEventsInWindow = 0;
        processedEventsInWindow = 0;
        coalescedMouseMotionInWindow = 0;
        coalescedFingerMotionInWindow = 0;
        coalescedResizeInWindow = 0;
      }
    }
#endif

    // shift left by 1
    // float translate[16];
    // bx::mtxTranslate(translate, 200.0f, 500.0f, 0.0f);
    // float rotate[16];
    // bx::mtxRotateZ(rotate, bx::toRad(45.0f));
    // float mtx[16];
    // bx::mtxMul(mtx, rotate, translate);
    // bgfx::setTransform(mtx);
    //
    // bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    //
    // bgfx::setVertexBuffer(0, triangleVbh);
    // bgfx::setIndexBuffer(triangleIbh);
    // bgfx::submit(rendering::ui_view, program);
    //
    // bx::mtxTranslate(translate, 300.0f, 500.0f, 0.0f);
    // bx::mtxRotateZ(rotate, bx::toRad(45.0f));
    // bx::mtxMul(mtx, rotate, translate);
    // bgfx::setTransform(mtx);
    // bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    // bgfx::setVertexBuffer(0, rectVbh);
    // bgfx::setIndexBuffer(rectIbh);
    // bgfx::submit(rendering::ui_view, program);

    // draw cube
    //    bgfx::touch(rendering::main_view);
    //
    //    bgfx::setVertexBuffer(0, vbh);
    //    bgfx::setIndexBuffer(ibh);
    //    bgfx::setState(BGFX_STATE_DEFAULT);
    //    bgfx::submit(rendering::main_view, program);

    if (renderedFrame) {
      const auto presentedAt = std::chrono::steady_clock::now();
      context.framePacer.framePresented(presentedAt);
#if TARGET_OS_IPHONE
      // iOS Metal renders on UIKit's main thread. Pace default VSync here so
      // its idle interval services touches instead of blocking in nextDrawable.
      // A CADisplayLink wait regressed measured UIKit delivery latency.
      const SDL_DisplayMode *iosMode =
          SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(s_window));
      iosPresentationPacer.setCap(video::iosPresentationPacingCap(
          iosMode != nullptr ? iosMode->refresh_rate : 0.0F,
          context.framePacer.currentFrameCap(),
          context.replayVideoExportActive.load(std::memory_order_acquire) ||
              context.rendererAccess.exportRequested()));
      iosPresentationPacer.framePresented(presentedAt);
#endif
      const auto waitStartedAt = std::chrono::steady_clock::now();
      auto waitDuration = context.framePacer.remaining(waitStartedAt);
#if TARGET_OS_IPHONE
      waitDuration = std::max(
          waitDuration, iosPresentationPacer.remaining(waitStartedAt));
#endif
      if (waitDuration > std::chrono::steady_clock::duration::zero()) {
#if TARGET_OS_IPHONE
        const auto waitMicros = std::max<long long>(
            1,
            std::chrono::duration_cast<std::chrono::microseconds>(waitDuration)
                .count());
        WaitIOSMainRunLoopForMicros(waitMicros);
#elif TARGET_OS_ANDROID
        std::this_thread::sleep_for(waitDuration);
#else
        // Keep the existing frame deadline while servicing input between
        // frames. Native realtime sources can also arrive during these waits.
        const auto waitDeadline = waitStartedAt + waitDuration;
        while (!context.quitFlag &&
               !context.appInBackground.load(std::memory_order_acquire)) {
          while (SDL_PollEvent(&e)) {
            if constexpr (ASOBMASHOW_ENABLE_PERF_TELEMETRY) {
              ++rawEventsInWindow;
            }
            context.inputDeviceRegistry.handleSdlEventAndDispatch(e);
            processEvent(e);
          }
          context.inputDeviceRegistry.pump();
          const auto remaining = waitDeadline - std::chrono::steady_clock::now();
          if (remaining <= std::chrono::steady_clock::duration::zero()) {
            break;
          }
          std::this_thread::sleep_for(std::min(
              remaining, std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::milliseconds(1))));
        }
#endif
      }
    } else {
      SDL_Delay(1);
    }
    sceneManager.handleDeferred();
    context.currentFrame++;
    //
  }
  sceneManager.cleanup();
  context.setGameplayOrientationLocked = {};
  if (context.displaySettingsManager) {
    const auto shutdownResult = context.displaySettingsManager->shutdown();
    if (!shutdownResult.message.empty()) {
      SDL_Log("%s", shutdownResult.message.resolve().c_str());
    }
  }
  context.displaySettingsManager.reset();
  context.displayBackend.reset();
  s_postProcess.shutdown();
  // bgfx::destroy(vbh);
  // bgfx::destroy(ibh);
}

static void runReadyApplication(ApplicationContext &context) {
  // Make defaults visible to the first selector load. Leave the online seed
  // pending so the normal background refresh replaces these snapshots.
  if (context.applicationUiStateLoadResult.status !=
      ApplicationUiStateLoadStatus::FutureVersion) {
    if (auto session = context.chartRepository.OpenSession()) {
      DifficultyTableImporter importer;
      if (importer.SeedBundledDefaultsForApplication(
              *session, context.applicationUiState)) {
        context.saveApplicationUiState();
      }
    }
  }
  application_result_recovery::execute(
      application_result_recovery::Dependencies{
          .recover = [&context] { return context.recoverPendingResults(); },
          .reportWarning =
              [](const auto &recovery) {
                reportResultRecoveryWarning(recovery);
              },
          .startProfileServices = [&context] { context.startIrServices(); },
          .runReadyRuntime =
              [&context] { runReadyApplicationAfterResultRecovery(context); },
      });
}

int run() {
  text_runtime::FontCacheSession fontCacheSession;
  ApplicationContext context;
  return application_startup::execute(
      context.profileReady(),
      application_startup::Dependencies{
          .initializeDatabases =
              [&context] {
                return app_database_initializer::initializeApplicationDatabases(
                    context.chartRepository, context.scoreRepository,
                    context.replayRepository, context.musicPlaylistRepository);
              },
          .reportFatal =
              [&context](const application_startup::Result &result) {
                reportStartupFailure(context, result);
              },
          .reconcileReplayFiles =
              [&context] {
                const auto report = replay::reconcileProfileReplayFiles(
                    context.replayRepository,
                    std::chrono::system_clock::now() - std::chrono::hours(24));
                for (const auto &failure : report.failures) {
                  SDL_Log("Replay reconciliation deferred: %s",
                          failure.c_str());
                }
              },
          .runReadyApplication = [&context] { runReadyApplication(context); },
      });
}

void resetViewTransform(uint16_t bgaWidth, uint16_t bgaHeight,
                        bgfx::ViewId blurViewH, bgfx::ViewId blurViewV,
                        bgfx::ViewId finalView, const AppSettings &settings) {
  float ortho[16];
  bx::mtxOrtho(ortho, 0.0f, rendering::window_width, rendering::window_height,
               0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);

  bgfx::setViewTransform(rendering::ui_view, nullptr, ortho);
  bgfx::setViewRect(rendering::ui_view, rendering::ui_offset_x,
                    rendering::ui_offset_y, rendering::ui_view_width,
                    rendering::ui_view_height);
  bgfx::setViewTransform(rendering::bga_view, nullptr, ortho);
  bgfx::setViewRect(rendering::bga_view, 0, 0, bgaWidth, bgaHeight);
  bgfx::setViewTransform(rendering::bga_layer_view, nullptr, ortho);
  bgfx::setViewRect(rendering::bga_layer_view, 0, 0, bgaWidth, bgaHeight);
  bgfx::setViewTransform(rendering::clear_view, nullptr, ortho);
  bgfx::setViewRect(rendering::clear_view, 0, 0, rendering::render_width,
                    rendering::render_height);
  bgfx::setViewTransform(finalView, nullptr, ortho);
  bgfx::setViewRect(finalView, rendering::ui_offset_x, rendering::ui_offset_y,
                    rendering::ui_view_width, rendering::ui_view_height);
  bgfx::setViewTransform(blurViewH, nullptr, ortho);
  bgfx::setViewTransform(blurViewV, nullptr, ortho);

  const float aspect =
      float(rendering::window_width) / float(rendering::window_height);
  float kCameraDepth = 2.1f;
  float laneLookAtY = settings.presentation().laneLength * 0.25f;
  if (settings.activePresentationOrientation() == player_settings::PresentationOrientation::Portrait) {
    const auto safe = rendering::uiSafeAreaInsets();
    const auto frame = rendering::framePortraitPlayfield(
        settings.presentation().laneLength, settings.playAreaWidthForKeyMode(7),
        settings.presentation().laneAngleDegrees, aspect,
        {.top = float(safe.top) / rendering::window_height,
         .right = float(safe.right) / rendering::window_width,
         .bottom = float(safe.bottom) / rendering::window_height,
         .left = float(safe.left) / rendering::window_width});
    kCameraDepth = frame.cameraDepth;
    laneLookAtY = frame.lookAtY;
  }
  const float laneAngleRad = bx::toRad(settings.presentation().laneAngleDegrees);
  bx::Vec3 at = {gameplay_geometry::kPlayAreaCenterX, laneLookAtY, 0.0f};
  bx::Vec3 eye = {gameplay_geometry::kPlayAreaCenterX,
                  laneLookAtY - std::tan(laneAngleRad) * kCameraDepth,
                  -kCameraDepth};

  rendering::game_camera.edit()
      .setPosition(eye)
      .setFov(rendering::kPlayfieldVerticalFovDegrees)
      .setLookAt(at)
      .setAspectRatio(aspect)
      .setViewRect(rendering::ui_offset_x, rendering::ui_offset_y,
                   rendering::ui_view_width, rendering::ui_view_height)
      .commit();
  if (rendering::main_camera != nullptr) {
    rendering::main_camera->render();
  }
}
