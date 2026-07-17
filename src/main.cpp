#include "targets.h"
#include "AppDatabaseInitializer.h"
#include "ApplicationResultRecovery.h"
#include "ApplicationStartup.h"
#include "bgfx_helper.h"
#include "rendering/ShaderManager.h"
#include "./audio/decoder.h"
#include "bx/math.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_video.h>
#if __APPLE__
#include <SDL2/SDL_metal.h>
#endif
#include "main.h"
#include "path.h"
#include "scene/MainMenuScene.h"
#include "scene/SceneEventRouting.h"
#include "scene/play/GameplayGeometry.h"
#include "scene/SettingsScene.h"
#include "scene/SceneManager.h"
#include "view/TextInputBox.h"
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
#include <SDL_uikit_rawtouch.h>
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

  const std::filesystem::path exeDir = exePath.parent_path();
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
#if TARGET_OS_OSX
  constexpr int msaaSamples = 0;
#else
  constexpr int msaaSamples = 2;
#endif
  uint32_t flags = parseMsaaFlag(msaaSamples);

  if (TARGET_PLATFORM == iOS) {
    flags |= BGFX_RESET_VSYNC;
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
#if SDL_VERSION_ATLEAST(2, 26, 0)
  if (window != nullptr) {
    SDL_GetWindowSizeInPixels(window, &renderW, &renderH);
  }
#endif
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
    SDL_Metal_GetDrawableSize(window, &renderW, &renderH);
  }
  if (renderW <= 0 || renderH <= 0) {
    getWindowDrawableSize(window, logicalW, logicalH, renderW, renderH);
    return;
  }

  int pixelW = 0;
  int pixelH = 0;
  getWindowDrawableSize(window, logicalW, logicalH, pixelW, pixelH);
  if (pixelW > renderW && pixelH > renderH && logicalW == renderW &&
      logicalH == renderH) {
    renderW = pixelW;
    renderH = pixelH;
  }

  // iPad Display Zoom can expose a larger fullscreen display mode than SDL's
  // native-scale Metal drawable. Render at that mode to avoid compositor
  // upscaling during screenshots and app-focus transitions.
  int preferredW = 0;
  int preferredH = 0;
  if (s_iosMetalLayer != nullptr &&
      GetIOSPreferredFullscreenDrawableSize(renderW, renderH, logicalW, logicalH,
                                            preferredW, preferredH) &&
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
  render_width = renderW;
  render_height = renderH;
  window_width = design_width;
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
        rendererType == bgfx::RendererType::Vulkan
            ? bgfx::TextureFormat::RGBA8
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

      const int runExitCode = run();
      rendering::ShaderManager::getInstance().release();
      rendering::UniformCache::getInstance().destroyAll();
      bgfx::shutdown();
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
  SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
  SDL_SetHint(SDL_HINT_IME_SUPPORT_EXTENDED_TEXT, "1");
#if TARGET_OS_IPHONE
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "ambient");
#endif
#if TARGET_OS_ANDROID
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO, "1");
#endif
  // print bgfx version
  APP_DEBUG_LOG("bgfx version: %d OSX:%d", BGFX_API_VERSION, BX_PLATFORM_OSX);
  // print libsdl version
  SDL_version compiled;
  SDL_version linked;
  SDL_VERSION(&compiled);
  SDL_GetVersion(&linked);

  APP_DEBUG_LOG(
      "SDL compile version: %d.%d.%d", static_cast<int>(compiled.major),
      static_cast<int>(compiled.minor), static_cast<int>(compiled.patch));
  APP_DEBUG_LOG("SDL link version: %d.%d.%d", static_cast<int>(linked.major),
                static_cast<int>(linked.minor), static_cast<int>(linked.patch));

#if TARGET_OS_OSX
  setSmoothScrolling(true);
#endif
  using std::cerr;
  using std::endl;

  if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
    cerr << "SDL_Init Error: " << SDL_GetError() << endl;
    return EXIT_FAILURE;
  }
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
  uint32_t windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
  if (TARGET_PLATFORM == iOS || TARGET_PLATFORM == MacOS) {
    windowFlags |= SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI;
  } else if (TARGET_PLATFORM == Android) {
    windowFlags |= SDL_WINDOW_VULKAN | SDL_WINDOW_ALLOW_HIGHDPI;
  }
  SDL_Window *win = SDL_CreateWindow("AsoBMaShow", 100, 100,
                                     windowCreateWidth, windowCreateHeight,
                                     windowFlags);
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
  SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN);
  SDL_GetWindowSize(win, &windowLogicalWidth, &windowLogicalHeight);
  if (windowLogicalWidth <= 0 || windowLogicalHeight <= 0) {
    windowLogicalWidth = windowCreateWidth;
    windowLogicalHeight = windowCreateHeight;
  }
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
#if !BX_PLATFORM_EMSCRIPTEN
  SDL_SysWMinfo wmi;
  SDL_VERSION(&wmi.version);
  APP_DEBUG_LOG("SDL_major: %d, SDL_minor: %d, SDL_patch: %d\n",
                wmi.version.major, wmi.version.minor, wmi.version.patch);
  if (!SDL_GetWindowWMInfo(win, &wmi)) {
    printf("SDL_SysWMinfo could not be retrieved. SDL_Error: %s\n",
           SDL_GetError());
#if TARGET_OS_IPHONE
    APP_DEBUG_LOG("Continuing without SDL_SysWMinfo on iOS Metal path");
#else
    SDL_DestroyWindow(win);
    s_window = nullptr;
    TextInputBox::releaseCachedCursors();
    SDL_Quit();
    return EXIT_FAILURE;
#endif
  }
#endif // !BX_PLATFORM_EMSCRIPTEN

  bgfx::PlatformData pd{};
  setup_bgfx_platform_data(pd, wmi, win);
#if TARGET_OS_IPHONE
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
  bgfx_init.platformData = pd;
  SDL_Log("Using bgfx internal multithreaded mode");

  int appExitCode = runApplication(bgfx_init);

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

static void reportStartupFailure(
    const ApplicationContext &context,
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
              status.chart ? 1 : 0, status.score ? 1 : 0,
              status.replay ? 1 : 0, status.music ? 1 : 0);
    }
    break;
  case application_startup::Failure::None:
    SDL_Log("Application startup reported an unspecified fatal failure");
    break;
  }

  if (SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                               "AsoBMaShow Startup Error",
                               result.userMessage.c_str(), s_window) != 0) {
    SDL_Log("Unable to show the startup error dialog: %s", SDL_GetError());
  }
}

static void reportResultRecoveryWarning(
    const result_persistence::RecoverySummary &recovery) {
  if (SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING,
                               "AsoBMaShow Result Recovery",
                               recovery.userMessage.c_str(), s_window) != 0) {
    SDL_Log("Unable to show the result recovery warning: %s", SDL_GetError());
  }
}

static void
runReadyApplicationAfterResultRecovery(ApplicationContext &context) {
  context.bgfxResetFlags.store(s_bgfxResetFlags, std::memory_order_relaxed);
  // Use depth-sorted main view for stable layering without sequential mode.
  bgfx::setViewMode(rendering::main_view, bgfx::ViewMode::DepthAscending);
  bgfx::setViewMode(rendering::ui_view, bgfx::ViewMode::Sequential);
  bgfx::setViewMode(rendering::readback_view, bgfx::ViewMode::Sequential);
  SceneManager sceneManager(context);
  sceneManager.registerScene("MainMenu",
                             std::make_unique<MainMenuScene>(context));
  sceneManager.registerScene("Settings",
                             std::make_unique<SettingsScene>(context));
  sceneManager.changeScene("MainMenu");

  // SDL_RenderClear(ren);
  // SDL_RenderCopy(ren, tex, nullptr, nullptr);
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

  constexpr bool kEnablePerfTelemetry = true;
  uint64_t rawEventsInWindow = 0;
  uint64_t processedEventsInWindow = 0;
  uint64_t coalescedMouseMotionInWindow = 0;
  uint64_t coalescedFingerMotionInWindow = 0;
  uint64_t coalescedResizeInWindow = 0;
  float appliedLaneAngleDegrees = context.settings.laneAngleDegrees;
  float appliedLaneLength = context.settings.laneLength;
  bool hasDeferredRenderResize = false;
  int deferredRenderResizeW = 0;
  int deferredRenderResizeH = 0;
  uint32_t activeBgfxResetFlags = s_bgfxResetFlags;
  context.displayBackend = std::make_unique<display::SDLDisplayBackend>(
      s_window, TARGET_PLATFORM == iOS || TARGET_PLATFORM == Android,
      [&activeBgfxResetFlags]() { return activeBgfxResetFlags; },
      [&context, &activeBgfxResetFlags](std::uint32_t /*resetFlags*/,
                                        std::string &errorMessage)
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
            [&context, &activeBgfxResetFlags](std::uint32_t resetFlags,
                                              std::string &syncError) {
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
      });
  context.displaySettingsManager =
      std::make_unique<display::DisplaySettingsManager>(
          *context.displayBackend, context.framePacer,
          context.settings.audioVideo.video);
  const auto startupDisplayResult =
      context.displaySettingsManager->applySafeStartupIntent();
  if (!startupDisplayResult.message.empty()) {
    SDL_Log("%s", startupDisplayResult.message.c_str());
  }
  context.framePacer.reset(lastFrameTime);
  bool pacingExportActive =
      context.replayVideoExportActive.load(std::memory_order_acquire);
  constexpr int kBackgroundEventWaitTimeoutMs = 1000;
  auto isAppBackgroundEvent = [](const SDL_Event &event) {
    return event.type == SDL_APP_WILLENTERBACKGROUND ||
           event.type == SDL_APP_DIDENTERBACKGROUND ||
           (event.type == SDL_WINDOWEVENT &&
            (event.window.event == SDL_WINDOWEVENT_MINIMIZED ||
             event.window.event == SDL_WINDOWEVENT_HIDDEN ||
             event.window.event == SDL_WINDOWEVENT_FOCUS_LOST));
  };
  auto isAppForegroundEvent = [](const SDL_Event &event) {
    return event.type == SDL_APP_WILLENTERFOREGROUND ||
           event.type == SDL_APP_DIDENTERFOREGROUND ||
           (event.type == SDL_WINDOWEVENT &&
            (event.window.event == SDL_WINDOWEVENT_RESTORED ||
             event.window.event == SDL_WINDOWEVENT_SHOWN ||
             event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED));
  };
  auto setAppBackground = [&](bool background) {
    if (background && context.displaySettingsManager) {
      if (const auto rollbackResult =
              context.displaySettingsManager->onFocusLost();
          rollbackResult.has_value() && !rollbackResult->message.empty()) {
        SDL_Log("%s", rollbackResult->message.c_str());
      }
    }
    const bool previous =
        context.appInBackground.exchange(background, std::memory_order_acq_rel);
    if (previous == background) {
      return;
    }
    context.jukebox.setVisualsSuspended(background);
    if (!background) {
      lastFrameTime = std::chrono::steady_clock::now();
      context.framePacer.reset(lastFrameTime);
      context.jukebox.seekVisualsToSongTime(context.jukebox.getTimeMicros());
    }
  };
#if TARGET_OS_ANDROID
  bool androidSystemSuspended = false;
  bool androidRenderSuspended = false;
  bool androidResumeResizePending = false;
#endif
  while (!context.quitFlag) {

    auto currentFrameTime = std::chrono::steady_clock::now();
    const bool exportActiveForPacing =
        context.replayVideoExportActive.load(std::memory_order_acquire);
    if (exportActiveForPacing != pacingExportActive) {
      pacingExportActive = exportActiveForPacing;
      context.framePacer.reset(currentFrameTime);
    }
    if (context.displaySettingsManager) {
      if (const auto previewResult =
              context.displaySettingsManager->tick(currentFrameTime)) {
        if (!previewResult->message.empty()) {
          SDL_Log("%s", previewResult->message.c_str());
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

      // set bgfx resolution
      bgfx::reset(rendering::render_width, rendering::render_height,
                  activeBgfxResetFlags);
      context.bgfxResetFlags.store(activeBgfxResetFlags,
                                   std::memory_order_relaxed);
      APP_DEBUG_LOG("Render size: %d x %d (logical: %d x %d, scale %.2f)",
                    rendering::render_width, rendering::render_height,
                    logicalW, logicalH, s_renderScale);
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
      SDL_SysWMinfo wmi;
      SDL_VERSION(&wmi.version);
      if (!SDL_GetWindowWMInfo(s_window, &wmi)) {
        SDL_Log("Failed to refresh Android window handle: %s",
                SDL_GetError());
        return false;
      }
      bgfx::PlatformData pd{};
      setup_bgfx_platform_data(pd, wmi, s_window);
      if (pd.nwh == nullptr) {
        SDL_Log("Android window handle is not ready yet");
        return false;
      }
      bgfx::setPlatformData(pd);
      return true;
    };

    auto applyAndroidRenderSuspend = [&](bool suspend) {
      if (androidRenderSuspended == suspend) {
        if (suspend) {
          NotifyAndroidExternalActivityRenderPaused();
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
      bgfx::frame();
      androidRenderSuspended = suspend;
      SDL_Log("Android rendering %s", suspend ? "suspended" : "resumed");
      if (suspend) {
        NotifyAndroidExternalActivityRenderPaused();
      } else {
        androidResumeResizePending = true;
      }
      return true;
    };

    auto syncAndroidRenderSuspend = [&]() {
      const bool shouldSuspend =
          androidSystemSuspended ||
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
      ++processedEventsInWindow;
      if (event.type == SDL_QUIT) {
        context.quitFlag = true;
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

      if (scene_event_routing::shouldDispatchToScene(event.type)) {
        auto result = sceneManager.handleEvents(event);
        if (result.quit) {
          context.quitFlag = true;
        }
      }

      // on window resize
      if (event.type == SDL_WINDOWEVENT &&
          (event.window.event == SDL_WINDOWEVENT_RESIZED ||
           event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
        const int logicalW = event.window.data1;
        const int logicalH = event.window.data2;
        if (!applyWindowResize(logicalW, logicalH)) {
          deferWindowResize(logicalW, logicalH);
        }
      }

      if (event.type == SDL_TEXTEDITING_EXT) {
        SDL_free(event.editExt.text);
        event.editExt.text = nullptr;
      }
    };

    auto waitForBackgroundEvent = [&]() {
      SDL_Event waitEvent{};
      if (SDL_WaitEventTimeout(&waitEvent, kBackgroundEventWaitTimeoutMs)) {
        ++rawEventsInWindow;
        context.inputDeviceRegistry.handleSdlEventAndDispatch(waitEvent);
        processEvent(waitEvent);
      }
    };

    while (SDL_PollEvent(&e)) {
      ++rawEventsInWindow;
      context.inputDeviceRegistry.handleSdlEventAndDispatch(e);

      if (e.type == SDL_MOUSEMOTION) {
        pendingMouseMotion = e;
        hasPendingMouseMotion = true;
        ++pendingMouseMotionCount;
        continue;
      }
      if (e.type == SDL_FINGERMOTION) {
        auto existing =
            std::find_if(pendingFingerMotions.begin(),
                         pendingFingerMotions.end(),
                         [&](const SDL_Event &pending) {
                           return pending.tfinger.touchId == e.tfinger.touchId &&
                                  pending.tfinger.fingerId ==
                                      e.tfinger.fingerId;
                         });
        if (existing != pendingFingerMotions.end()) {
          *existing = e;
        } else {
          pendingFingerMotions.push_back(e);
        }
        ++pendingFingerMotionCount;
        continue;
      }
      if (e.type == SDL_WINDOWEVENT &&
          (e.window.event == SDL_WINDOWEVENT_RESIZED ||
           e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
        pendingResizeEvent = e;
        hasPendingResize = true;
        ++pendingResizeCount;
        continue;
      }

      processEvent(e);
    }

    if (hasPendingResize) {
      processEvent(pendingResizeEvent);
      if (pendingResizeCount > 1) {
        coalescedResizeInWindow += (pendingResizeCount - 1);
      }
    }
    if (!pendingFingerMotions.empty()) {
      for (const auto &pendingFingerMotion : pendingFingerMotions) {
        processEvent(pendingFingerMotion);
      }
      if (pendingFingerMotionCount > pendingFingerMotions.size()) {
        coalescedFingerMotionInWindow +=
            pendingFingerMotionCount - pendingFingerMotions.size();
      }
    }
    if (hasPendingMouseMotion) {
      processEvent(pendingMouseMotion);
      if (pendingMouseMotionCount > 1) {
        coalescedMouseMotionInWindow += (pendingMouseMotionCount - 1);
      }
    }
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
        std::abs(appliedLaneAngleDegrees - context.settings.laneAngleDegrees) >
            0.001f ||
        std::abs(appliedLaneLength - context.settings.laneLength) > 0.001f;
    if (laneTransformChanged &&
        !context.replayVideoExportActive.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> bgfxLock(context.bgfxRenderMutex,
                                           std::try_to_lock);
      if (bgfxLock.owns_lock()) {
        appliedLaneAngleDegrees = context.settings.laneAngleDegrees;
        appliedLaneLength = context.settings.laneLength;
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
    const bool replayExportUiFrameRequested =
        context.replayVideoExportUiFrameRequested.load(
            std::memory_order_acquire);
    if (!replayExportActive || replayExportUiFrameRequested) {
      std::unique_lock<std::mutex> bgfxLock(context.bgfxRenderMutex,
                                           std::try_to_lock);
      if (bgfxLock.owns_lock() &&
          context.replayVideoExportActive.load(std::memory_order_acquire) &&
          context.replayVideoExportUiFrameRequested.load(
              std::memory_order_acquire)) {
        bgfx::touch(rendering::clear_view);
        bgfx::touch(rendering::ui_view);
        sceneManager.render();
        bgfx::frame();
        context.replayVideoExportUiFrameSerial.fetch_add(
            1, std::memory_order_release);
        context.replayVideoExportUiFrameRequested.store(
            false, std::memory_order_release);
        renderedFrame = true;
      } else if (bgfxLock.owns_lock() &&
                 !context.replayVideoExportActive.load(
                     std::memory_order_acquire)) {
        const bool hasActiveVisuals = context.jukebox.hasActiveVisuals();

        bgfx::touch(rendering::clear_view);
        bgfx::touch(rendering::ui_view);
        if (hasActiveVisuals) {
          bgfx::touch(rendering::bga_view);
          bgfx::touch(rendering::bga_layer_view);
          bgfx::touch(s_blurPass->finalView());
          bgfx::touch(s_blurPass->blurViewH());
          bgfx::touch(s_blurPass->blurViewV());
        }

        sceneManager.render();
        if (hasActiveVisuals) {
          const bool ignoreBgaPostOptions =
              context.ignoreBgaPostOptions.load(std::memory_order_acquire);
          context.jukebox.render();
          s_blurPass->setBlurStrength(ignoreBgaPostOptions
                                           ? 0.0f
                                           : context.settings.bgaBlurStrength);
          s_postProcess.apply();
          rendering::renderFullscreenTextureTint(
              s_blurPass->outputTexture(), s_blurPass->finalView(),
              ignoreBgaPostOptions
                  ? 1.0f
                  : static_cast<float>(
                        context.settings.bgaBrightnessPercent) /
                        100.0f);
        }
        bgfx::frame();
        renderedFrame = true;
      }
    }

    if constexpr (kEnablePerfTelemetry) {
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
            "FPS %.1f | Avg %.1f | 1%% Low %.1f | Audio %.2f us (%.2f Hz) | "
            "Events raw %llu proc %llu coalesced M/F/R %llu/%llu/%llu",
            currentFps, avgFps, low1Fps, avgDeltaTime, freq,
            static_cast<unsigned long long>(rawEventsInWindow),
            static_cast<unsigned long long>(processedEventsInWindow),
            static_cast<unsigned long long>(coalescedMouseMotionInWindow),
            static_cast<unsigned long long>(coalescedFingerMotionInWindow),
            static_cast<unsigned long long>(coalescedResizeInWindow));
        rawEventsInWindow = 0;
        processedEventsInWindow = 0;
        coalescedMouseMotionInWindow = 0;
        coalescedFingerMotionInWindow = 0;
        coalescedResizeInWindow = 0;
      }
    }

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
      const auto waitDuration =
          context.framePacer.remaining(std::chrono::steady_clock::now());
      if (waitDuration > std::chrono::steady_clock::duration::zero()) {
#if TARGET_OS_IPHONE
        const auto waitMicros = std::max<long long>(
            1, std::chrono::duration_cast<std::chrono::microseconds>(
                   waitDuration)
                   .count());
        WaitIOSMainRunLoopForMicros(waitMicros);
#else
        std::this_thread::sleep_for(waitDuration);
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
  if (context.displaySettingsManager) {
    const auto shutdownResult = context.displaySettingsManager->shutdown();
    if (!shutdownResult.message.empty()) {
      SDL_Log("%s", shutdownResult.message.c_str());
    }
  }
  context.displaySettingsManager.reset();
  context.displayBackend.reset();
  s_postProcess.shutdown();
  // bgfx::destroy(vbh);
  // bgfx::destroy(ibh);
}

static void runReadyApplication(ApplicationContext &context) {
  application_result_recovery::execute(
      application_result_recovery::Dependencies{
          .recover = [&context] { return context.recoverPendingResults(); },
          .reportWarning = [](const auto &recovery) {
            reportResultRecoveryWarning(recovery);
          },
          .runReadyRuntime = [&context] {
            runReadyApplicationAfterResultRecovery(context);
          },
      });
}

int run() {
  ApplicationContext context;
  return application_startup::execute(
      context.profileReady(),
      application_startup::Dependencies{
          .initializeDatabases = [&context] {
            return app_database_initializer::initializeApplicationDatabases(
                context.chartRepository, context.scoreRepository,
                context.replayRepository,
                context.musicPlaylistRepository);
          },
          .reportFatal = [&context](const application_startup::Result &result) {
            reportStartupFailure(context, result);
          },
          .runReadyApplication = [&context] {
            runReadyApplication(context);
          },
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

  constexpr float kCameraDepth = 2.1f;
  const float laneLookAtY = settings.laneLength * 0.25f;
  const float laneAngleRad = bx::toRad(settings.laneAngleDegrees);
  bx::Vec3 at = {gameplay_geometry::kPlayAreaCenterX, laneLookAtY, 0.0f};
  bx::Vec3 eye = {gameplay_geometry::kPlayAreaCenterX,
                  laneLookAtY - std::tan(laneAngleRad) * kCameraDepth,
                  -kCameraDepth};

  float aspect =
      float(rendering::window_width) / float(rendering::window_height);
  rendering::game_camera.edit()
      .setPosition(eye)
      .setLookAt(at)
      .setAspectRatio(aspect)
      .setViewRect(rendering::ui_offset_x, rendering::ui_offset_y,
                   rendering::ui_view_width, rendering::ui_view_height)
      .commit();
  if (rendering::main_camera != nullptr) {
    rendering::main_camera->render();
  }
}
