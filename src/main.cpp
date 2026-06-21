#include "targets.h"
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
#include "scene/MainMenuScene.h"
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
#ifdef _WIN32
#include <windows.h>

#elif __APPLE__

#include "TargetConditionals.h"
#if TARGET_OS_IPHONE
#include "iOSNatives.hpp"
// define something for iphone
#include <dirent.h>
#include <sys/stat.h>
#else
// define something for OSX
#include "MacNatives.h"
#include <dirent.h>
#include <sys/stat.h>
#endif
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

static int runApplication(const bgfx::Init &bgfxInit) {
  SDL_Log("bgfx_init: %d x %d", bgfxInit.resolution.width,
          bgfxInit.resolution.height);
  if (!bgfx::init(bgfxInit)) {
    SDL_Log("bgfx::init failed");
    return EXIT_FAILURE;
  }
  SDL_Log("bgfx renderer: %s", bgfx::getRendererName(bgfx::getRendererType()));
  // Keep debug rendering disabled in normal runtime to avoid perturbing
  // frame pacing and post-process output.
  // bgfx::setDebug(BGFX_DEBUG_TEXT);

  run();
  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return EXIT_SUCCESS;
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
        exePath = std::filesystem::absolute(exePath);
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
          exePath = std::filesystem::absolute(exePath);
        }
      }
    }
  }
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
          exePath = std::filesystem::absolute(exePath);
        }
      }
    }
  }
#endif

  if (!exePath.empty()) {
    std::filesystem::path exeDir = exePath.parent_path();
    if (!exeDir.empty() && std::filesystem::exists(exeDir)) {
      std::filesystem::current_path(exeDir);
      APP_DEBUG_LOG("Changed working directory to: %s",
                    exeDir.string().c_str());
    }
  }

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
  SDL_Log("Render scale: %.2f | bgfx reset flags: 0x%08x", s_renderScale,
          s_bgfxResetFlags);

  int windowCreateWidth = 1280;
  int windowCreateHeight = 720;
  SDL_Window *win = SDL_CreateWindow(
      "AsoBMaShow", 100, 100, windowCreateWidth, windowCreateHeight,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
          (TARGET_PLATFORM == iOS || TARGET_PLATFORM == MacOS
               ? SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI
               : 0));
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

#if TARGET_OS_IPHONE
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
#if __APPLE__
  bgfx_init.type = bgfx::RendererType::Metal; // force Metal on Apple platforms
#else
  bgfx_init.type = bgfx::RendererType::Count; // auto choose renderer
#endif
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

void run() {
  ApplicationContext context;
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
  while (!context.quitFlag) {

    auto currentFrameTime = std::chrono::steady_clock::now();
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

    auto shouldDispatchToScene = [](Uint32 eventType) {
      switch (eventType) {
      case SDL_QUIT:
      case SDL_WINDOWEVENT:
      case SDL_KEYDOWN:
      case SDL_KEYUP:
      case SDL_TEXTINPUT:
      case SDL_TEXTEDITING:
      case SDL_TEXTEDITING_EXT:
      case SDL_MOUSEMOTION:
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
      case SDL_MOUSEWHEEL:
      case SDL_FINGERDOWN:
      case SDL_FINGERMOTION:
      case SDL_FINGERUP:
        return true;
      default:
        return false;
      }
    };

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
                  s_bgfxResetFlags);
      APP_DEBUG_LOG("Render size: %d x %d (logical: %d x %d, scale %.2f)",
                    rendering::render_width, rendering::render_height,
                    logicalW, logicalH, s_renderScale);
      s_postProcess.resize(rendering::render_width, rendering::render_height);
      context.restoreGameplayRenderViews();
      return true;
    };

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

#if TARGET_OS_IPHONE
      if (event.type == SDL_APP_WILLENTERFOREGROUND ||
          event.type == SDL_APP_DIDENTERFOREGROUND ||
          (event.type == SDL_WINDOWEVENT &&
           (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
            event.window.event == SDL_WINDOWEVENT_RESTORED ||
            event.window.event == SDL_WINDOWEVENT_SHOWN))) {
        restoreIOSViewportAfterKeyboardFocus();
      }
#endif

      if (shouldDispatchToScene(event.type)) {
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

    while (SDL_PollEvent(&e)) {
      ++rawEventsInWindow;

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
          context.jukebox.render();
          s_postProcess.apply();
          rendering::renderFullscreenTextureTint(
              s_blurPass->outputTexture(), s_blurPass->finalView(),
              static_cast<float>(context.settings.bgaBrightnessPercent) /
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

    if (!renderedFrame) {
      SDL_Delay(1);
    }
    sceneManager.handleDeferred();
    context.currentFrame++;
    //
  }
  sceneManager.cleanup();
  s_postProcess.shutdown();
  // bgfx::destroy(vbh);
  // bgfx::destroy(ibh);
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
