#include "ReplayVideoExporter.h"
#include "replay/CourseReplayConsumer.h"

#include "ChartPlaybackDuration.h"
#include "CoursePlaySession.h"
#include "PlayOptionUtils.h"
#include "PreparationPlan.h"
#include "RAII.h"
#include "ReplayResultStateBuilder.h"
#include "ResultPresentationUtils.h"
#include "Utils.h"
#include "audio/ChartAudioRenderer.h"
#include "audio/GameplayBgaMissStateTracker.h"
#include "audio/SoundFileIO.h"
#include "main.h"
#include "path.h"
#include "rendering/BlurPass.h"
#include "rendering/Color.h"
#include "rendering/RenderPlan.h"
#include "rendering/SimpleBatchRenderer.h"
#include "rendering/common.h"
#include "scene/PracticeAnalyticsPresentation.h"
#include "scene/PracticeAnalyticsView.h"
#include "scene/play/BMSRenderer.h"
#include "scene/play/GamePlayTiming.h"
#include "scene/play/ReplayPlayfieldPresentation.h"
#include "scene/play/ReplayVideoGameplayPreflight.h"
#include "skin/DefaultSkin.h"
#include "view/UiTheme.h"
#include "view/View.h"
#include "targets.h"
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
#include "iOSNatives.hpp"
#endif

#if __APPLE__
#include <Accelerate/Accelerate.h>
#include <sys/sysctl.h>
#endif

#include <SDL2/SDL.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <sndfile.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
constexpr int kExportSampleRate = chart_audio::kOutputSampleRate;
constexpr int kExportChannels = chart_audio::kOutputChannels;
constexpr int kDefaultExportFps = 120;
constexpr int kH264HighProfile = 100;
constexpr long long kResultSceneTailMicros = 10000000;

PracticeAnalyticsView *addReplayResultAnalytics(
    View &resultRoot, const bms_parser::Chart &chart,
    const ReplayData &replay) {
  const std::span<const ReplayData> attempts(&replay, 1);
  auto *host = new View();
  host->setName("timingAnalytics");
  host->setWidthPercent(100.0f);
  host->setHeight(
      practice_analytics_presentation::kPreferredAnalyticsHeight);
  host->setMinHeight(
      practice_analytics_presentation::kMinimumAnalyticsHeight);
  host->setFlexShrink(1.0f);
  host->setFlexDirection(FlexDirection::Column);
  host->setAlignItems(YGAlignStretch);
  auto *analytics = new PracticeAnalyticsView(
      practice::ResultModel(chart, attempts, 0));
  host->addView(analytics);
  resultRoot.addView(host);
  return analytics;
}

class ReplayVideoExportLog;
void replayExportLog(ReplayVideoExportLog *log, const char *format, ...);
ReplayVideoExportOptions
resolveReplayVideoExportOptions(const ReplayVideoExportOptions &options);

struct PreparedReplayGameplayPresentation {
  std::unique_ptr<ReplayPlayfieldPresentation> presentation;
};

GameplaySkinSessionServices
replayGameplaySkinSessionServices(ApplicationContext &context) {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  return {.acquire = context.acquireGameplaySkinForNextChart,
          .storageRoots = context.skinStorageRoots
                              ? &*context.skinStorageRoots
                              : nullptr,
          .resourcePreparation = context.skinResourcePreparationService.get(),
          .liveResourceCounters = context.skinLiveResourceCounters,
          .configurationWrites = context.skinConfigurationWriteQueue.get(),
          .diagnosticHistory = context.skinDiagnosticHistory.get()};
#else
  (void)context;
  return {};
#endif
}

[[nodiscard]] std::optional<ReplayVideoExportResult>
preflightReplayGameplayPresentation(
    ApplicationContext &context, bms_parser::Chart &chart,
    const ReplayData &replay, const AppSettings &settings,
    const preparation::Plan &preparationPlan,
    const ReplayVideoExportOptions &options,
    PreparedReplayGameplayPresentation &prepared, ReplayVideoExportLog *log,
    const skin::RuntimeSkinConfigurationSelection *pinnedRuntimeSelection =
        nullptr) {
  const auto resolvedOptions = resolveReplayVideoExportOptions(options);
  const auto result = replay_video_export::preflightReplayGameplayPresentation(
      chart, replay, settings, preparationPlan,
      replay_video_export::replayGameplayPresentationConfig(
          settings, settings.playAreaWidthForKeyMode(chart.Meta.KeyMode),
          chart,
          resolvedOptions.renderTouchPoints,
          resolvedOptions.renderReplayGhosts),
      resolvedOptions.width, resolvedOptions.height,
      context.jukebox, replayGameplaySkinSessionServices(context),
      context.rendererAccess,
      prepared.presentation, pinnedRuntimeSelection);
  if (result) {
    replayExportLog(log, "Replay export skin preflight failed: %s",
                    result->message.c_str());
  }
  return result;
}

std::optional<std::string>
ensureReplayExportDirectoryError(const std::filesystem::path &path,
                                 const char *failureMessage) {
  std::error_code error;
  if (Utils::EnsureDirectoryExists(path, error)) {
    return std::nullopt;
  }

  return std::string(failureMessage) + " (" +
         fspath_to_utf8(path) + "): " + error.message();
}

void removeReplayExportOutputFile(const std::filesystem::path &outputPath) {
  std::error_code error;
  std::filesystem::remove(outputPath, error);
}

void removeReplayExportWorkDirectory(const std::filesystem::path &tempDir,
                                     ReplayVideoExportLog *log = nullptr) {
  std::error_code error;
  std::filesystem::remove_all(tempDir, error);
  if (error && log != nullptr) {
    replayExportLog(log, "Replay export could not clean work directory: %s",
                    fspath_to_utf8(tempDir).c_str());
  }
}

int makeEvenExportDimension(int value) { return std::max(2, value & ~1); }

int replayVideoSourceWidth() {
  return std::max({2, rendering::render_width, rendering::window_width});
}

int replayVideoSourceHeight() {
  return std::max({2, rendering::render_height, rendering::window_height});
}

ReplayVideoExportOptions
resolveReplayVideoExportOptions(const ReplayVideoExportOptions &options) {
  const int sourceWidth = replayVideoSourceWidth();
  const int sourceHeight = replayVideoSourceHeight();
  const double aspectRatio =
      static_cast<double>(sourceWidth) / static_cast<double>(sourceHeight);
  const bool hasWidth = options.width > 0;
  const bool hasHeight = options.height > 0;

  int width = sourceWidth;
  int height = sourceHeight;
  if (hasWidth && hasHeight) {
    width = options.width;
    height = options.height;
  } else if (hasWidth) {
    width = options.width;
    height =
        static_cast<int>(std::lround(static_cast<double>(width) / aspectRatio));
  } else if (hasHeight) {
    height = options.height;
    width = static_cast<int>(
        std::lround(static_cast<double>(height) * aspectRatio));
  }

  ReplayVideoExportOptions resolved;
  resolved.width = makeEvenExportDimension(width);
  resolved.height = makeEvenExportDimension(height);
  resolved.fps =
      std::clamp(options.fps > 0 ? options.fps : kDefaultExportFps, 1, 120);
  resolved.includeResultScreen = options.includeResultScreen;
  resolved.renderTouchPoints = options.renderTouchPoints;
  resolved.renderReplayGhosts = options.renderReplayGhosts;
  resolved.pacemakerTarget = options.pacemakerTarget;
  resolved.progressCallback = options.progressCallback;
  return resolved;
}

int64_t replayVideoBitRate(int width, int height, int fps) {
  const int64_t pixelsPerSecond = static_cast<int64_t>(width) *
                                  static_cast<int64_t>(height) *
                                  static_cast<int64_t>(fps);
  return std::clamp<int64_t>(pixelsPerSecond / 6, 8000000, 80000000);
}

int replayVideoEncoderThreadCount() {
  const auto hardwareThreads = std::thread::hardware_concurrency();
  if (hardwareThreads <= 1) {
    return 1;
  }
  return std::clamp(static_cast<int>(hardwareThreads) - 1, 1, 16);
}

int replayVideoPixelConvertThreadCount() {
  const auto hardwareThreads = std::thread::hardware_concurrency();
  if (hardwareThreads <= 1) {
    return 1;
  }
  return std::clamp(static_cast<int>(hardwareThreads) - 1, 1, 8);
}

bool replayVideoEncoderSupportsFrameThreads(const AVCodec *codec) {
  return codec != nullptr &&
         (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS) != 0;
}

bool replayVideoEncoderSupportsOtherThreads(const AVCodec *codec) {
  return codec != nullptr &&
         (codec->capabilities & AV_CODEC_CAP_OTHER_THREADS) != 0;
}

int replayVideoH264Level(int width, int height, int fps) {
  const int64_t macroblocksPerFrame = static_cast<int64_t>((width + 15) / 16) *
                                      static_cast<int64_t>((height + 15) / 16);
  const int64_t macroblocksPerSecond =
      macroblocksPerFrame * static_cast<int64_t>(fps);

  if (macroblocksPerFrame > 22080 || macroblocksPerSecond > 983040) {
    return 52;
  }
  if (macroblocksPerFrame > 8704 || macroblocksPerSecond > 589824) {
    return 51;
  }
  if (macroblocksPerSecond > 522240) {
    return 50;
  }
  if (macroblocksPerSecond > 245760) {
    return 42;
  }
  return 41;
}

std::string replayVideoH264LevelString(int level) {
  return std::to_string(level / 10) + "." + std::to_string(level % 10);
}

size_t replayVideoFrameBytes(int width, int height) {
  return static_cast<size_t>(width) * static_cast<size_t>(height) * 4ULL;
}

uint64_t replayVideoPhysicalMemoryBytes() {
#if __APPLE__
  uint64_t memoryBytes = 0;
  size_t size = sizeof(memoryBytes);
  if (sysctlbyname("hw.memsize", &memoryBytes, &size, nullptr, 0) == 0 &&
      memoryBytes > 0) {
    return memoryBytes;
  }
#endif
  return 0;
}

size_t replayVideoFrameBufferMemoryBudgetBytes() {
  constexpr uint64_t kDefaultBudgetBytes = 128ULL * 1024ULL * 1024ULL;
  constexpr uint64_t kMinBudgetBytes = 128ULL * 1024ULL * 1024ULL;
  constexpr uint64_t kMaxBudgetBytes = 384ULL * 1024ULL * 1024ULL;
  const uint64_t physicalMemoryBytes = replayVideoPhysicalMemoryBytes();
  if (physicalMemoryBytes == 0) {
    return static_cast<size_t>(kDefaultBudgetBytes);
  }
  return static_cast<size_t>(
      std::clamp<uint64_t>(physicalMemoryBytes / uint64_t{32},
                           kMinBudgetBytes, kMaxBudgetBytes));
}

size_t replayVideoFrameBufferSlotBytes(int width, int height) {
  return replayVideoFrameBytes(width, height) * 2ULL;
}

size_t replayVideoFrameBufferCount(int width, int height) {
  constexpr size_t kMaxFrameBufferCount = 24;
  const size_t frameSlotBytes = replayVideoFrameBufferSlotBytes(width, height);
  const size_t memoryBudgetBytes = replayVideoFrameBufferMemoryBudgetBytes();
  const size_t memoryLimitedBuffers =
      frameSlotBytes == 0 ? 3 : memoryBudgetBytes / frameSlotBytes;
  const auto hardwareThreads = std::thread::hardware_concurrency();
  if (hardwareThreads <= 2) {
    return std::clamp(memoryLimitedBuffers, static_cast<size_t>(3),
                      static_cast<size_t>(4));
  }
  const size_t hardwareLimitedBuffers =
      std::clamp(static_cast<size_t>(hardwareThreads) * static_cast<size_t>(2),
                 static_cast<size_t>(4), kMaxFrameBufferCount);
  return std::clamp(std::min(memoryLimitedBuffers, hardwareLimitedBuffers),
                    static_cast<size_t>(3), kMaxFrameBufferCount);
}

long long elapsedMicros(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

long long gameplayEndMicrosForReplay(const bms_parser::Chart &chart,
                                     const ReplayData &replay) {
  return replay_video_export::replayGameplayStatePlayDeadlineMicros(chart,
                                                                       replay);
}

ReplayData replayThroughFailure(const ReplayData &replay,
                                std::optional<long long> failureMicros) {
  ReplayData result = replay;
  if (!failureMicros.has_value()) {
    return result;
  }
  result.events.erase(
      std::ranges::find_if(result.events, [&](const ReplayEvent &event) {
        return event.songTimeMicros > *failureMicros;
      }),
      result.events.end());
  result.finalGauge = 0.0f;
  result.clearType = kClearTypeFailedRank;
  result.finalScore = result.events.empty() ? 0 : result.events.back().score;
  result.maxCombo = 0;
  for (const ReplayEvent &event : result.events) {
    result.maxCombo = std::max(result.maxCombo, event.combo);
  }
  return result;
}

long long courseStageGameplayDurationMicrosForReplay(
    const bms_parser::Chart &chart, const ReplayData &replay,
    long long audioDurationMicros,
    bool includeResultScreen, const preparation::Plan &preparationPlan,
    long long audioOffsetMicros) {
  const long long transitionDurationMicros =
      preparationPlan.realTimeAtGameplayTime(
          gameplayEndMicrosForReplay(chart, replay), audioOffsetMicros) +
      chart_playback_duration::kGameplayResultTransitionDelayMicros;
  if (includeResultScreen) {
    return transitionDurationMicros;
  }
  return std::max(transitionDurationMicros, std::max(0LL, audioDurationMicros));
}

std::string formatString(const char *format, va_list args) {
  va_list sizeArgs;
  va_copy(sizeArgs, args);
  const int length = std::vsnprintf(nullptr, 0, format, sizeArgs);
  va_end(sizeArgs);
  if (length < 0) {
    return format != nullptr ? std::string(format) : std::string();
  }
  if (length == 0) {
    return {};
  }

  std::vector<char> buffer(static_cast<size_t>(length) + 1);
  std::vsnprintf(buffer.data(), buffer.size(), format, args);
  return std::string(buffer.data(), static_cast<size_t>(length));
}

class ReplayVideoExportLog {
public:
  explicit ReplayVideoExportLog(const std::filesystem::path &path)
      : startedAt(std::chrono::steady_clock::now()), path(path) {
    file.open(path, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
      SDL_Log("Replay export could not open log file: %s",
              fspath_to_utf8(path).c_str());
    }
  }

  void write(const std::string &message) {
    SDL_Log("%s", message.c_str());
    if (!file.is_open()) {
      return;
    }
    const double elapsedSeconds =
        static_cast<double>(elapsedMicros(startedAt)) / 1000000.0;
    file << std::fixed << std::setprecision(3) << elapsedSeconds << "s "
         << message << '\n';
    file.flush();
  }

private:
  std::chrono::steady_clock::time_point startedAt;
  std::filesystem::path path;
  std::ofstream file;
};

void replayExportLog(ReplayVideoExportLog *log, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const std::string message = formatString(format, args);
  va_end(args);

  if (log != nullptr) {
    log->write(message);
  }
}

void reportReplayExportProgress(const ReplayVideoExportOptions &options,
                                double fraction, const std::string &message,
                                std::size_t frameIndex = 0,
                                std::size_t frameCount = 0) {
  if (!options.progressCallback) {
    return;
  }

  options.progressCallback({.fraction = std::clamp(fraction, 0.0, 1.0),
                            .message = message,
                            .frameIndex = frameIndex,
                            .frameCount = frameCount});
}

struct ReplayVideoRenderGeometryState {
  int windowWidth = rendering::window_width;
  int windowHeight = rendering::window_height;
  int renderWidth = rendering::render_width;
  int renderHeight = rendering::render_height;
  float widthScale = rendering::widthScale;
  float heightScale = rendering::heightScale;
  float uiScaleX = rendering::ui_scale_x;
  float uiScaleY = rendering::ui_scale_y;
  int uiOffsetX = rendering::ui_offset_x;
  int uiOffsetY = rendering::ui_offset_y;
  int uiViewWidth = rendering::ui_view_width;
  int uiViewHeight = rendering::ui_view_height;
};

void restoreReplayVideoRenderGeometry(
    const ReplayVideoRenderGeometryState &state) {
  rendering::window_width = state.windowWidth;
  rendering::window_height = state.windowHeight;
  rendering::render_width = state.renderWidth;
  rendering::render_height = state.renderHeight;
  rendering::widthScale = state.widthScale;
  rendering::heightScale = state.heightScale;
  rendering::ui_scale_x = state.uiScaleX;
  rendering::ui_scale_y = state.uiScaleY;
  rendering::ui_offset_x = state.uiOffsetX;
  rendering::ui_offset_y = state.uiOffsetY;
  rendering::ui_view_width = state.uiViewWidth;
  rendering::ui_view_height = state.uiViewHeight;
}

class ScopedReplayVideoRenderGeometry {
public:
  ScopedReplayVideoRenderGeometry(int exportWidth, int exportHeight)
      : exportWidth(exportWidth), exportHeight(exportHeight) {}

  ~ScopedReplayVideoRenderGeometry() { restorePrimary(); }

  void applyExport() { rendering::updateUIScale(exportWidth, exportHeight); }

  void restorePrimary() {
    restoreReplayVideoRenderGeometry(primary);
  }

private:
  ReplayVideoRenderGeometryState primary;
  int exportWidth = 0;
  int exportHeight = 0;
};

void restorePrimaryRenderViews(ApplicationContext *context = nullptr) {
  for (const auto view : rendering::kGameplayOutputViews) {
    bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
  }
  bgfx::setViewFrameBuffer(rendering::readback_view, BGFX_INVALID_HANDLE);
  if (context != nullptr && context->restoreGameplayRenderViews) {
    context->restoreGameplayRenderViews();
    return;
  }

  bgfx::setViewFrameBuffer(rendering::bga_view, BGFX_INVALID_HANDLE);
  bgfx::setViewFrameBuffer(rendering::bga_layer_view, BGFX_INVALID_HANDLE);
  bgfx::setViewRect(rendering::clear_view, 0, 0,
                    static_cast<uint16_t>(rendering::render_width),
                    static_cast<uint16_t>(rendering::render_height));
  bgfx::setViewRect(rendering::bga_view, 0, 0,
                    static_cast<uint16_t>(rendering::render_width),
                    static_cast<uint16_t>(rendering::render_height));
  bgfx::setViewRect(rendering::bga_layer_view, 0, 0,
                    static_cast<uint16_t>(rendering::render_width),
                    static_cast<uint16_t>(rendering::render_height));
  bgfx::setViewRect(rendering::ui_view, rendering::ui_offset_x,
                    rendering::ui_offset_y,
                    static_cast<uint16_t>(rendering::ui_view_width),
                    static_cast<uint16_t>(rendering::ui_view_height));
  bgfx::setViewRect(rendering::main_view, rendering::ui_offset_x,
                    rendering::ui_offset_y,
                    static_cast<uint16_t>(rendering::ui_view_width),
                    static_cast<uint16_t>(rendering::ui_view_height));
  bgfx::setViewRect(rendering::final_view, rendering::ui_offset_x,
                    rendering::ui_offset_y,
                    static_cast<uint16_t>(rendering::ui_view_width),
                    static_cast<uint16_t>(rendering::ui_view_height));
  bgfx::setViewRect(rendering::readback_view, 0, 0,
                    static_cast<uint16_t>(rendering::render_width),
                    static_cast<uint16_t>(rendering::render_height));

  float ortho[16];
  bx::mtxOrtho(ortho, 0.0f, rendering::window_width, rendering::window_height,
               0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
  for (const auto view : rendering::kGameplayOrthographicOutputViews) {
    bgfx::setViewTransform(view, nullptr, ortho);
  }
  bgfx::setViewTransform(rendering::bga_view, nullptr, ortho);
  bgfx::setViewTransform(rendering::bga_layer_view, nullptr, ortho);
  rendering::game_camera.render(true);
}

void configureReplayExportRenderViews(int width, int height,
                                      bgfx::FrameBufferHandle outputFrameBuffer,
                                      rendering::BlurPass &bgaBlurPass,
                                      const AppSettings &settings) {
  const auto exportWidth = static_cast<uint16_t>(width);
  const auto exportHeight = static_cast<uint16_t>(height);

  bgaBlurPass.setInputViews(
      std::vector<bgfx::ViewId>(rendering::kGameplayBgaInputViews.begin(),
                                rendering::kGameplayBgaInputViews.end()));

  for (const auto view : rendering::kGameplayOutputViews) {
    bgfx::setViewFrameBuffer(view, outputFrameBuffer);
  }
  bgfx::setViewFrameBuffer(rendering::readback_view, BGFX_INVALID_HANDLE);
  resetViewTransform(bgaBlurPass.sceneWidth(), bgaBlurPass.sceneHeight(),
                     rendering::blur_view_h, rendering::blur_view_v,
                     rendering::final_view, settings);
  bgfx::setViewRect(rendering::readback_view, 0, 0, exportWidth, exportHeight);

  rendering::applyViewOrder(rendering::blur_view_h, rendering::blur_view_v,
                            rendering::final_view);
}

class ScopedReplayVideoBgfxAccess {
public:
  explicit ScopedReplayVideoBgfxAccess(ApplicationContext &context)
      : context(context), access(context.rendererAccess.acquireExport()) {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
    originalResetFlags = context.bgfxResetFlags.load(std::memory_order_relaxed);
    if ((originalResetFlags & BGFX_RESET_VSYNC) != 0) {
      bgfx::reset(rendering::render_width, rendering::render_height,
                  originalResetFlags & ~BGFX_RESET_VSYNC);
      restoreResetFlags = true;
    }
#endif
  }

  ~ScopedReplayVideoBgfxAccess() { release(); }

  void release() {
    if (released) {
      return;
    }
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
    if (restoreResetFlags) {
      bgfx::reset(rendering::render_width, rendering::render_height,
                  originalResetFlags);
      restorePrimaryRenderViews(&context);
    }
#endif
    context.replayVideoExportUiFrameRequested.store(false,
                                                    std::memory_order_release);
    access.release();
    released = true;
  }

  void allowUiFrame(const std::function<void()> &restoreExportViews) {
    if (released || context.quitFlag.load(std::memory_order_acquire)) {
      return;
    }

    restorePrimaryRenderViews(&context);
    const auto previousFrame =
        context.replayVideoExportUiFrameSerial.load(std::memory_order_acquire);
    context.replayVideoExportUiFrameRequested.store(true,
                                                    std::memory_order_release);
    access.unlockForUiFrame();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(16);
    while (!context.quitFlag.load(std::memory_order_acquire) &&
           context.replayVideoExportUiFrameSerial.load(
               std::memory_order_acquire) == previousFrame &&
           std::chrono::steady_clock::now() < deadline) {
      SDL_Delay(1);
    }

    access.relockAfterUiFrame();
    context.replayVideoExportUiFrameRequested.store(false,
                                                    std::memory_order_release);
    restoreExportViews();
  }

  ScopedReplayVideoBgfxAccess(const ScopedReplayVideoBgfxAccess &) = delete;
  ScopedReplayVideoBgfxAccess &
  operator=(const ScopedReplayVideoBgfxAccess &) = delete;

private:
  ApplicationContext &context;
  display::RendererAccessCoordinator::ExportReservation access;
  uint32_t originalResetFlags = 0;
  bool restoreResetFlags = false;
  bool released = false;
};

std::string makeTimestamp() {
  std::time_t rawTime = std::time(nullptr);
  std::tm timeInfo{};
#ifdef _WIN32
  localtime_s(&timeInfo, &rawTime);
#else
  localtime_r(&rawTime, &timeInfo);
#endif
  std::ostringstream stream;
  stream << std::put_time(&timeInfo, "%Y%m%d_%H%M%S");
  return stream.str();
}

std::string sanitizeFileNamePart(const std::string &value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char ch : value) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
      result.push_back(static_cast<char>(ch));
    } else if (ch == ' ' || ch == '.' || ch == '[' || ch == ']') {
      result.push_back('_');
    }
  }

  while (!result.empty() && result.back() == '_') {
    result.pop_back();
  }
  if (result.empty()) {
    return "replay";
  }
  return result.substr(0, 80);
}

std::string replayExportPlayOptionLabel(const ReplayData &replay) {
  const std::string label = play_options::formatPlayOptionLabel(
      replay.playOption, replay.playOptionSeed, replay.playOption2,
      replay.playOption2Seed);
  return label.empty() ? "" : "Option: " + label;
}

// LaneRenderer advances both BPM and #SCROLL from its timeline cursor before
// computing the live green number and note travel.  Export must carry the
// same pair into the shared presentation instead of treating BPM changes as
// the only visual-rate state.
std::vector<const bms_parser::TimeLine *>
collectPlaybackRateChangeTimelines(const bms_parser::Chart &chart) {
  std::vector<const bms_parser::TimeLine *> timelines;
  for (const auto &measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr ||
          (!timeline->BpmChange && !timeline->ScrollChange) ||
          !std::isfinite(timeline->Bpm) || timeline->Bpm <= 0.0 ||
          !std::isfinite(timeline->Scroll)) {
        continue;
      }
      timelines.push_back(timeline);
    }
  }
  std::sort(timelines.begin(), timelines.end(),
            [](const bms_parser::TimeLine *lhs,
               const bms_parser::TimeLine *rhs) {
              return lhs->Timing < rhs->Timing;
            });
  return timelines;
}

struct ReplayAudioTrackResult {
  bool success = false;
  std::filesystem::path outputPath;
  std::string message;
  long long durationMicros = 0;
};

ReplayAudioTrackResult
writeReplayAudioTrack(bms_parser::Chart &chart, const ReplayData &replay,
                      const preparation::Plan &preparationPlan,
                      long long audioOffsetMicros,
                      const std::filesystem::path &path,
                      ReplayVideoExportLog *log) {
  std::atomic_bool isCancelled = false;
  const chart_audio::RenderOptions options{
      .keySoundMode = chart_audio::KeySoundMode::ReplayTiming,
      .replay = &replay,
      .playback = preparationPlan.playback,
      .clubMode = replay.provenance.clubMode,
      .keySoundOffsetMicros = audioOffsetMicros,
      .timelineStartMicros = preparationPlan.playbackStartTimeMicros,
      .prepMetronomePlan = preparationPlan.metronome.enabled
                               ? &preparationPlan.metronome
                               : nullptr,
      .isCancelled = &isCancelled,
      .log = [log](const std::string &message) {
        if (log != nullptr) {
          log->write("Replay export " + message);
        }
      },
  };
  const auto result = chart_audio::RenderChartAudioToWav(chart, path, options);
  return {.success = result.success,
          .outputPath = result.outputPath,
          .message = result.message,
          .durationMicros = result.durationMicros};
}

sf_count_t replayAudioFramesForMicros(long long durationMicros) {
  if (durationMicros <= 0) {
    return 0;
  }
  return static_cast<sf_count_t>(
      std::ceil(static_cast<long double>(durationMicros) *
                static_cast<long double>(kExportSampleRate) / 1000000.0L));
}

long long replayAudioMicrosForFrames(sf_count_t frames) {
  if (frames <= 0) {
    return 0;
  }
  return static_cast<long long>(
      std::llround(static_cast<long double>(frames) * 1000000.0L /
                   static_cast<long double>(kExportSampleRate)));
}

bool writeSilentAudioFrames(SNDFILE *output, sf_count_t frames) {
  if (output == nullptr || frames <= 0) {
    return output != nullptr;
  }

  constexpr sf_count_t kChunkFrames = 4096;
  std::vector<short> silence(
      static_cast<size_t>(kChunkFrames) * kExportChannels, 0);
  sf_count_t remaining = frames;
  while (remaining > 0) {
    const sf_count_t chunkFrames = std::min(remaining, kChunkFrames);
    if (sf_writef_short(output, silence.data(), chunkFrames) != chunkFrames) {
      return false;
    }
    remaining -= chunkFrames;
  }
  return true;
}

struct CourseReplayAudioSegment {
  std::filesystem::path wavPath;
  long long durationMicros = 0;
  long long contentDurationMicros = 0;
};

bool appendReplayAudioFile(SNDFILE *output, const std::filesystem::path &path,
                           sf_count_t maxFrames, sf_count_t &writtenFrames,
                           std::string &errorMessage) {
  if (output == nullptr) {
    errorMessage = "Course replay audio output is invalid";
    return false;
  }

  SF_INFO inputInfo{};
  auto inputHandle =
      asobmashow::audio::openSoundFileHandle(path, SFM_READ, inputInfo);
  if (inputHandle == nullptr) {
    errorMessage = std::string("Failed to open course replay stage audio: ") +
                   sf_strerror(nullptr);
    return false;
  }
  if (inputInfo.channels != kExportChannels ||
      inputInfo.samplerate != kExportSampleRate) {
    errorMessage = "Course replay stage audio format is invalid";
    return false;
  }

  constexpr sf_count_t kChunkFrames = 4096;
  std::vector<short> buffer(
      static_cast<size_t>(kChunkFrames) * kExportChannels);
  sf_count_t copiedFrames = 0;
  while (copiedFrames < maxFrames) {
    const sf_count_t requestedFrames =
        std::min(kChunkFrames, maxFrames - copiedFrames);
    const sf_count_t framesRead =
        sf_readf_short(inputHandle.get(), buffer.data(), requestedFrames);
    if (framesRead < 0) {
      errorMessage = std::string("Failed to read course replay stage audio: ") +
                     sf_strerror(inputHandle.get());
      return false;
    }
    if (framesRead == 0) {
      break;
    }
    if (sf_writef_short(output, buffer.data(), framesRead) != framesRead) {
      errorMessage = std::string("Failed to write course replay audio: ") +
                     sf_strerror(output);
      return false;
    }
    writtenFrames += framesRead;
    copiedFrames += framesRead;
  }
  return true;
}

bool writeReplayAudioFileAtDuration(const std::filesystem::path &inputPath,
                                    const std::filesystem::path &outputPath,
                                    long long durationMicros,
                                    long long contentDurationMicros,
                                    std::string &errorMessage) {
  const sf_count_t targetFrames = replayAudioFramesForMicros(durationMicros);
  const sf_count_t contentFrames = replayAudioFramesForMicros(
      std::min(durationMicros, contentDurationMicros));
  SF_INFO outputInfo{};
  outputInfo.channels = kExportChannels;
  outputInfo.samplerate = kExportSampleRate;
  outputInfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  auto outputHandle =
      asobmashow::audio::openSoundFileHandle(outputPath, SFM_WRITE, outputInfo);
  if (outputHandle == nullptr) {
    errorMessage = std::string("Failed to create aligned replay audio: ") +
                   sf_strerror(nullptr);
    return false;
  }

  sf_count_t writtenFrames = 0;
  if (contentFrames > 0 &&
      !appendReplayAudioFile(outputHandle.get(), inputPath, contentFrames,
                             writtenFrames, errorMessage)) {
    return false;
  }
  if (writtenFrames < targetFrames &&
      !writeSilentAudioFrames(outputHandle.get(), targetFrames - writtenFrames)) {
    errorMessage = std::string("Failed to pad aligned replay audio: ") +
                   sf_strerror(outputHandle.get());
    return false;
  }
  return true;
}

ReplayAudioTrackResult writeCourseReplayAudioTrack(
    const std::vector<CourseReplayAudioSegment> &segments,
    const std::filesystem::path &path, ReplayVideoExportLog *log) {
  if (segments.empty()) {
    return {.success = false, .message = "No course replay audio"};
  }

  SF_INFO outputInfo{};
  outputInfo.channels = kExportChannels;
  outputInfo.samplerate = kExportSampleRate;
  outputInfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  auto outputHandle =
      asobmashow::audio::openSoundFileHandle(path, SFM_WRITE, outputInfo);
  if (outputHandle == nullptr) {
    return {.success = false,
            .outputPath = path,
            .message = std::string("Failed to create course replay audio: ") +
                       sf_strerror(nullptr)};
  }

  sf_count_t writtenFrames = 0;
  for (const auto &segment : segments) {
    const sf_count_t segmentFrames =
        replayAudioFramesForMicros(segment.durationMicros);
    const sf_count_t contentFrames = replayAudioFramesForMicros(
        std::min(segment.durationMicros, segment.contentDurationMicros));
    std::string errorMessage;
    const sf_count_t writtenFramesBeforeSegment = writtenFrames;
    if (!appendReplayAudioFile(outputHandle.get(), segment.wavPath,
                               contentFrames, writtenFrames, errorMessage)) {
      return {.success = false, .outputPath = path, .message = errorMessage};
    }
    const sf_count_t segmentWrittenFrames =
        writtenFrames - writtenFramesBeforeSegment;

    const sf_count_t silenceFrames =
        std::max<sf_count_t>(0, segmentFrames - segmentWrittenFrames);
    if (silenceFrames > 0) {
      if (!writeSilentAudioFrames(outputHandle.get(), silenceFrames)) {
        return {.success = false,
                .outputPath = path,
                .message = std::string("Failed to write course replay silence: ") +
                           sf_strerror(outputHandle.get())};
      }
      writtenFrames += silenceFrames;
    }
  }

  if (log != nullptr) {
    log->write("Replay export course audio duration: " +
               std::to_string(static_cast<double>(
                                  replayAudioMicrosForFrames(writtenFrames)) /
                              1000000.0) +
               "s");
  }
  return {.success = true,
          .outputPath = path,
          .message = "Course audio exported",
          .durationMicros = replayAudioMicrosForFrames(writtenFrames)};
}

void applyReplayEventToPacemakerState(RhythmState &state,
                                      const ReplayEvent &event) {
  if (!pacemaker::replayEventCountsAsPlayedNote(event)) {
    return;
  }

  const JudgeResult judgeResult(event.judgement, event.diffMicros);
  state.judgeCount[event.judgement]++;
  if (judgeResult.isComboBreak()) {
    state.combo = 0;
    state.comboBreak++;
  } else if (event.judgement != Kpoor) {
    state.combo++;
    state.maxCombo = std::max(state.maxCombo, state.combo);
  }
  state.recordFastSlow(judgeResult);
  state.combo = event.combo;
  state.maxCombo = std::max(state.maxCombo, event.combo);
  state.gaugeType = event.gaugeType;
  state.currentGauge = event.gauge;
  const int gaugeIndex = gaugeTypeIndex(event.gaugeType);
  if (gaugeIndex >= 0 &&
      gaugeIndex < static_cast<int>(state.gaugeValues.size())) {
    state.gaugeValues[gaugeIndex] = event.gauge;
  }
}

Color resultGaugeLineColor(float value) {
  if (value > 80.0f) {
    return ui_theme::withAlpha(ui_theme::cyan(), 210);
  }
  if (value > 30.0f) {
    return ui_theme::withAlpha(ui_theme::lime(), 210);
  }
  return ui_theme::withAlpha(ui_theme::coral(), 210);
}

void drawResultGaugeLineGraph(rendering::SimpleBatchRenderer &batch,
                              const RhythmState &resultState, float x, float y,
                              float w, float h) {
  batch.addRect(x, y, w, h, ui_theme::resultPanelSubtle().toABGR());

  const float padding = 8.0f;
  const float graphX = x + padding;
  const float graphY = y + padding;
  const float graphW = std::max(1.0f, w - padding * 2.0f);
  const float graphH = std::max(1.0f, h - padding * 2.0f);
  const float gaugeMaximum =
      gaugeMaximumValue(resultState.gaugeType, resultState.gaugeProfile);
  auto clampedValue = [gaugeMaximum](float value) {
    return std::clamp(value, 0.0f, gaugeMaximum);
  };
  auto valueY = [&](float value) {
    return graphY + graphH - (clampedValue(value) / gaugeMaximum) * graphH;
  };

  const uint32_t guideColor = ui_theme::hairlineSubtle().toABGR();
  batch.addLine(graphX, valueY(80.0f), graphX + graphW, valueY(80.0f), 1.0f,
                guideColor);
  batch.addLine(graphX, valueY(30.0f), graphX + graphW, valueY(30.0f), 1.0f,
                guideColor);

  const size_t count = resultState.gaugeHistory.size();
  if (count == 1) {
    const float value = clampedValue(resultState.gaugeHistory.front());
    batch.addCircle(graphX, valueY(value), 3.5f,
                    resultGaugeLineColor(value).toABGR());
    return;
  }

  for (size_t i = 1; i < count; ++i) {
    const float prevValue = clampedValue(resultState.gaugeHistory[i - 1]);
    const float value = clampedValue(resultState.gaugeHistory[i]);
    const float x0 =
        graphX + (static_cast<float>(i - 1) / static_cast<float>(count - 1)) *
                     graphW;
    const float x1 =
        graphX + (static_cast<float>(i) / static_cast<float>(count - 1)) *
                     graphW;
    batch.addLine(x0, valueY(prevValue), x1, valueY(value), 3.0f,
                  resultGaugeLineColor(value).toABGR());
  }

  const size_t markerStep = std::max<size_t>(1, count / 40);
  for (size_t i = 0; i < count; i += markerStep) {
    const float value = clampedValue(resultState.gaugeHistory[i]);
    const float pointX =
        graphX + (static_cast<float>(i) / static_cast<float>(count - 1)) *
                     graphW;
    batch.addCircle(pointX, valueY(value), 2.5f,
                    resultGaugeLineColor(value).toABGR());
  }
}

void drawReplayResultGaugeGraph(rendering::SimpleBatchRenderer &batch,
                                const RhythmState &resultState,
                                const View *graphPlaceHolder) {
  if (graphPlaceHolder == nullptr || resultState.gaugeHistory.empty()) {
    return;
  }

  const float x = graphPlaceHolder->getX();
  const float y = graphPlaceHolder->getY();
  const float w = graphPlaceHolder->getWidth();
  const float h = graphPlaceHolder->getHeight();
  if (w <= 0.0f || h <= 0.0f) {
    return;
  }

  batch.setSubmitView(rendering::ui_view);
  batch.setSubmitDepth(0);
  batch.begin();
  drawResultGaugeLineGraph(batch, resultState, x, y, w, h);
  batch.end();
}

struct CourseReplayVideoStage {
  std::unique_ptr<bms_parser::Chart> chart;
  ReplayData replay;
  preparation::Plan preparationPlan;
  GaugeStateSnapshot initialGaugeState;
  RhythmState resultState;
  std::optional<PreparedReplayGameplayPresentation> gameplayPresentation;
  std::optional<skin::SkinGameplayTiming> selectedSkinTiming;
  std::optional<skin::RuntimeSkinConfigurationSelection> runtimeSkinSelection;
  std::optional<long long> failureMicros;
  long long gameplayDurationMicros = 0;
  long long resultDurationMicros = 0;
  long long audioDurationMicros = 0;

  CourseReplayVideoStage(std::unique_ptr<bms_parser::Chart> chart,
                         ReplayData replay,
                         preparation::Plan preparationPlan,
                         GaugeStateSnapshot initialGaugeState,
                         RhythmState resultState,
                         std::optional<long long> failureMicros,
                         long long gameplayDurationMicros,
                         long long resultDurationMicros,
                         long long audioDurationMicros)
      : chart(std::move(chart)), replay(std::move(replay)),
        preparationPlan(std::move(preparationPlan)),
        initialGaugeState(std::move(initialGaugeState)),
        resultState(std::move(resultState)), failureMicros(failureMicros),
        gameplayDurationMicros(gameplayDurationMicros),
        resultDurationMicros(resultDurationMicros),
        audioDurationMicros(audioDurationMicros) {}
};

[[nodiscard]] std::optional<ReplayVideoExportResult>
preflightCourseReplayGameplayPresentations(
    ApplicationContext &context, std::vector<CourseReplayVideoStage> &stages,
    const AppSettings &settings, const ReplayVideoExportOptions &options,
    ReplayVideoExportLog *log) {
  const auto resolvedOptions = resolveReplayVideoExportOptions(options);
  std::vector<replay_video_export::CourseReplayGameplayPreflightStage>
      preflightStages;
  preflightStages.reserve(stages.size());
  for (auto &stage : stages) {
    if (stage.chart == nullptr) {
      return ReplayVideoExportResult{.success = false,
                                     .message = "No course replay stage"};
    }
    stage.gameplayPresentation.emplace();
    preflightStages.push_back(
        {.chart = *stage.chart,
         .replay = stage.replay,
         .preparationPlan = stage.preparationPlan,
         .configuration = replay_video_export::replayGameplayPresentationConfig(
             settings,
             settings.playAreaWidthForKeyMode(stage.chart->Meta.KeyMode),
             *stage.chart,
             resolvedOptions.renderTouchPoints,
             resolvedOptions.renderReplayGhosts),
         .exportWidth = resolvedOptions.width,
         .exportHeight = resolvedOptions.height,
         .skinServices = replayGameplaySkinSessionServices(context),
         .presentation = stage.gameplayPresentation->presentation,
         .selectedSkinTiming = stage.selectedSkinTiming,
         .runtimeSelection = stage.runtimeSkinSelection});
  }
  const auto result =
      replay_video_export::preflightCourseReplayGameplayPresentations(
          preflightStages, context.jukebox, settings, context.rendererAccess);
  if (result) {
    replayExportLog(log, "Replay export skin preflight failed: %s",
                    result->message.c_str());
  }
  return result;
}

long long courseResultDurationMicrosForReplayVideo(
    const std::vector<CourseReplayVideoStage> &stages, bool includeResultScreen) {
  long long finalAudioTailMicros = 0;
  if (!stages.empty()) {
    const CourseReplayVideoStage &finalStage = stages.back();
    finalAudioTailMicros =
        std::max(0LL, finalStage.audioDurationMicros -
                          finalStage.gameplayDurationMicros -
                          finalStage.resultDurationMicros);
  }
  return includeResultScreen
             ? std::max(kResultSceneTailMicros, finalAudioTailMicros)
             : finalAudioTailMicros;
}

bms_parser::ChartMeta courseResultMetaForReplayVideo(
    const CourseReplayData &replay,
    const std::vector<CourseReplayVideoStage> &stages) {
  int totalNotes = 0;
  long long playLength = 0;
  for (const auto &stage : stages) {
    if (stage.chart == nullptr) {
      continue;
    }
    totalNotes += std::max(0, stage.chart->Meta.TotalNotes);
    playLength += std::max(0LL, stage.chart->Meta.PlayLength);
  }
  return result_presentation::courseResultMeta(
      replay.courseName, replay.courseGroupName, stages.size(), totalNotes,
      playLength);
}

RhythmState courseResultStateForReplayVideo(
    const CourseReplayData &replay,
    const std::vector<CourseReplayVideoStage> &stages) {
  RhythmState aggregate(nullptr, false);
  aggregate.configureGauge(replay.initialGaugeType, replay.gaugeAutoShift,
                           replay.gaugeProfile,
                           replay.gaugeAutoShiftLowerBound);
  aggregate.resetJudgeCounts();
  aggregate.comboBreak = 0;
  aggregate.maxCombo = 0;
  aggregate.fastCount = 0;
  aggregate.slowCount = 0;
  aggregate.gaugeHistory.clear();

  for (const auto &stage : stages) {
    const RhythmState &state = stage.resultState;
    for (int i = 0; i < JudgementCount; ++i) {
      aggregate.addJudgeCountFrom(state, static_cast<Judgement>(i));
    }
    aggregate.comboBreak += state.comboBreak;
    aggregate.fastCount += state.fastCount;
    aggregate.slowCount += state.slowCount;
    aggregate.maxCombo = std::max(aggregate.maxCombo, state.maxCombo);
    aggregate.gaugeHistory.insert(aggregate.gaugeHistory.end(),
                                  state.gaugeHistory.begin(),
                                  state.gaugeHistory.end());
    aggregate.combo = state.combo;
    aggregate.currentGauge = state.currentGauge;
    aggregate.gaugeType = state.gaugeType;
    aggregate.gaugeValues = state.gaugeValues;
    aggregate.gaugeSurvivalFailed = state.gaugeSurvivalFailed;
  }

  aggregate.currentGauge = replay.finalGauge;
  aggregate.gaugeType = stages.empty() ? replay.initialGaugeType
                                       : stages.back().resultState.gaugeType;
  const int gaugeIndex = gaugeTypeIndex(aggregate.gaugeType);
  if (gaugeIndex >= 0 &&
      gaugeIndex < static_cast<int>(aggregate.gaugeValues.size())) {
    aggregate.gaugeValues[gaugeIndex] = aggregate.currentGauge;
  }
  return aggregate;
}

std::string ffmpegError(int errorCode) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(errorCode, buffer.data(), buffer.size()) < 0) {
    return "Unknown FFmpeg error";
  }
  return buffer.data();
}

const AVCodec *findReplayVideoEncoder() {
#if TARGET_OS_ANDROID
  if (const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
      codec != nullptr) {
    return codec;
  }
  if (const AVCodec *codec = avcodec_find_encoder_by_name("mpeg4");
      codec != nullptr) {
    return codec;
  }
  return avcodec_find_encoder(AV_CODEC_ID_H264);
#else
#if __APPLE__
  if (const AVCodec *codec = avcodec_find_encoder_by_name("h264_videotoolbox");
      codec != nullptr) {
    return codec;
  }
  if (const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
      codec != nullptr) {
    return codec;
  }
#else
  if (const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
      codec != nullptr) {
    return codec;
  }
  if (const AVCodec *codec = avcodec_find_encoder_by_name("h264_videotoolbox");
      codec != nullptr) {
    return codec;
  }
#endif
  return avcodec_find_encoder(AV_CODEC_ID_H264);
#endif
}

bool codecSupportsPixelFormat(const AVCodec *codec, AVPixelFormat format) {
  const void *configs = nullptr;
  int configCount = 0;
  const int ret = avcodec_get_supported_config(
      nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &configCount);
  if (ret < 0 || configs == nullptr) {
    return true;
  }
  const auto *formats = static_cast<const AVPixelFormat *>(configs);
  for (int i = 0; i < configCount; ++i) {
    if (formats[i] == format) {
      return true;
    }
  }
  return false;
}

std::optional<AVPixelFormat> chooseVideoPixelFormat(const AVCodec *codec) {
  const std::string codecName =
      codec != nullptr && codec->name != nullptr ? codec->name : "";
  const std::array<AVPixelFormat, 3> hardwarePreferredFormats = {
      AV_PIX_FMT_BGRA, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P};
  const std::array<AVPixelFormat, 3> softwarePreferredFormats = {
      AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_BGRA};
  const auto &preferredFormats =
      codecName.find("videotoolbox") != std::string::npos
          ? hardwarePreferredFormats
          : softwarePreferredFormats;
  for (const AVPixelFormat format : preferredFormats) {
    if (codecSupportsPixelFormat(codec, format)) {
      return format;
    }
  }
  return std::nullopt;
}

bool codecSupportsSampleFormat(const AVCodec *codec, AVSampleFormat format) {
  const void *configs = nullptr;
  int configCount = 0;
  const int ret = avcodec_get_supported_config(
      nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &configCount);
  if (ret < 0 || configs == nullptr) {
    return true;
  }
  const auto *formats = static_cast<const AVSampleFormat *>(configs);
  for (int i = 0; i < configCount; ++i) {
    if (formats[i] == format) {
      return true;
    }
  }
  return false;
}

std::optional<AVSampleFormat> chooseAudioSampleFormat(const AVCodec *codec) {
  const std::array<AVSampleFormat, 4> preferredFormats = {
      AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16P,
      AV_SAMPLE_FMT_S16};
  for (const AVSampleFormat format : preferredFormats) {
    if (codecSupportsSampleFormat(codec, format)) {
      return format;
    }
  }
  return std::nullopt;
}

struct ReplayFfmpegEncodeProfile {
  long long sendMicros = 0;
  long long receiveMicros = 0;
  long long writeMicros = 0;
  long long stallMicros = 0;
  long long packetCount = 0;
  int stalledRetries = 0;
};

bool encodeFrame(AVCodecContext *encoderContext, AVFormatContext *formatContext,
                 AVStream *stream, AVFrame *frame, AVPacket *packet,
                 std::string &errorMessage, int64_t forcedPacketDuration = 0,
                 ReplayFfmpegEncodeProfile *profile = nullptr) {
  auto receiveAvailablePackets = [&](bool *wrotePacket = nullptr) {
    if (wrotePacket != nullptr) {
      *wrotePacket = false;
    }
    av_packet_unref(packet);
    const auto receiveStart = std::chrono::steady_clock::now();
    int ret = avcodec_receive_packet(encoderContext, packet);
    if (profile != nullptr) {
      profile->receiveMicros += elapsedMicros(receiveStart);
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return true;
    }

    while (ret >= 0) {
      if (forcedPacketDuration > 0) {
        packet->duration = forcedPacketDuration;
      }
      if (packet->pts != AV_NOPTS_VALUE && packet->dts == AV_NOPTS_VALUE) {
        packet->dts = packet->pts;
      }
      av_packet_rescale_ts(packet, encoderContext->time_base, stream->time_base);
      packet->stream_index = stream->index;
      const auto writeStart = std::chrono::steady_clock::now();
      ret = av_interleaved_write_frame(formatContext, packet);
      if (profile != nullptr) {
        profile->writeMicros += elapsedMicros(writeStart);
      }
      av_packet_unref(packet);
      if (ret < 0) {
        errorMessage = "Failed to write encoded packet: " + ffmpegError(ret);
        return false;
      }
      if (profile != nullptr) {
        profile->packetCount++;
      }
      if (wrotePacket != nullptr) {
        *wrotePacket = true;
      }

      const auto nextReceiveStart = std::chrono::steady_clock::now();
      ret = avcodec_receive_packet(encoderContext, packet);
      if (profile != nullptr) {
        profile->receiveMicros += elapsedMicros(nextReceiveStart);
      }
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return true;
      }
    }

    errorMessage = "Failed to receive encoded packet: " + ffmpegError(ret);
    return false;
  };

  int stalledRetries = 0;
  while (true) {
    const auto sendStart = std::chrono::steady_clock::now();
    const int ret = avcodec_send_frame(encoderContext, frame);
    if (profile != nullptr) {
      profile->sendMicros += elapsedMicros(sendStart);
    }
    if (ret == AVERROR(EAGAIN)) {
      bool wrotePacket = false;
      if (!receiveAvailablePackets(&wrotePacket)) {
        return false;
      }
      if (wrotePacket) {
        stalledRetries = 0;
      } else if (++stalledRetries > 10000) {
        errorMessage = "Timed out waiting for encoder input space";
        return false;
      } else {
        const auto stallStart = std::chrono::steady_clock::now();
        SDL_Delay(1);
        if (profile != nullptr) {
          profile->stallMicros += elapsedMicros(stallStart);
          profile->stalledRetries++;
        }
      }
      continue;
    }
    if (ret < 0) {
      errorMessage = "Failed to send frame to encoder: " + ffmpegError(ret);
      return false;
    }
    break;
  }

  return receiveAvailablePackets();
}

bool fillAudioFrame(AVFrame *frame, const std::vector<float> &interleaved,
                    int framesRead, std::string &errorMessage) {
  int ret = av_frame_make_writable(frame);
  if (ret < 0) {
    errorMessage = "Failed to prepare audio frame: " + ffmpegError(ret);
    return false;
  }

  const int frameCount = frame->nb_samples;
  switch (static_cast<AVSampleFormat>(frame->format)) {
  case AV_SAMPLE_FMT_FLTP: {
    auto *left = reinterpret_cast<float *>(frame->data[0]);
    auto *right = reinterpret_cast<float *>(frame->data[1]);
    for (int i = 0; i < frameCount; ++i) {
      const bool hasSample = i < framesRead;
      left[i] = hasSample ? interleaved[i * kExportChannels] : 0.0f;
      right[i] = hasSample ? interleaved[i * kExportChannels + 1] : 0.0f;
    }
    return true;
  }
  case AV_SAMPLE_FMT_FLT: {
    auto *samples = reinterpret_cast<float *>(frame->data[0]);
    for (int i = 0; i < frameCount * kExportChannels; ++i) {
      samples[i] = i < framesRead * kExportChannels ? interleaved[i] : 0.0f;
    }
    return true;
  }
  case AV_SAMPLE_FMT_S16P: {
    auto *left = reinterpret_cast<int16_t *>(frame->data[0]);
    auto *right = reinterpret_cast<int16_t *>(frame->data[1]);
    for (int i = 0; i < frameCount; ++i) {
      const float leftSample =
          i < framesRead ? interleaved[i * kExportChannels] : 0.0f;
      const float rightSample =
          i < framesRead ? interleaved[i * kExportChannels + 1] : 0.0f;
      left[i] = static_cast<int16_t>(
          std::lrint(std::clamp(leftSample, -1.0f, 1.0f) * 32767.0f));
      right[i] = static_cast<int16_t>(
          std::lrint(std::clamp(rightSample, -1.0f, 1.0f) * 32767.0f));
    }
    return true;
  }
  case AV_SAMPLE_FMT_S16: {
    auto *samples = reinterpret_cast<int16_t *>(frame->data[0]);
    for (int i = 0; i < frameCount * kExportChannels; ++i) {
      const float sample =
          i < framesRead * kExportChannels ? interleaved[i] : 0.0f;
      samples[i] = static_cast<int16_t>(
          std::lrint(std::clamp(sample, -1.0f, 1.0f) * 32767.0f));
    }
    return true;
  }
  default:
    if (const char *formatName =
            av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format));
        formatName != nullptr) {
      errorMessage =
          std::string("Unsupported AAC sample format: ") + formatName;
    } else {
      errorMessage = "Unsupported AAC sample format";
    }
    return false;
  }
}

bool encodeNextAudioFrame(SNDFILE *audioFile, AVCodecContext *audioContext,
                          AVFormatContext *formatContext, AVStream *audioStream,
                          AVFrame *audioFrame, AVPacket *packet,
                          std::vector<float> &audioBuffer,
                          int64_t &nextAudioPts, bool &audioFinished,
                          std::string &errorMessage) {
  if (audioFinished) {
    return true;
  }

  const int frameSize = audioFrame->nb_samples;
  std::fill(audioBuffer.begin(), audioBuffer.end(), 0.0f);
  const sf_count_t framesRead =
      sf_readf_float(audioFile, audioBuffer.data(), frameSize);
  if (framesRead <= 0) {
    audioFinished = true;
    return true;
  }

  if (!fillAudioFrame(audioFrame, audioBuffer, static_cast<int>(framesRead),
                      errorMessage)) {
    return false;
  }
  audioFrame->pts = nextAudioPts;
  nextAudioPts += frameSize;
  if (framesRead < frameSize) {
    audioFinished = true;
  }

  return encodeFrame(audioContext, formatContext, audioStream, audioFrame,
                     packet, errorMessage);
}

#if __APPLE__
std::string vImageReplayError(vImage_Error error) {
  switch (error) {
  case kvImageNoError:
    return "no error";
  case kvImageUnknownFlagsBit:
    return "unknown flags";
  case kvImageRoiLargerThanInputBuffer:
    return "ROI larger than input buffer";
  case kvImageUnsupportedConversion:
    return "unsupported conversion";
  default:
    return "error " + std::to_string(static_cast<long long>(error));
  }
}

bool initBgraToNv12Converter(vImage_ARGBToYpCbCr &converter,
                             std::string &errorMessage) {
  const vImage_YpCbCrPixelRange pixelRange{
      16, 128, 235, 240, 255, 0, 255, 1};
  const vImage_Error error = vImageConvert_ARGBToYpCbCr_GenerateConversion(
      kvImage_ARGBToYpCbCrMatrix_ITU_R_601_4, &pixelRange, &converter,
      kvImageARGB8888, kvImage420Yp8_CbCr8, kvImageNoFlags);
  if (error != kvImageNoError) {
    errorMessage =
        "Failed to initialize vImage BGRA to NV12 converter: " +
        vImageReplayError(error);
    return false;
  }
  return true;
}
#endif

class ReplayMp4StreamWriter {
public:
  ~ReplayMp4StreamWriter() { cleanup(); }
  ReplayMp4StreamWriter() = default;
  ReplayMp4StreamWriter(const ReplayMp4StreamWriter &) = delete;
  ReplayMp4StreamWriter &operator=(const ReplayMp4StreamWriter &) = delete;
  ReplayMp4StreamWriter(ReplayMp4StreamWriter &&) = delete;
  ReplayMp4StreamWriter &operator=(ReplayMp4StreamWriter &&) = delete;

  bool open(const std::filesystem::path &wavPath,
            const std::filesystem::path &outputPath, int width, int height,
            int fps, ReplayVideoExportLog *log, std::string &errorMessage) {
    this->outputPath = outputPath;
    this->width = width;
    this->height = height;
    this->fps = fps;

    auto failOpen = [&](const std::string &message) {
      cleanup();
      errorMessage = message;
      return false;
    };

    const std::string outputPathString = fspath_to_utf8(outputPath);
    int ret = avformat_alloc_output_context2(&formatContext, nullptr, "mp4",
                                             outputPathString.c_str());
    if (ret < 0 || formatContext == nullptr) {
      return failOpen("Failed to create MP4 muxer: " + ffmpegError(ret));
    }

    const AVCodec *videoCodec = findReplayVideoEncoder();
    if (videoCodec == nullptr) {
      return failOpen("Replay video encoder was not found");
    }
    const std::string videoCodecName =
        videoCodec->name != nullptr ? videoCodec->name : "";
    const bool videoCodecIsH264 = videoCodec->id == AV_CODEC_ID_H264;
    const auto videoPixelFormat = chooseVideoPixelFormat(videoCodec);
    if (!videoPixelFormat.has_value()) {
      return failOpen("Replay video encoder does not support a BGRA-convertible "
                      "format");
    }

    videoStream = avformat_new_stream(formatContext, nullptr);
    if (videoStream == nullptr) {
      return failOpen("Failed to create MP4 video stream");
    }
    videoContext = avcodec_alloc_context3(videoCodec);
    if (videoContext == nullptr) {
      return failOpen("Failed to allocate replay video encoder");
    }
    videoContext->codec_id = videoCodec->id;
    videoContext->codec_type = AVMEDIA_TYPE_VIDEO;
    videoContext->width = width;
    videoContext->height = height;
    videoContext->pix_fmt = videoPixelFormat.value();
    videoContext->time_base = AVRational{1, fps};
    videoContext->framerate = AVRational{fps, 1};
    videoContext->sample_aspect_ratio = AVRational{1, 1};
    videoContext->gop_size = fps * 2;
    videoContext->max_b_frames = 0;
    videoContext->bit_rate = replayVideoBitRate(width, height, fps);
    const int h264Level =
        videoCodecIsH264 ? replayVideoH264Level(width, height, fps) : 0;
    if (videoCodecIsH264) {
      videoContext->profile = kH264HighProfile;
      videoContext->level = h264Level;
    }
    const bool videoCodecSupportsFrameThreads =
        replayVideoEncoderSupportsFrameThreads(videoCodec);
    const bool videoCodecSupportsOtherThreads =
        replayVideoEncoderSupportsOtherThreads(videoCodec);
    if (videoCodecSupportsFrameThreads || videoCodecSupportsOtherThreads) {
      videoContext->thread_count = replayVideoEncoderThreadCount();
      if (videoCodecSupportsFrameThreads) {
        videoContext->thread_type = FF_THREAD_FRAME;
      }
    } else {
      videoContext->thread_count = 1;
    }
    if (formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
      videoContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVDictionary *videoOptions = nullptr;
    if (videoCodecName == "libx264") {
      av_dict_set(&videoOptions, "preset", "ultrafast", 0);
      av_dict_set(&videoOptions, "crf", "22", 0);
      av_dict_set(&videoOptions, "profile", "high", 0);
      const std::string levelString = replayVideoH264LevelString(h264Level);
      av_dict_set(&videoOptions, "level", levelString.c_str(), 0);
      const std::string threadCount =
          std::to_string(videoContext->thread_count);
      av_dict_set(&videoOptions, "threads", threadCount.c_str(), 0);
      replayExportLog(log,
                      "Replay video export libx264 speed hints: preset=ultrafast, "
                      "crf=22, profile=high, threads=%s",
                      threadCount.c_str());
    } else if (videoCodecName.find("videotoolbox") != std::string::npos) {
      av_dict_set(&videoOptions, "realtime", "1", 0);
      av_dict_set(&videoOptions, "prio_speed", "1", 0);
      av_dict_set(&videoOptions, "power_efficient", "0", 0);
      av_dict_set(&videoOptions, "spatial_aq", "0", 0);
      av_dict_set(&videoOptions, "max_ref_frames", "1", 0);
      av_dict_set(&videoOptions, "allow_sw", "1", 0);
      replayExportLog(log, "Replay video export VideoToolbox speed hints: "
                       "realtime=1, prio_speed=1, power_efficient=0, "
                       "spatial_aq=0, max_ref_frames=1, allow_sw=1");
    }
    ret = avcodec_open2(videoContext, videoCodec, &videoOptions);
    av_dict_free(&videoOptions);
    if (ret < 0) {
      return failOpen("Failed to open replay video encoder: " +
                      ffmpegError(ret));
    }
    videoFrameDuration = 1;
    const char *pixelFormatName = av_get_pix_fmt_name(videoContext->pix_fmt);
    if (videoCodecIsH264) {
      replayExportLog(log,
                      "Replay video export encoder: %s, pixel format: %s, time "
                      "base: %d/%d, frame duration: %lld, H.264 level: %s",
                      videoCodec->name,
                      pixelFormatName != nullptr ? pixelFormatName : "unknown",
                      videoContext->time_base.num, videoContext->time_base.den,
                      static_cast<long long>(videoFrameDuration),
                      replayVideoH264LevelString(h264Level).c_str());
    } else {
      replayExportLog(log,
                      "Replay video export encoder: %s, pixel format: %s, time "
                      "base: %d/%d, frame duration: %lld",
                      videoCodec->name,
                      pixelFormatName != nullptr ? pixelFormatName : "unknown",
                      videoContext->time_base.num, videoContext->time_base.den,
                      static_cast<long long>(videoFrameDuration));
    }
    ret = avcodec_parameters_from_context(videoStream->codecpar, videoContext);
    if (ret < 0) {
      return failOpen("Failed to configure MP4 video stream: " +
                      ffmpegError(ret));
    }
    videoStream->time_base = videoContext->time_base;
    videoStream->avg_frame_rate = videoContext->framerate;
    videoStream->r_frame_rate = videoContext->framerate;

    const AVCodec *audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (audioCodec == nullptr) {
      return failOpen("AAC encoder was not found");
    }
    const auto audioSampleFormat = chooseAudioSampleFormat(audioCodec);
    if (!audioSampleFormat.has_value()) {
      return failOpen("AAC encoder does not support replay export sample "
                      "formats");
    }

    audioStream = avformat_new_stream(formatContext, nullptr);
    if (audioStream == nullptr) {
      return failOpen("Failed to create MP4 audio stream");
    }
    audioContext = avcodec_alloc_context3(audioCodec);
    if (audioContext == nullptr) {
      return failOpen("Failed to allocate AAC encoder");
    }
    audioContext->codec_id = AV_CODEC_ID_AAC;
    audioContext->codec_type = AVMEDIA_TYPE_AUDIO;
    audioContext->sample_rate = kExportSampleRate;
    audioContext->sample_fmt = audioSampleFormat.value();
    audioContext->time_base = AVRational{1, kExportSampleRate};
    audioContext->bit_rate = 192000;
    AVChannelLayout stereoLayout = AV_CHANNEL_LAYOUT_STEREO;
    ret = av_channel_layout_copy(&audioContext->ch_layout, &stereoLayout);
    if (ret < 0) {
      return failOpen("Failed to configure AAC channel layout: " +
                      ffmpegError(ret));
    }
    if (formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
      audioContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    ret = avcodec_open2(audioContext, audioCodec, nullptr);
    if (ret < 0) {
      return failOpen("Failed to open AAC encoder: " + ffmpegError(ret));
    }
    ret = avcodec_parameters_from_context(audioStream->codecpar, audioContext);
    if (ret < 0) {
      return failOpen("Failed to configure MP4 audio stream: " +
                      ffmpegError(ret));
    }
    audioStream->time_base = audioContext->time_base;

    SF_INFO audioInfo{};
    audioFile = asobmashow::audio::openSoundFile(wavPath, SFM_READ, audioInfo);
    if (audioFile == nullptr) {
      return failOpen(std::string("Failed to open replay audio track: ") +
                      sf_strerror(nullptr));
    }
    if (audioInfo.channels != kExportChannels ||
        audioInfo.samplerate != kExportSampleRate) {
      return failOpen("Replay audio track format is invalid");
    }

    videoFrame = av_frame_alloc();
    if (videoFrame == nullptr) {
      return failOpen("Failed to allocate video frame");
    }
    videoFrame->format = videoContext->pix_fmt;
    videoFrame->width = width;
    videoFrame->height = height;
    ret = av_frame_get_buffer(videoFrame, 32);
    if (ret < 0) {
      return failOpen("Failed to allocate video frame buffer: " +
                      ffmpegError(ret));
    }

    const int audioFrameSize =
        audioContext->frame_size > 0 ? audioContext->frame_size : 1024;
    audioFrame = av_frame_alloc();
    if (audioFrame == nullptr) {
      return failOpen("Failed to allocate audio frame");
    }
    audioFrame->format = audioContext->sample_fmt;
    audioFrame->sample_rate = audioContext->sample_rate;
    audioFrame->nb_samples = audioFrameSize;
    ret = av_channel_layout_copy(&audioFrame->ch_layout,
                                 &audioContext->ch_layout);
    if (ret < 0) {
      return failOpen("Failed to configure audio frame layout: " +
                      ffmpegError(ret));
    }
    ret = av_frame_get_buffer(audioFrame, 0);
    if (ret < 0) {
      return failOpen("Failed to allocate audio frame buffer: " +
                      ffmpegError(ret));
    }
    replayExportLog(
        log,
        "Replay video export FFmpeg buffers: refcounted AVFrames via "
        "av_frame_get_buffer");

    pixelConvertThreadCount = replayVideoPixelConvertThreadCount();
#if __APPLE__
    useVImageBgraToNv12 =
        videoContext->pix_fmt == AV_PIX_FMT_NV12 && width % 2 == 0 &&
        height % 2 == 0;
    if (useVImageBgraToNv12 &&
        !initBgraToNv12Converter(vImageBgraToNv12Converter, errorMessage)) {
      return failOpen(errorMessage);
    }
#endif
    if (videoContext->pix_fmt != AV_PIX_FMT_BGRA
#if __APPLE__
        && !useVImageBgraToNv12
#endif
    ) {
      swsContext = sws_alloc_context();
      if (swsContext == nullptr) {
        return failOpen("Failed to create video pixel converter");
      }
      swsContext->flags = SWS_FAST_BILINEAR;
      swsContext->threads = pixelConvertThreadCount;
      swsContext->src_w = width;
      swsContext->src_h = height;
      swsContext->src_format = AV_PIX_FMT_BGRA;
      swsContext->dst_w = width;
      swsContext->dst_h = height;
      swsContext->dst_format = videoContext->pix_fmt;
      ret = sws_init_context(swsContext, nullptr, nullptr);
      if (ret < 0) {
        return failOpen("Failed to initialize video pixel converter: " +
                        ffmpegError(ret));
      }
    }
    if (videoContext->pix_fmt == AV_PIX_FMT_BGRA) {
      replayExportLog(log, "Replay video export pixel converter: bgra copy");
#if __APPLE__
    } else if (useVImageBgraToNv12) {
      replayExportLog(
          log,
          "Replay video export pixel converter: vImage bgra->nv12, matrix: "
          "ITU-R 601 video range");
#endif
    } else {
      replayExportLog(log,
                      "Replay video export pixel converter: swscale, threads: %d",
                      pixelConvertThreadCount);
    }

    videoPacket = av_packet_alloc();
    audioPacket = av_packet_alloc();
    if (videoPacket == nullptr || audioPacket == nullptr) {
      return failOpen("Failed to allocate encoder packet");
    }

    audioBuffer.assign(static_cast<size_t>(audioFrameSize) * kExportChannels,
                       0.0f);

    if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
      ret = avio_open(&formatContext->pb, outputPathString.c_str(),
                      AVIO_FLAG_WRITE);
      if (ret < 0) {
        return failOpen("Failed to open MP4 output: " + ffmpegError(ret));
      }
    }
    formatContext->flush_packets = 0;

    AVDictionary *formatOptions = nullptr;
    av_dict_set(&formatOptions, "movflags", "+faststart", 0);
    const std::string videoTrackTimescale = std::to_string(fps);
    av_dict_set(&formatOptions, "video_track_timescale",
                videoTrackTimescale.c_str(), 0);
    ret = avformat_write_header(formatContext, &formatOptions);
    av_dict_free(&formatOptions);
    if (ret < 0) {
      return failOpen("Failed to write MP4 header: " + ffmpegError(ret));
    }
    replayExportLog(log, "Replay video export MP4 stream time base: %d/%d",
                    videoStream->time_base.num, videoStream->time_base.den);
    replayExportLog(log, "Replay video export MP4 muxer: flush_packets=0, "
                         "movflags=+faststart");

    return true;
  }

  bool encodeVideoFrame(const uint8_t *bgraFrame, size_t frameIndex,
                        long long videoTimeMicros, std::string &errorMessage) {
    const auto audioEncodeStart = std::chrono::steady_clock::now();
    while (!audioFinished &&
           (nextAudioPts * 1000000LL) / kExportSampleRate <= videoTimeMicros) {
      if (!encodeNextAudioFrame(audioFile, audioContext, formatContext,
                                audioStream, audioFrame, audioPacket,
                                audioBuffer, nextAudioPts, audioFinished,
                                errorMessage)) {
        return false;
      }
    }
    audioEncodeMicrosTotal += elapsedMicros(audioEncodeStart);

    const auto framePrepareStart = std::chrono::steady_clock::now();
    std::array<AVBufferRef *, AV_NUM_DATA_POINTERS> bufferRefsBefore{};
    std::array<uint8_t *, AV_NUM_DATA_POINTERS> dataPointersBefore{};
    int maxFrameBufferRefCount = 0;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) {
      bufferRefsBefore[i] = videoFrame->buf[i];
      dataPointersBefore[i] = videoFrame->data[i];
      if (videoFrame->buf[i] != nullptr) {
        maxFrameBufferRefCount = std::max(
            maxFrameBufferRefCount,
            av_buffer_get_ref_count(videoFrame->buf[i]));
      }
    }
    maxVideoFrameBufferRefCount =
        std::max(maxVideoFrameBufferRefCount, maxFrameBufferRefCount);
    int ret = av_frame_make_writable(videoFrame);
    framePrepareMicrosTotal += elapsedMicros(framePrepareStart);
    if (ret < 0) {
      errorMessage = "Failed to prepare video frame: " + ffmpegError(ret);
      return false;
    }
    bool copiedFrameBuffer = false;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) {
      if ((bufferRefsBefore[i] != nullptr &&
           videoFrame->buf[i] != bufferRefsBefore[i]) ||
          (dataPointersBefore[i] != nullptr &&
           videoFrame->data[i] != dataPointersBefore[i])) {
        copiedFrameBuffer = true;
        break;
      }
    }
    if (copiedFrameBuffer) {
      ++videoFrameCowCopyCount;
    }

    const auto pixelConvertStart = std::chrono::steady_clock::now();
    if (videoContext->pix_fmt == AV_PIX_FMT_BGRA) {
      const int sourceLinesize = width * 4;
      for (int y = 0; y < height; ++y) {
        std::memcpy(videoFrame->data[0] + y * videoFrame->linesize[0],
                    bgraFrame + y * sourceLinesize,
                    static_cast<size_t>(sourceLinesize));
      }
#if __APPLE__
    } else if (useVImageBgraToNv12) {
      vImage_Buffer source{
          .data = const_cast<uint8_t *>(bgraFrame),
          .height = static_cast<vImagePixelCount>(height),
          .width = static_cast<vImagePixelCount>(width),
          .rowBytes = static_cast<size_t>(width) * 4ULL,
      };
      vImage_Buffer destinationY{
          .data = videoFrame->data[0],
          .height = static_cast<vImagePixelCount>(height),
          .width = static_cast<vImagePixelCount>(width),
          .rowBytes = static_cast<size_t>(videoFrame->linesize[0]),
      };
      vImage_Buffer destinationCbCr{
          .data = videoFrame->data[1],
          .height = static_cast<vImagePixelCount>(height / 2),
          .width = static_cast<vImagePixelCount>(width / 2),
          .rowBytes = static_cast<size_t>(videoFrame->linesize[1]),
      };
      const uint8_t bgraPermuteMap[4] = {3, 2, 1, 0};
      const vImage_Error error = vImageConvert_ARGB8888To420Yp8_CbCr8(
          &source, &destinationY, &destinationCbCr,
          &vImageBgraToNv12Converter, bgraPermuteMap, kvImageNoFlags);
      if (error != kvImageNoError) {
        errorMessage =
            "Failed to convert replay video frame with vImage: " +
            vImageReplayError(error);
        return false;
      }
#endif
    } else {
      const uint8_t *sourceData[4] = {bgraFrame, nullptr, nullptr, nullptr};
      const int sourceLinesize[4] = {width * 4, 0, 0, 0};
      sws_scale(swsContext, sourceData, sourceLinesize, 0, height,
                videoFrame->data, videoFrame->linesize);
    }
    pixelConvertMicrosTotal += elapsedMicros(pixelConvertStart);

    videoFrame->pts = static_cast<int64_t>(frameIndex);
    videoFrame->duration = videoFrameDuration;
    const auto videoEncodeStart = std::chrono::steady_clock::now();
    ReplayFfmpegEncodeProfile profile;
    const bool success =
        encodeFrame(videoContext, formatContext, videoStream, videoFrame,
                    videoPacket, errorMessage, videoFrameDuration, &profile);
    videoEncodeMicrosTotal += elapsedMicros(videoEncodeStart);
    videoSendMicrosTotal += profile.sendMicros;
    videoReceiveMicrosTotal += profile.receiveMicros;
    videoWriteMicrosTotal += profile.writeMicros;
    videoStallMicrosTotal += profile.stallMicros;
    videoPacketCount += profile.packetCount;
    videoStalledRetries += profile.stalledRetries;
    return success;
  }

  ReplayVideoExportResult finish() {
    std::string errorMessage;
    auto fail = [&](const std::string &message) {
      cleanup();
      return ReplayVideoExportResult{
          .success = false, .outputPath = outputPath, .message = message};
    };

    while (!audioFinished) {
      const auto audioEncodeStart = std::chrono::steady_clock::now();
      if (!encodeNextAudioFrame(audioFile, audioContext, formatContext,
                                audioStream, audioFrame, audioPacket,
                                audioBuffer, nextAudioPts, audioFinished,
                                errorMessage)) {
        audioEncodeMicrosTotal += elapsedMicros(audioEncodeStart);
        return fail(errorMessage);
      }
      audioEncodeMicrosTotal += elapsedMicros(audioEncodeStart);
    }
    const auto videoEncodeFlushStart = std::chrono::steady_clock::now();
    ReplayFfmpegEncodeProfile videoFlushProfile;
    if (!encodeFrame(videoContext, formatContext, videoStream, nullptr,
                     videoPacket, errorMessage, videoFrameDuration,
                     &videoFlushProfile)) {
      const long long videoFlushMicros = elapsedMicros(videoEncodeFlushStart);
      videoEncodeMicrosTotal += videoFlushMicros;
      videoFlushMicrosTotal += videoFlushMicros;
      videoSendMicrosTotal += videoFlushProfile.sendMicros;
      videoReceiveMicrosTotal += videoFlushProfile.receiveMicros;
      videoWriteMicrosTotal += videoFlushProfile.writeMicros;
      videoStallMicrosTotal += videoFlushProfile.stallMicros;
      videoPacketCount += videoFlushProfile.packetCount;
      videoStalledRetries += videoFlushProfile.stalledRetries;
      return fail(errorMessage);
    }
    const long long videoFlushMicros = elapsedMicros(videoEncodeFlushStart);
    videoEncodeMicrosTotal += videoFlushMicros;
    videoFlushMicrosTotal += videoFlushMicros;
    videoSendMicrosTotal += videoFlushProfile.sendMicros;
    videoReceiveMicrosTotal += videoFlushProfile.receiveMicros;
    videoWriteMicrosTotal += videoFlushProfile.writeMicros;
    videoStallMicrosTotal += videoFlushProfile.stallMicros;
    videoPacketCount += videoFlushProfile.packetCount;
    videoStalledRetries += videoFlushProfile.stalledRetries;
    const auto audioEncodeFlushStart = std::chrono::steady_clock::now();
    if (!encodeFrame(audioContext, formatContext, audioStream, nullptr,
                     audioPacket, errorMessage)) {
      audioEncodeMicrosTotal += elapsedMicros(audioEncodeFlushStart);
      return fail(errorMessage);
    }
    audioEncodeMicrosTotal += elapsedMicros(audioEncodeFlushStart);

    const int ret = av_write_trailer(formatContext);
    if (ret < 0) {
      return fail("Failed to write MP4 trailer: " + ffmpegError(ret));
    }

    cleanup();
    return {
        .success = true, .outputPath = outputPath, .message = "MP4 exported"};
  }

  long long audioEncodeMicros() const { return audioEncodeMicrosTotal; }
  long long framePrepareMicros() const { return framePrepareMicrosTotal; }
  long long pixelConvertMicros() const { return pixelConvertMicrosTotal; }
  long long videoEncodeMicros() const { return videoEncodeMicrosTotal; }
  long long videoSendMicros() const { return videoSendMicrosTotal; }
  long long videoReceiveMicros() const { return videoReceiveMicrosTotal; }
  long long videoWriteMicros() const { return videoWriteMicrosTotal; }
  long long videoStallMicros() const { return videoStallMicrosTotal; }
  long long videoFlushMicros() const { return videoFlushMicrosTotal; }
  long long videoPackets() const { return videoPacketCount; }
  int videoStalls() const { return videoStalledRetries; }
  int videoThreadCount() const {
    return videoContext != nullptr ? videoContext->thread_count : 0;
  }
  int maxVideoFrameRefCount() const { return maxVideoFrameBufferRefCount; }
  long long videoFrameCowCopies() const { return videoFrameCowCopyCount; }

private:
  void cleanup() {
    if (audioFile != nullptr) {
      sf_close(audioFile);
      audioFile = nullptr;
    }
    if (swsContext != nullptr) {
      sws_freeContext(swsContext);
      swsContext = nullptr;
    }
    av_packet_free(&audioPacket);
    av_packet_free(&videoPacket);
    av_frame_free(&audioFrame);
    av_frame_free(&videoFrame);
    avcodec_free_context(&audioContext);
    avcodec_free_context(&videoContext);
    if (formatContext != nullptr &&
        !(formatContext->oformat->flags & AVFMT_NOFILE)) {
      avio_closep(&formatContext->pb);
    }
    avformat_free_context(formatContext);
    formatContext = nullptr;
    videoStream = nullptr;
    audioStream = nullptr;
  }

  std::filesystem::path outputPath;
  int width = 0;
  int height = 0;
  int fps = 0;
  int64_t videoFrameDuration = 1;
  AVFormatContext *formatContext = nullptr;
  AVCodecContext *videoContext = nullptr;
  AVCodecContext *audioContext = nullptr;
  AVStream *videoStream = nullptr;
  AVStream *audioStream = nullptr;
  AVFrame *videoFrame = nullptr;
  AVFrame *audioFrame = nullptr;
  AVPacket *videoPacket = nullptr;
  AVPacket *audioPacket = nullptr;
  SwsContext *swsContext = nullptr;
  SNDFILE *audioFile = nullptr;
  std::vector<float> audioBuffer;
#if __APPLE__
  vImage_ARGBToYpCbCr vImageBgraToNv12Converter{};
  bool useVImageBgraToNv12 = false;
#endif
  long long audioEncodeMicrosTotal = 0;
  long long framePrepareMicrosTotal = 0;
  long long pixelConvertMicrosTotal = 0;
  long long videoEncodeMicrosTotal = 0;
  long long videoSendMicrosTotal = 0;
  long long videoReceiveMicrosTotal = 0;
  long long videoWriteMicrosTotal = 0;
  long long videoStallMicrosTotal = 0;
  long long videoFlushMicrosTotal = 0;
  long long videoPacketCount = 0;
  long long videoFrameCowCopyCount = 0;
  int videoStalledRetries = 0;
  int maxVideoFrameBufferRefCount = 0;
  int64_t nextAudioPts = 0;
  int pixelConvertThreadCount = 1;
  bool audioFinished = false;
};

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
// Disabled as the default export path: the native AVAssetWriter run stalled at
// 1080p60 while status remained writing, and showed no measurable speedup at
// the 1080-frame checkpoint versus the FFmpeg/VideoToolbox path.
class ReplayIOSMp4StreamWriter {
public:
  ~ReplayIOSMp4StreamWriter() { cleanup(); }

  bool open(const std::filesystem::path &wavPath,
            const std::filesystem::path &outputPath, int width, int height,
            int fps, ReplayVideoExportLog *log, std::string &errorMessage) {
    this->outputPath = outputPath;
    this->log = log;
    const int64_t bitRate = replayVideoBitRate(width, height, fps);
    replayExportLog(log,
                    "Replay video native writer setup started: %dx%d @ "
                    "%dfps, bitrate %lld",
                    width, height, fps, static_cast<long long>(bitRate));
    std::string nativeErrorMessage;
    writer = CreateIOSReplayVideoWriter(
        fspath_to_utf8(wavPath), fspath_to_utf8(outputPath), width, height, fps,
        bitRate, nativeErrorMessage);
    if (writer != nullptr) {
      replayExportLog(log,
                      "Replay video export encoder: avassetwriter_hevc, pixel "
                      "format: bgra, time base: 1/%d, frame duration: 1, HEVC "
                      "profile: auto, bitrate: %lld",
                      fps, static_cast<long long>(bitRate));
      replayExportLog(log,
                      "Replay video export native writer: BGRA pixel buffers, "
                      "no CPU pixel conversion");
      replayExportLog(log,
                      "Replay video export native writer: audio interleaved "
                      "during video append");
      return true;
    }

    errorMessage = nativeErrorMessage;
    replayExportLog(log, "Replay video native writer setup failed: %s",
                    errorMessage.c_str());
    return false;
  }

  bool encodeVideoFrame(const uint8_t *bgraFrame, size_t frameIndex,
                        long long /*videoTimeMicros*/,
                        std::string &errorMessage) {
    const bool success =
        AppendIOSReplayVideoFrame(writer, bgraFrame, frameIndex, errorMessage);
    if (!success) {
      replayExportLog(log, "Replay video native writer append failed: %s",
                      errorMessage.c_str());
    }
    return success;
  }

  ReplayVideoExportResult finish() {
    std::string errorMessage;
    IOSReplayVideoWriterProfile profile{};
    if (!FinishIOSReplayVideoWriter(writer, profile, errorMessage)) {
      writer = nullptr;
      replayExportLog(log, "Replay video native writer finish failed: %s",
                      errorMessage.c_str());
      return {
          .success = false, .outputPath = outputPath, .message = errorMessage};
    }
    writer = nullptr;
    audioEncodeMicrosTotal = profile.audioAppendMicros;
    framePrepareMicrosTotal = profile.videoPixelBufferCopyMicros;
    videoEncodeMicrosTotal = profile.videoAppendMicros + profile.finishMicros;
    replayExportLog(log,
                    "Replay video native writer profile: %.2fs BGRA copy, "
                    "%.2fs video append, %.2fs audio append, %.2fs finish",
                    static_cast<double>(profile.videoPixelBufferCopyMicros) /
                        1000000.0,
                    static_cast<double>(profile.videoAppendMicros) / 1000000.0,
                    static_cast<double>(profile.audioAppendMicros) / 1000000.0,
                    static_cast<double>(profile.finishMicros) / 1000000.0);
    return {
        .success = true, .outputPath = outputPath, .message = "MP4 exported"};
  }

  long long audioEncodeMicros() const { return audioEncodeMicrosTotal; }
  long long framePrepareMicros() const { return framePrepareMicrosTotal; }
  long long pixelConvertMicros() const { return 0; }
  long long videoEncodeMicros() const { return videoEncodeMicrosTotal; }
  long long videoSendMicros() const { return 0; }
  long long videoReceiveMicros() const { return 0; }
  long long videoWriteMicros() const { return videoEncodeMicrosTotal; }
  long long videoStallMicros() const { return 0; }
  long long videoFlushMicros() const { return 0; }
  long long videoPackets() const { return 0; }
  int videoStalls() const { return 0; }
  int videoThreadCount() const { return 0; }
  int maxVideoFrameRefCount() const { return 0; }
  long long videoFrameCowCopies() const { return 0; }

private:
  void cleanup() {
    if (writer != nullptr) {
      CancelIOSReplayVideoWriter(writer);
      writer = nullptr;
    }
  }

  std::filesystem::path outputPath;
  ReplayVideoExportLog *log = nullptr;
  void *writer = nullptr;
  long long audioEncodeMicrosTotal = 0;
  long long framePrepareMicrosTotal = 0;
  long long videoEncodeMicrosTotal = 0;
};

using ReplayPlatformMp4StreamWriter = ReplayMp4StreamWriter;
#else
using ReplayPlatformMp4StreamWriter = ReplayMp4StreamWriter;
#endif

class ReplayAsyncFrameEncoder {
public:
  ~ReplayAsyncFrameEncoder() { cancel(); }

  bool start(const std::filesystem::path &wavPath,
             const std::filesystem::path &outputPath, int width, int height,
             int fps, size_t frameBytes, size_t bufferCount,
             ReplayVideoExportLog *log, std::string &errorMessage) {
    if (!writer.open(wavPath, outputPath, width, height, fps, log,
                     errorMessage)) {
      return false;
    }

    frameBuffers.assign(bufferCount, std::vector<uint8_t>(frameBytes));
    {
      std::lock_guard<std::mutex> lock(mutex);
      acceptingFrames = true;
      failed = false;
      cancelled = false;
      encodedFrameCount.store(0, std::memory_order_relaxed);
      workerFinished.store(false, std::memory_order_release);
      for (size_t i = 0; i < frameBuffers.size(); ++i) {
        freeBuffers.push_back(i);
      }
    }

    worker = std::thread([this]() { encodeLoop(); });
    return true;
  }

  int acquireFrameBuffer(std::string &errorMessage) {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [this]() {
      return failed || !freeBuffers.empty() || !acceptingFrames;
    });
    if (failed) {
      errorMessage = failureMessage;
      return -1;
    }
    if (freeBuffers.empty()) {
      errorMessage = "Replay video encoder stopped unexpectedly";
      return -1;
    }
    const size_t index = freeBuffers.front();
    freeBuffers.pop_front();
    return static_cast<int>(index);
  }

  uint8_t *frameData(size_t index) { return frameBuffers[index].data(); }

  bool submitFrame(size_t bufferIndex, size_t frameIndex,
                   long long videoTimeMicros, std::string &errorMessage) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (failed) {
        errorMessage = failureMessage;
        return false;
      }
      if (!acceptingFrames) {
        errorMessage = "Replay video encoder is not accepting frames";
        return false;
      }
      pendingFrames.push_back({bufferIndex, frameIndex, videoTimeMicros});
    }
    condition.notify_one();
    return true;
  }

  ReplayVideoExportResult
  finish(const std::function<void(size_t)> &progressCallback = {}) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      acceptingFrames = false;
    }
    condition.notify_all();
    auto lastProgress =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(500);
    while (!workerFinished.load(std::memory_order_acquire)) {
      const auto now = std::chrono::steady_clock::now();
      if (progressCallback &&
          now - lastProgress >= std::chrono::milliseconds(250)) {
        lastProgress = now;
        progressCallback(encodedFrameCount.load(std::memory_order_relaxed));
      }
      SDL_Delay(10);
    }
    if (progressCallback) {
      progressCallback(encodedFrameCount.load(std::memory_order_relaxed));
    }
    joinWorker();

    {
      std::lock_guard<std::mutex> lock(mutex);
      if (failed) {
        return {.success = false, .message = failureMessage};
      }
    }
    return writer.finish();
  }

  void cancel() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      acceptingFrames = false;
      cancelled = true;
      pendingFrames.clear();
    }
    condition.notify_all();
    joinWorker();
  }

  long long encodedMicros() const {
    return encodedMicrosTotal.load(std::memory_order_relaxed);
  }
  size_t encodedFrames() const {
    return encodedFrameCount.load(std::memory_order_relaxed);
  }

  long long audioEncodeMicros() const { return writer.audioEncodeMicros(); }
  long long framePrepareMicros() const { return writer.framePrepareMicros(); }
  long long pixelConvertMicros() const { return writer.pixelConvertMicros(); }
  long long videoEncodeMicros() const { return writer.videoEncodeMicros(); }
  long long videoSendMicros() const { return writer.videoSendMicros(); }
  long long videoReceiveMicros() const { return writer.videoReceiveMicros(); }
  long long videoWriteMicros() const { return writer.videoWriteMicros(); }
  long long videoStallMicros() const { return writer.videoStallMicros(); }
  long long videoFlushMicros() const { return writer.videoFlushMicros(); }
  long long videoPackets() const { return writer.videoPackets(); }
  int videoStalls() const { return writer.videoStalls(); }
  int videoThreadCount() const { return writer.videoThreadCount(); }
  int maxVideoFrameRefCount() const {
    return writer.maxVideoFrameRefCount();
  }
  long long videoFrameCowCopies() const {
    return writer.videoFrameCowCopies();
  }

private:
  struct PendingFrame {
    size_t bufferIndex = 0;
    size_t frameIndex = 0;
    long long videoTimeMicros = 0;
  };

  void joinWorker() {
    if (worker.joinable()) {
      worker.join();
    }
  }

  void encodeLoop() {
    struct WorkerFinishedMarker {
      std::atomic_bool &finished;
      ~WorkerFinishedMarker() {
        finished.store(true, std::memory_order_release);
      }
    } marker{workerFinished};

    while (true) {
      PendingFrame frame{};
      {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this]() {
          return cancelled || failed || !pendingFrames.empty() ||
                 !acceptingFrames;
        });
        if (cancelled || failed ||
            (pendingFrames.empty() && !acceptingFrames)) {
          return;
        }
        frame = pendingFrames.front();
        pendingFrames.pop_front();
      }

      std::string errorMessage;
      const auto encodeStart = std::chrono::steady_clock::now();
      const bool success = writer.encodeVideoFrame(
          frameBuffers[frame.bufferIndex].data(), frame.frameIndex,
          frame.videoTimeMicros, errorMessage);
      encodedMicrosTotal.fetch_add(elapsedMicros(encodeStart),
                                   std::memory_order_relaxed);
      if (success) {
        encodedFrameCount.fetch_add(1, std::memory_order_relaxed);
      }

      {
        std::lock_guard<std::mutex> lock(mutex);
        freeBuffers.push_back(frame.bufferIndex);
        if (!success && !failed) {
          failed = true;
          failureMessage = errorMessage;
          acceptingFrames = false;
          pendingFrames.clear();
        }
      }
      condition.notify_all();
    }
  }

  ReplayPlatformMp4StreamWriter writer;
  std::vector<std::vector<uint8_t>> frameBuffers;
  std::deque<size_t> freeBuffers;
  std::deque<PendingFrame> pendingFrames;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::thread worker;
  std::atomic_llong encodedMicrosTotal{0};
  std::atomic_size_t encodedFrameCount{0};
  std::atomic_bool workerFinished{true};
  bool acceptingFrames = false;
  bool failed = false;
  bool cancelled = false;
  std::string failureMessage;
};

ReplayVideoExportResult
renderReplayVideoToMp4(ApplicationContext &context, bms_parser::Chart &chart,
                       const ReplayData &replay, const AppSettings &settings,
                       const preparation::Plan &preparationPlan,
                       PreparedReplayGameplayPresentation &preparedGameplay,
                       const ReplayVideoExportOptions &options,
                       const std::filesystem::path &wavPath,
                       const std::filesystem::path &outputPath,
                       long long requestedGameplayDurationMicros,
                       long long requestedAudioDurationMicros,
                       bool stoppedOnGaugeFailure,
                       ReplayVideoExportLog *log) {
  const auto resolvedOptions = resolveReplayVideoExportOptions(options);
  const int width = resolvedOptions.width;
  const int height = resolvedOptions.height;
  const int fps = resolvedOptions.fps;
  const long long audioOffsetMicros =
      static_cast<long long>(settings.audioOffsetMs) * 1000LL;
  long long gameplayDurationMicros =
      std::max(0LL, requestedGameplayDurationMicros);

  if (width > UINT16_MAX || height > UINT16_MAX) {
    return {.success = false,
            .outputPath = outputPath,
            .message = "Replay export size is too large"};
  }

  ScopedReplayVideoBgfxAccess bgfxAccess(context);
  ScopedReplayVideoRenderGeometry exportGeometry(width, height);
  const uint64_t requiredCaps =
      BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK;
  if ((bgfx::getCaps()->supported & requiredCaps) != requiredCaps) {
    return {.success = false,
            .outputPath = outputPath,
            .message = "Renderer does not support texture readback"};
  }

  context.jukebox.setBgaDisplayMode(settings.bgaDisplayMode);
  context.jukebox.setVisualsEnabled(settings.bgaEnabled);
  context.jukebox.setEmbeddedBgaBrightnessPercent(
      settings.bgaBrightnessPercent);
  std::atomic_bool visualLoadCancelled = false;
  context.jukebox.loadVisuals(chart, visualLoadCancelled);
  if (visualLoadCancelled) {
    context.jukebox.stop();
    context.jukebox.unloadVisuals();
    return {.success = false,
            .outputPath = outputPath,
            .message = "Replay export visual loading was cancelled"};
  }

  const auto outputTexture = bgfx::createTexture2D(
      static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
      bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
  if (!bgfx::isValid(outputTexture)) {
    context.jukebox.stop();
    context.jukebox.unloadVisuals();
    return {.success = false,
            .outputPath = outputPath,
            .message = "Failed to create replay export render target"};
  }

  bgfx::FrameBufferHandle outputFrameBuffer = BGFX_INVALID_HANDLE;
  const size_t frameBytes = replayVideoFrameBytes(width, height);
  const size_t frameBufferCount = replayVideoFrameBufferCount(width, height);
  std::vector<bgfx::TextureHandle> readbackTextures(frameBufferCount,
                                                    BGFX_INVALID_HANDLE);
  std::unique_ptr<rendering::BlurPass> bgaBlurPass;
  std::unique_ptr<View> resultRoot;
  View *resultGraphPlaceholder = nullptr;
  PracticeAnalyticsView *resultAnalytics = nullptr;
  PracticeAnalyticsMode resultAnalyticsMode =
      PracticeAnalyticsMode::Histogram;
  auto cleanupBgfx = [&]() {
    resultGraphPlaceholder = nullptr;
    resultAnalytics = nullptr;
    resultRoot.reset();
    context.jukebox.stop();
    context.jukebox.unloadVisuals();
    exportGeometry.restorePrimary();
    restorePrimaryRenderViews(&context);
    if (bgaBlurPass != nullptr) {
      bgaBlurPass->shutdown();
      bgaBlurPass.reset();
    }
    for (auto &readbackTexture : readbackTextures) {
      if (bgfx::isValid(readbackTexture)) {
        bgfx::destroy(readbackTexture);
        readbackTexture = BGFX_INVALID_HANDLE;
      }
    }
    if (bgfx::isValid(outputFrameBuffer)) {
      bgfx::destroy(outputFrameBuffer);
      outputFrameBuffer = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(outputTexture)) {
      bgfx::destroy(outputTexture);
    }
  };
  auto bgfxCleanup = makeScopeExit(cleanupBgfx);

  outputFrameBuffer = bgfx::createFrameBuffer(1, &outputTexture, false);
  if (!bgfx::isValid(outputFrameBuffer)) {
    bgfxCleanup.runNow();
    return {.success = false,
            .outputPath = outputPath,
            .message = "Failed to create replay export framebuffer"};
  }

  for (auto &readbackTexture : readbackTextures) {
    readbackTexture = bgfx::createTexture2D(
        static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
        bgfx::TextureFormat::BGRA8,
        BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
    if (!bgfx::isValid(readbackTexture)) {
      bgfxCleanup.runNow();
      return {.success = false,
              .outputPath = outputPath,
              .message = "Failed to create replay export readback texture"};
    }
  }

  bgaBlurPass = std::make_unique<rendering::BlurPass>(2, 0.6f);
  bgaBlurPass->init(static_cast<uint16_t>(width),
                    static_cast<uint16_t>(height));
  bgaBlurPass->setInputViews(
      std::vector<bgfx::ViewId>(rendering::kGameplayBgaInputViews.begin(),
                                rendering::kGameplayBgaInputViews.end()));
  bgaBlurPass->setCompositeEnabled(false);
  bgaBlurPass->setBlurStrength(settings.bgaBlurStrength);

  exportGeometry.applyExport();
  configureReplayExportRenderViews(width, height, outputFrameBuffer,
                                   *bgaBlurPass, settings);
  auto restoreExportRenderViews = [&]() {
    exportGeometry.applyExport();
    configureReplayExportRenderViews(width, height, outputFrameBuffer,
                                     *bgaBlurPass, settings);
  };
  auto allowUiFrame = [&]() {
    exportGeometry.restorePrimary();
    bool exportViewsRestored = false;
    bgfxAccess.allowUiFrame([&]() {
      restoreExportRenderViews();
      exportViewsRestored = true;
    });
    if (!exportViewsRestored) {
      restoreExportRenderViews();
    }
  };

  if (preparedGameplay.presentation == nullptr) {
    return {.success = false,
            .outputPath = outputPath,
            .message = "Replay gameplay presentation was not prepared"};
  }
  gameplayDurationMicros =
      replay_video_export::replayGameplayDurationWithSelectedSkinAnimation(
          chart, replay, preparationPlan, audioOffsetMicros, fps,
          gameplayDurationMicros, stoppedOnGaugeFailure,
          preparedGameplay.presentation.get());

  const auto playbackRateChangeTimelines =
      collectPlaybackRateChangeTimelines(chart);
  size_t playbackRateChangeCursor = 0;
  double currentExportBpm = chart.Meta.Bpm;
  double currentExportScrollRate = 1.0;
  auto applyExportPlaybackRate = [&](long long songTimeMicros) {
    while (playbackRateChangeCursor < playbackRateChangeTimelines.size() &&
           playbackRateChangeTimelines[playbackRateChangeCursor]->Timing <=
               songTimeMicros) {
      const auto *timeline =
          playbackRateChangeTimelines[playbackRateChangeCursor];
      currentExportBpm = timeline->Bpm;
      currentExportScrollRate = timeline->Scroll;
      ++playbackRateChangeCursor;
    }
  };
  replay_video_export::ReplayLaneCoverPlayback laneCoverPlayback(
      settings.noteStartPositionPercent, settings.laneCoverEnabled);
  const GaugeProfile gaugeProfile =
      resolveGaugeProfile(GaugeProfile::Standard, chart.Meta.KeyMode);
  const RhythmState initialGaugeState =
      replay_result::BuildInitialGaugeState(chart, replay, gaugeProfile);
  GaugeType replayGaugeType = initialGaugeState.gaugeType;
  float replayGauge = initialGaugeState.currentGauge;
  const std::optional<ResultPreviousBestData> previousBest =
      result_presentation::previousBestForReplayChart(
          context.scoreRepository, chart.Meta, replay);
  const std::string selectedPacemakerTarget =
      resolvedOptions.pacemakerTarget.empty()
          ? settings.selectedPacemakerTarget
          : resolvedOptions.pacemakerTarget;
  const auto bestScoreReplay = result_presentation::replayForPreviousBestChart(
      context, chart.Meta, previousBest, pacemaker::kTargetBest,
      visualLoadCancelled);
  const auto bestScoreAuthority =
      result_presentation::gameplayBestScoreAuthorityForReplay(
          chart, replay, previousBest, bestScoreReplay.get());
  const pacemaker::Target activePacemakerTarget =
      result_presentation::pacemakerTargetForReplay(
          chart, replay, selectedPacemakerTarget, previousBest,
          bestScoreReplay.get());
  RhythmState pacemakerState(&chart, false);
  pacemakerState.configureGauge(replay.initialGaugeType,
                                replay.gaugeAutoShift,
                                GaugeProfile::Standard,
                                replay.gaugeAutoShiftLowerBound);
  const long long scheduledVisualEndMicros =
      preparationPlan.realTimeAtGameplayTime(
          context.jukebox.getScheduledVisualEndMicros(), audioOffsetMicros);
  const long long visualTailMicros =
      stoppedOnGaugeFailure
          ? 0LL
          : std::max(0LL,
                     scheduledVisualEndMicros - gameplayDurationMicros);
  const long long audioTailMicros =
      stoppedOnGaugeFailure
          ? 0LL
          : std::max(0LL,
                     requestedAudioDurationMicros - gameplayDurationMicros);
  const long long resultTailMicros =
      resolvedOptions.includeResultScreen
          ? std::max({kResultSceneTailMicros, visualTailMicros,
                      audioTailMicros})
          : std::max(visualTailMicros, audioTailMicros);
  const long long totalDurationMicros =
      gameplayDurationMicros + resultTailMicros;
  const long long visualOffsetMicros =
      static_cast<long long>(settings.visualOffsetMs) * 1000LL;
  const size_t gameplayFrameCount = static_cast<size_t>(std::ceil(
      static_cast<long double>(gameplayDurationMicros) * fps / 1000000.0L));
  const size_t resultFrameCount = static_cast<size_t>(
      std::ceil(static_cast<long double>(resultTailMicros) * fps / 1000000.0L));
  const size_t frameCount = gameplayFrameCount + resultFrameCount;
  const ReplayData resultReplay = replayThroughFailure(
      replay, stoppedOnGaugeFailure
                  ? replay_result::FindGaugeFailureMicros(
                        chart, replay, GaugeProfile::Standard)
                  : std::nullopt);
  const RhythmState replayResultState =
      replay_result::BuildResultState(chart, resultReplay);
  rendering::SimpleBatchRenderer resultGraphBatch;
  if (resultFrameCount > 0 && resolvedOptions.includeResultScreen) {
    resultRoot = std::make_unique<View>(0, 0, rendering::window_width,
                                        rendering::window_height);
    const std::string difficultyLabel =
        result_presentation::difficultyLabelForChart(context.chartRepository,
                                                      chart.Meta);
    ResultSkinData resultSkinData = {&replayResultState, &chart.Meta, &context};
    resultSkinData.outGraphPlaceholder = &resultGraphPlaceholder;
    resultSkinData.showControls = false;
    const play_options::PlayModeDisplayLabel playModeDisplay =
        play_options::formatPlayModeDisplayLabel(replay);
    resultSkinData.playModeLabel = playModeDisplay.mode;
    resultSkinData.laneOrderLabel = playModeDisplay.laneOrder;
    resultSkinData.difficultyLabel = difficultyLabel;
    if (replay.autoPlay) {
      resultSkinData.currentClearLabelOverride = "AUTO PLAY";
    }
    resultSkinData.previousBest = previousBest;
    resultSkinData.pacemaker =
        result_presentation::pacemakerDataForReplayResult(
            chart, replayResultState, replay, selectedPacemakerTarget,
            previousBest, bestScoreReplay.get());
    DefaultSkin resultSkin;
    resultSkin.buildLayout("Result", resultRoot.get(), &resultSkinData);
    resultAnalytics =
        addReplayResultAnalytics(*resultRoot, chart, resultReplay);
    resultRoot->applyYogaLayout();
  }
  ReplayAsyncFrameEncoder encoder;
  std::string errorMessage;
  std::filesystem::path videoAudioPath = wavPath;
  std::filesystem::path alignedAudioPath;
  if (totalDurationMicros > 0) {
    std::filesystem::path alignedAudioName = wavPath.stem();
    alignedAudioName += PATH("_video.wav");
    alignedAudioPath = wavPath.parent_path() / alignedAudioName;
    if (!writeReplayAudioFileAtDuration(wavPath, alignedAudioPath,
                                        totalDurationMicros,
                                        requestedAudioDurationMicros,
                                        errorMessage)) {
      bgfxCleanup.runNow();
      return {
          .success = false, .outputPath = outputPath, .message = errorMessage};
    }
    videoAudioPath = alignedAudioPath;
  }
  if (!encoder.start(videoAudioPath, outputPath, width, height, fps, frameBytes,
                     frameBufferCount, log, errorMessage)) {
    bgfxCleanup.runNow();
    return {
        .success = false, .outputPath = outputPath, .message = errorMessage};
  }
  const double durationSeconds =
      static_cast<double>(totalDurationMicros) / 1000000.0;
  const double rawFrameGiB =
      (static_cast<double>(frameBytes) * static_cast<double>(frameCount)) /
      static_cast<double>(1024ULL * 1024ULL * 1024ULL);
  const double frameBufferMemoryMiB =
      (static_cast<double>(replayVideoFrameBufferSlotBytes(width, height)) *
       static_cast<double>(frameBufferCount)) /
      static_cast<double>(1024ULL * 1024ULL);
  const double frameBufferBudgetMiB =
      static_cast<double>(replayVideoFrameBufferMemoryBudgetBytes()) /
      static_cast<double>(1024ULL * 1024ULL);
  replayExportLog(log,
                  "Replay video export workload: %zu frames, %.2fs, %.2f GiB "
                  "BGRA readback",
                  frameCount, durationSeconds, rawFrameGiB);
  replayExportLog(log,
                  "Replay video export timing: gameplay %.2fs, result %.2fs, "
                  "audio %.2fs",
                  static_cast<double>(gameplayDurationMicros) / 1000000.0,
                  static_cast<double>(resultTailMicros) / 1000000.0,
                  static_cast<double>(requestedAudioDurationMicros) /
                      1000000.0);
  if (scheduledVisualEndMicros > gameplayDurationMicros) {
    replayExportLog(log,
                    "Replay video export BGA tail: visual end %.2fs, "
                    "gameplay end %.2fs",
                    static_cast<double>(scheduledVisualEndMicros) / 1000000.0,
                    static_cast<double>(gameplayDurationMicros) / 1000000.0);
  }
  replayExportLog(log,
                  "Replay video export frame buffers/readbacks: %zu, encoder "
                  "threads: %d, queued memory: %.1f MiB / %.1f MiB budget",
                  frameBufferCount, encoder.videoThreadCount(),
                  frameBufferMemoryMiB, frameBufferBudgetMiB);
  RenderContext renderContext;
  size_t replayCursor = 0;
  GameplayBgaMissStateTracker bgaMissTracker;
  std::uint64_t presentationSerial = 1;
  replay_video_export::ReplayJudgementAuthorityPlayback replayJudgementAuthority;
  uint32_t currentFrame = bgfx::frame();
  const auto exportStart = std::chrono::steady_clock::now();
  auto lastUiProgress =
      std::chrono::steady_clock::now() - std::chrono::milliseconds(500);
  long long bufferWaitMicros = 0;
  long long renderSubmitMicros = 0;
  long long readbackWaitMicros = 0;

  struct PendingReadback {
    size_t frameIndex = 0;
    size_t frameBufferIndex = 0;
    size_t readbackTextureIndex = 0;
    long long songTimeMicros = 0;
    uint32_t expectedFrame = 0;
  };
  std::deque<size_t> freeReadbackTextures;
  for (size_t i = 0; i < readbackTextures.size(); ++i) {
    freeReadbackTextures.push_back(i);
  }
  std::deque<PendingReadback> pendingReadbacks;

  auto drainOldestReadback = [&]() -> bool {
    if (pendingReadbacks.empty()) {
      return true;
    }

    PendingReadback pending = pendingReadbacks.front();
    pendingReadbacks.pop_front();

    const auto readbackWaitStart = std::chrono::steady_clock::now();
    while (currentFrame < pending.expectedFrame) {
      currentFrame = bgfx::frame();
    }
    readbackWaitMicros += elapsedMicros(readbackWaitStart);

    if (!encoder.submitFrame(pending.frameBufferIndex, pending.frameIndex,
                             pending.songTimeMicros, errorMessage)) {
      return false;
    }
    freeReadbackTextures.push_back(pending.readbackTextureIndex);
    return true;
  };

  auto drainReadyReadbacks = [&]() -> bool {
    while (!pendingReadbacks.empty() &&
           currentFrame >= pendingReadbacks.front().expectedFrame) {
      if (!drainOldestReadback()) {
        return false;
      }
    }
    return true;
  };

  auto videoPipelineProgress = [&](size_t renderedFrames,
                                   size_t encodedFrames) {
    if (frameCount == 0) {
      return 1.0;
    }
    const double completedUnits =
        static_cast<double>(std::min(renderedFrames, frameCount)) +
        static_cast<double>(std::min(encodedFrames, frameCount));
    return completedUnits / (static_cast<double>(frameCount) * 2.0);
  };

  auto reportVideoPipelineProgress = [&](size_t renderedFrames,
                                         size_t encodedFrames,
                                         const std::string &message) {
    const double progress =
        videoPipelineProgress(renderedFrames, encodedFrames);
    reportReplayExportProgress(options, 0.05 + 0.90 * progress, message,
                               std::min(renderedFrames, frameCount),
                               frameCount);
  };

  reportVideoPipelineProgress(0, 0, "Rendering / encoding video");

  auto maybeReportFrameProgress = [&](size_t renderedFrames, bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - lastUiProgress < std::chrono::milliseconds(250)) {
      return;
    }

    lastUiProgress = now;
    reportVideoPipelineProgress(renderedFrames, encoder.encodedFrames(),
                                "Rendering / encoding video");
    allowUiFrame();
  };

  auto renderAndQueueFrame = [&](size_t frameIndex, long long videoTimeMicros,
                                 auto &&renderFrame) -> bool {
    if (!drainReadyReadbacks()) {
      return false;
    }
    while (freeReadbackTextures.empty()) {
      if (!drainOldestReadback()) {
        return false;
      }
    }

    const auto bufferWaitStart = std::chrono::steady_clock::now();
    const int frameBufferIndex = encoder.acquireFrameBuffer(errorMessage);
    bufferWaitMicros += elapsedMicros(bufferWaitStart);
    if (frameBufferIndex < 0) {
      return false;
    }
    const size_t readbackTextureIndex = freeReadbackTextures.front();
    freeReadbackTextures.pop_front();

    const auto renderStart = std::chrono::steady_clock::now();
    renderFrame();
    bgfx::blit(rendering::readback_view, readbackTextures[readbackTextureIndex],
               0, 0, outputTexture);
    currentFrame = bgfx::frame();
    const uint32_t expectedFrame = bgfx::readTexture(
        readbackTextures[readbackTextureIndex],
        encoder.frameData(static_cast<size_t>(frameBufferIndex)));
    renderSubmitMicros += elapsedMicros(renderStart);
    pendingReadbacks.push_back(
        {.frameIndex = frameIndex,
         .frameBufferIndex = static_cast<size_t>(frameBufferIndex),
         .readbackTextureIndex = readbackTextureIndex,
         .songTimeMicros = videoTimeMicros,
         .expectedFrame = expectedFrame});
    maybeReportFrameProgress(frameIndex + 1, frameIndex + 1 == frameCount);
    return true;
  };

  for (size_t frameIndex = 0; frameIndex < gameplayFrameCount; ++frameIndex) {
    const long long videoTimeMicros = static_cast<long long>(
        (static_cast<long double>(frameIndex) * 1000000.0L) / fps);
    const long long rawSongTimeMicros =
        preparationPlan.chartTimeAtRealTime(videoTimeMicros);
    const auto frameTiming = gameplay_timing::frameTiming(
        rawSongTimeMicros, audioOffsetMicros, visualOffsetMicros);
    const auto presentationFrameState =
        replay_video_export::replayGameplayFrameState(
            preparationPlan, chart, replay, settings, ++presentationSerial,
            videoTimeMicros);
    while (replayCursor < replay.events.size() &&
           replay.events[replayCursor].songTimeMicros <=
               frameTiming.gameplayTimeMicros) {
      const auto &event = replay.events[replayCursor];
      const bool appliedHud =
          preparedGameplay.presentation->applyReplayEvent(
              event,
              makePlayfieldJudgeEventClock(event.songTimeMicros,
                                           visualOffsetMicros),
              true);
      if (event.judgement != None || event.action == ReplayEventAction::Mine ||
          event.action == ReplayEventAction::Gauge) {
        replayGaugeType = event.gaugeType;
        replayGauge = event.gauge;
      }
      if (appliedHud && event.judgement != None) {
        applyReplayEventToPacemakerState(pacemakerState, event);
        bgaMissTracker.onJudge(JudgeResult(event.judgement, event.diffMicros),
                               event.combo,
                               makePlayfieldJudgeEventClock(
                                   event.songTimeMicros, visualOffsetMicros));
        replayJudgementAuthority.recordApplied(event);
      }
      ++replayCursor;
    }
    applyExportPlaybackRate(frameTiming.gameplayTimeMicros);
    const auto laneCover = laneCoverPlayback.advance(
        replay.laneCoverEvents, frameTiming.gameplayTimeMicros);
    for (const auto &transition : laneCover.transitions) {
      preparedGameplay.presentation->applyLaneCoverTransition(
          transition, currentExportBpm);
    }
    preparedGameplay.presentation->releaseDueClassicLongNoteTails(
        frameTiming.gameplayTimeMicros);

    preparedGameplay.presentation->applyAuthorityUpdate({
        .currentBpm = currentExportBpm,
        .currentScrollRate = currentExportScrollRate,
        .judgementCounters = replayJudgementAuthority.judgementCounters(),
        .judgementFastSlowCounters =
            replayJudgementAuthority.judgementFastSlowCounters(),
        .comboBreak = replayJudgementAuthority.comboBreak(),
        .maximumCombo =
            preparedGameplay.presentation->progressiveMaximumCombo(),
        .bestScore = bestScoreAuthority.bestScore,
        .bestScoreTarget = bestScoreAuthority.bestScoreTarget,
        .gaugeType = replayGaugeType,
        .gaugeAutoShift = replay.gaugeAutoShift,
        .currentGauge = replayGauge,
        .gaugeRules = initialGaugeState.gaugeRules(),
        .pacemakerTarget = activePacemakerTarget,
        .pacemakerStatus =
            pacemaker::snapshotForState(activePacemakerTarget, pacemakerState),
        .playOptionLabel = replayExportPlayOptionLabel(replay),
        .autoPlayMarkVisible = replay.autoPlay,
        .gameplayMode = PlayfieldGameplayMode::Replay,
        .loadingState = PlayfieldLoadingState::Loaded,
        .startLaneIndicators = preparationPlan.laneIndicator.lanes,
        .startLaneIndicatorsVisible =
            preparationPlan.indicatorVisibleAt(rawSongTimeMicros),
        .laneCoverPercent = laneCover.percent,
        .laneCoverEnabled = laneCover.enabled,
        .laneCoverChanged = false,
    });

    bool presentationFailed = false;
    if (!renderAndQueueFrame(frameIndex, videoTimeMicros, [&]() {
          bgfx::touch(rendering::clear_view);
          bgfx::touch(rendering::bga_view);
          bgfx::touch(rendering::bga_layer_view);
          const auto presentationFrame = preparedGameplay.presentation->renderFrame(
              renderContext,
              presentationFrameState.clock,
              {.includeInvisibleNotes = settings.showInvisibleNotes});
          if (presentationFrame.outcome ==
                  PresentationFrameOutcome::CriticalFailure ||
              presentationFrame.failure) {
            replay_video_export::releaseUnsubmittedReplayGameplayBga(
                context.jukebox, presentationFrame);
            errorMessage = presentationFrame.failure
                               ? replay_video_export::skinExportFailureMessage(
                                     *presentationFrame.failure)
                               : "Replay gameplay presentation failed "
                                 "without a diagnostic "
                                 "[presentation.frame_failure_missing]";
            presentationFailed = true;
            return;
          }
          if (presentationFrame.bgaCompositeMode ==
                  GameplayBgaCompositeMode::FullscreenBuiltIn &&
              presentationFrame.preparedBga) {
            context.jukebox.submitFullscreen(*presentationFrame.preparedBga);
            bgaBlurPass->execute();
            rendering::renderFullscreenTextureTint(
                bgaBlurPass->outputTexture(), rendering::final_view,
                static_cast<float>(settings.bgaBrightnessPercent) / 100.0f);
          }
        })) {
      bgfxCleanup.runNow();
      return {
          .success = false, .outputPath = outputPath, .message = errorMessage};
    }
    if (presentationFailed) {
      bgfxCleanup.runNow();
      return {
          .success = false, .outputPath = outputPath, .message = errorMessage};
    }

    if (frameIndex == 0 || (frameIndex + 1) % static_cast<size_t>(fps) == 0 ||
        frameIndex + 1 == frameCount) {
      replayExportLog(log, "Replay video export encoded frame %zu/%zu",
                      frameIndex + 1, frameCount);
    }
  }

  for (size_t resultFrameIndex = 0; resultFrameIndex < resultFrameCount;
       ++resultFrameIndex) {
    const size_t frameIndex = gameplayFrameCount + resultFrameIndex;
    const long long videoTimeMicros =
        gameplayDurationMicros +
        static_cast<long long>((static_cast<long double>(resultFrameIndex) *
                                1000000.0L) /
                               fps);
    const long long rawSongTimeMicros =
        preparationPlan.chartTimeAtRealTime(videoTimeMicros);
    const long long bgaTimeMicros =
        gameplay_timing::gameplayTimeFromRawSongTime(rawSongTimeMicros,
                                                     audioOffsetMicros);
    const auto analyticsMode =
        practice_analytics_presentation::analyticsModeForSlideshow(
            videoTimeMicros - gameplayDurationMicros, resultTailMicros);
    if (resultAnalytics != nullptr && analyticsMode != resultAnalyticsMode) {
      resultAnalytics->setMode(analyticsMode);
      resultAnalyticsMode = analyticsMode;
    }
    if (!renderAndQueueFrame(frameIndex, videoTimeMicros, [&]() {
          bgfx::touch(rendering::clear_view);
          bgfx::touch(rendering::bga_view);
          bgfx::touch(rendering::bga_layer_view);
          const auto bgaFrame = context.jukebox.prepareVisualFrameAt(
              ++presentationSerial, bgaTimeMicros, bgaMissTracker.snapshot());
          context.jukebox.submitFullscreen(bgaFrame);
          bgaBlurPass->execute();
          rendering::renderFullscreenTextureTint(
              bgaBlurPass->outputTexture(), rendering::final_view,
              static_cast<float>(settings.bgaBrightnessPercent) / 100.0f);
          bgfx::touch(rendering::ui_view);
          if (resultRoot != nullptr) {
            resultRoot->render(renderContext);
            drawReplayResultGaugeGraph(resultGraphBatch, replayResultState,
                                       resultGraphPlaceholder);
          }
        })) {
      bgfxCleanup.runNow();
      return {
          .success = false, .outputPath = outputPath, .message = errorMessage};
    }

    if ((frameIndex + 1) % static_cast<size_t>(fps) == 0 ||
        frameIndex + 1 == frameCount) {
      replayExportLog(log, "Replay video export encoded frame %zu/%zu",
                      frameIndex + 1, frameCount);
    }
  }

  while (!pendingReadbacks.empty()) {
    if (!drainOldestReadback()) {
      bgfxCleanup.runNow();
      return {
          .success = false, .outputPath = outputPath, .message = errorMessage};
    }
  }

  bgfxCleanup.runNow();
  bgfxAccess.release();
  auto result = encoder.finish([&](size_t encodedFrames) {
    reportVideoPipelineProgress(frameCount, encodedFrames, "Encoding video");
  });
  if (result.success) {
    reportReplayExportProgress(options, 0.97, "Finalizing video", frameCount,
                               frameCount);
  }
  if (!result.success && result.outputPath.empty()) {
    result.outputPath = outputPath;
  }
  replayExportLog(log,
                  "Replay video export profile: %.2fs total, %.2fs render "
                  "submit, %.2fs readback wait, %.2fs encode worker, %.2fs "
                  "waiting for frame buffers",
                  static_cast<double>(elapsedMicros(exportStart)) / 1000000.0,
                  static_cast<double>(renderSubmitMicros) / 1000000.0,
                  static_cast<double>(readbackWaitMicros) / 1000000.0,
                  static_cast<double>(encoder.encodedMicros()) / 1000000.0,
                  static_cast<double>(bufferWaitMicros) / 1000000.0);
  replayExportLog(log,
                  "Replay video encoder profile: %.2fs audio, %.2fs frame "
                  "prepare, %.2fs pixel convert, %.2fs video encode/write",
                  static_cast<double>(encoder.audioEncodeMicros()) / 1000000.0,
                  static_cast<double>(encoder.framePrepareMicros()) / 1000000.0,
                  static_cast<double>(encoder.pixelConvertMicros()) / 1000000.0,
                  static_cast<double>(encoder.videoEncodeMicros()) / 1000000.0);
  replayExportLog(log,
                  "Replay video encoder detail: %.2fs send, %.2fs receive, "
                  "%.2fs mux write, %.2fs stalls, %.2fs flush, packets=%lld, "
                  "stall retries=%d",
                  static_cast<double>(encoder.videoSendMicros()) / 1000000.0,
                  static_cast<double>(encoder.videoReceiveMicros()) /
                      1000000.0,
                  static_cast<double>(encoder.videoWriteMicros()) / 1000000.0,
                  static_cast<double>(encoder.videoStallMicros()) / 1000000.0,
                  static_cast<double>(encoder.videoFlushMicros()) / 1000000.0,
                  encoder.videoPackets(), encoder.videoStalls());
  replayExportLog(log,
                  "Replay video frame buffer detail: max refcount=%d, COW "
                  "copies=%lld",
                  encoder.maxVideoFrameRefCount(),
                  encoder.videoFrameCowCopies());
  return result;
}

ReplayVideoExportResult renderCourseReplayVideoToMp4(
    ApplicationContext &context, const CourseReplayData &replay,
    std::vector<CourseReplayVideoStage> &stages, const AppSettings &settings,
    const ReplayVideoExportOptions &options,
    const std::filesystem::path &wavPath,
    const std::filesystem::path &outputPath, ReplayVideoExportLog *log) {
  if (stages.empty()) {
    return {.success = false, .outputPath = outputPath,
            .message = "No course replay stages"};
  }

  const auto resolvedOptions = resolveReplayVideoExportOptions(options);
  const int width = resolvedOptions.width;
  const int height = resolvedOptions.height;
  const int fps = resolvedOptions.fps;
  const long long audioOffsetMicros =
      static_cast<long long>(settings.audioOffsetMs) * 1000LL;

  if (width > UINT16_MAX || height > UINT16_MAX) {
    return {.success = false,
            .outputPath = outputPath,
            .message = "Replay export size is too large"};
  }

  auto framesForMicros = [fps](long long micros) -> size_t {
    if (micros <= 0) {
      return 0;
    }
    return static_cast<size_t>(
        std::ceil(static_cast<long double>(micros) *
                  static_cast<long double>(fps) / 1000000.0L));
  };

  size_t frameCount = 0;
  long long totalDurationMicros = 0;
  for (const auto &stage : stages) {
    frameCount += framesForMicros(stage.gameplayDurationMicros);
    frameCount += framesForMicros(stage.resultDurationMicros);
    totalDurationMicros +=
        std::max(0LL, stage.gameplayDurationMicros) +
        std::max(0LL, stage.resultDurationMicros);
  }
  const long long courseResultDurationMicros =
      courseResultDurationMicrosForReplayVideo(
          stages, resolvedOptions.includeResultScreen);
  const size_t courseResultFrameCount =
      framesForMicros(courseResultDurationMicros);
  frameCount += courseResultFrameCount;
  totalDurationMicros += courseResultDurationMicros;

  ScopedReplayVideoBgfxAccess bgfxAccess(context);
  ScopedReplayVideoRenderGeometry exportGeometry(width, height);
  const uint64_t requiredCaps =
      BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK;
  if ((bgfx::getCaps()->supported & requiredCaps) != requiredCaps) {
    return {.success = false,
            .outputPath = outputPath,
            .message = "Renderer does not support texture readback"};
  }

  const auto outputTexture = bgfx::createTexture2D(
      static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
      bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
  if (!bgfx::isValid(outputTexture)) {
    return {.success = false,
            .outputPath = outputPath,
            .message = "Failed to create replay export render target"};
  }

  bgfx::FrameBufferHandle outputFrameBuffer = BGFX_INVALID_HANDLE;
  const size_t frameBytes = replayVideoFrameBytes(width, height);
  const size_t frameBufferCount = replayVideoFrameBufferCount(width, height);
  std::vector<bgfx::TextureHandle> readbackTextures(frameBufferCount,
                                                    BGFX_INVALID_HANDLE);
  std::unique_ptr<rendering::BlurPass> bgaBlurPass;
  std::unique_ptr<View> stageResultRoot;
  View *stageResultGraphPlaceholder = nullptr;
  PracticeAnalyticsView *stageResultAnalytics = nullptr;
  PracticeAnalyticsMode stageResultAnalyticsMode =
      PracticeAnalyticsMode::Histogram;
  std::unique_ptr<View> courseResultRoot;
  View *courseResultGraphPlaceholder = nullptr;

  auto cleanupBgfx = [&]() {
    stageResultGraphPlaceholder = nullptr;
    stageResultAnalytics = nullptr;
    courseResultGraphPlaceholder = nullptr;
    stageResultRoot.reset();
    courseResultRoot.reset();
    context.jukebox.stop();
    context.jukebox.unloadVisuals();
    exportGeometry.restorePrimary();
    restorePrimaryRenderViews(&context);
    if (bgaBlurPass != nullptr) {
      bgaBlurPass->shutdown();
      bgaBlurPass.reset();
    }
    for (auto &readbackTexture : readbackTextures) {
      if (bgfx::isValid(readbackTexture)) {
        bgfx::destroy(readbackTexture);
        readbackTexture = BGFX_INVALID_HANDLE;
      }
    }
    if (bgfx::isValid(outputFrameBuffer)) {
      bgfx::destroy(outputFrameBuffer);
      outputFrameBuffer = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(outputTexture)) {
      bgfx::destroy(outputTexture);
    }
  };
  auto bgfxCleanup = makeScopeExit(cleanupBgfx);

  outputFrameBuffer = bgfx::createFrameBuffer(1, &outputTexture, false);
  if (!bgfx::isValid(outputFrameBuffer)) {
    bgfxCleanup.runNow();
    return {.success = false,
            .outputPath = outputPath,
            .message = "Failed to create replay export framebuffer"};
  }

  for (auto &readbackTexture : readbackTextures) {
    readbackTexture = bgfx::createTexture2D(
        static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
        bgfx::TextureFormat::BGRA8,
        BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
    if (!bgfx::isValid(readbackTexture)) {
      bgfxCleanup.runNow();
      return {.success = false,
              .outputPath = outputPath,
              .message = "Failed to create replay export readback texture"};
    }
  }

  bgaBlurPass = std::make_unique<rendering::BlurPass>(2, 0.6f);
  bgaBlurPass->init(static_cast<uint16_t>(width),
                    static_cast<uint16_t>(height));
  bgaBlurPass->setInputViews(
      std::vector<bgfx::ViewId>(rendering::kGameplayBgaInputViews.begin(),
                                rendering::kGameplayBgaInputViews.end()));
  bgaBlurPass->setCompositeEnabled(false);
  bgaBlurPass->setBlurStrength(settings.bgaBlurStrength);

  exportGeometry.applyExport();
  configureReplayExportRenderViews(width, height, outputFrameBuffer,
                                   *bgaBlurPass, settings);
  auto restoreExportRenderViews = [&]() {
    exportGeometry.applyExport();
    configureReplayExportRenderViews(width, height, outputFrameBuffer,
                                     *bgaBlurPass, settings);
  };
  auto allowUiFrame = [&]() {
    exportGeometry.restorePrimary();
    bool exportViewsRestored = false;
    bgfxAccess.allowUiFrame([&]() {
      restoreExportRenderViews();
      exportViewsRestored = true;
    });
    if (!exportViewsRestored) {
      restoreExportRenderViews();
    }
  };

  bms_parser::ChartMeta courseMeta =
      courseResultMetaForReplayVideo(replay, stages);
  RhythmState courseState = courseResultStateForReplayVideo(replay, stages);
  rendering::SimpleBatchRenderer courseResultGraphBatch;
  if (courseResultFrameCount > 0 && resolvedOptions.includeResultScreen) {
    courseResultRoot =
        std::make_unique<View>(0, 0, rendering::window_width,
                               rendering::window_height);
    ResultSkinData data = {&courseState, &courseMeta, &context};
    data.outGraphPlaceholder = &courseResultGraphPlaceholder;
    data.showControls = false;
    const play_options::PlayModeDisplayLabel display =
        play_options::formatPlayModeDisplayLabel(stages.back().replay);
    data.playModeLabel = display.mode;
    data.laneOrderLabel = display.laneOrder;
    data.difficultyLabel = "Course";
    data.headerDifficultyLabelOverride = "COURSE";
    const bool fullCombo = result_presentation::isFullComboCourseResult(
        replay.completedCharts, replay.totalCharts, stages.size(), courseState,
        courseMeta);
    const int clearRank = clear_policy::fullComboRankForPlayback(
        replay.clearType, fullCombo, replay.provenance.playback);
    data.currentClearLabelOverride = clearTypeRankToLabel(clearRank);
    data.currentClearRankOverride = clearRank;
    DefaultSkin resultSkin;
    resultSkin.buildLayout("Result", courseResultRoot.get(), &data);
    if (auto *analytics = courseResultRoot->findViewByName("timingAnalytics");
        analytics != nullptr) {
      analytics->setDisplay(YGDisplayNone);
    }
    courseResultRoot->applyYogaLayout();
  }

  ReplayAsyncFrameEncoder encoder;
  std::string errorMessage;
  if (!encoder.start(wavPath, outputPath, width, height, fps, frameBytes,
                     frameBufferCount, log, errorMessage)) {
    bgfxCleanup.runNow();
    return {
        .success = false, .outputPath = outputPath, .message = errorMessage};
  }

  const double durationSeconds =
      static_cast<double>(totalDurationMicros) / 1000000.0;
  const double rawFrameGiB =
      (static_cast<double>(frameBytes) * static_cast<double>(frameCount)) /
      static_cast<double>(1024ULL * 1024ULL * 1024ULL);
  const double frameBufferMemoryMiB =
      (static_cast<double>(replayVideoFrameBufferSlotBytes(width, height)) *
       static_cast<double>(frameBufferCount)) /
      static_cast<double>(1024ULL * 1024ULL);
  const double frameBufferBudgetMiB =
      static_cast<double>(replayVideoFrameBufferMemoryBudgetBytes()) /
      static_cast<double>(1024ULL * 1024ULL);
  replayExportLog(log,
                  "Course replay video export workload: %zu frames, %.2fs, "
                  "%.2f GiB BGRA readback",
                  frameCount, durationSeconds, rawFrameGiB);
  replayExportLog(log,
                  "Replay video export frame buffers/readbacks: %zu, encoder "
                  "threads: %d, queued memory: %.1f MiB / %.1f MiB budget",
                  frameBufferCount, encoder.videoThreadCount(),
                  frameBufferMemoryMiB, frameBufferBudgetMiB);

  RenderContext renderContext;
  uint32_t currentFrame = bgfx::frame();
  const auto exportStart = std::chrono::steady_clock::now();
  auto lastUiProgress =
      std::chrono::steady_clock::now() - std::chrono::milliseconds(500);
  long long bufferWaitMicros = 0;
  long long renderSubmitMicros = 0;
  long long readbackWaitMicros = 0;

  struct PendingReadback {
    size_t frameIndex = 0;
    size_t frameBufferIndex = 0;
    size_t readbackTextureIndex = 0;
    long long songTimeMicros = 0;
    uint32_t expectedFrame = 0;
  };
  std::deque<size_t> freeReadbackTextures;
  for (size_t i = 0; i < readbackTextures.size(); ++i) {
    freeReadbackTextures.push_back(i);
  }
  std::deque<PendingReadback> pendingReadbacks;

  auto drainOldestReadback = [&]() -> bool {
    if (pendingReadbacks.empty()) {
      return true;
    }

    PendingReadback pending = pendingReadbacks.front();
    pendingReadbacks.pop_front();
    const auto readbackWaitStart = std::chrono::steady_clock::now();
    while (currentFrame < pending.expectedFrame) {
      currentFrame = bgfx::frame();
    }
    readbackWaitMicros += elapsedMicros(readbackWaitStart);

    if (!encoder.submitFrame(pending.frameBufferIndex, pending.frameIndex,
                             pending.songTimeMicros, errorMessage)) {
      return false;
    }
    freeReadbackTextures.push_back(pending.readbackTextureIndex);
    return true;
  };

  auto drainReadyReadbacks = [&]() -> bool {
    while (!pendingReadbacks.empty() &&
           currentFrame >= pendingReadbacks.front().expectedFrame) {
      if (!drainOldestReadback()) {
        return false;
      }
    }
    return true;
  };

  auto videoPipelineProgress = [&](size_t renderedFrames,
                                   size_t encodedFrames) {
    if (frameCount == 0) {
      return 1.0;
    }
    const double completedUnits =
        static_cast<double>(std::min(renderedFrames, frameCount)) +
        static_cast<double>(std::min(encodedFrames, frameCount));
    return completedUnits / (static_cast<double>(frameCount) * 2.0);
  };

  auto reportVideoPipelineProgress = [&](size_t renderedFrames,
                                         size_t encodedFrames,
                                         const std::string &message) {
    const double progress =
        videoPipelineProgress(renderedFrames, encodedFrames);
    reportReplayExportProgress(options, 0.05 + 0.90 * progress, message,
                               std::min(renderedFrames, frameCount),
                               frameCount);
  };

  reportVideoPipelineProgress(0, 0, "Rendering / encoding video");
  auto maybeReportFrameProgress = [&](size_t renderedFrames, bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - lastUiProgress < std::chrono::milliseconds(250)) {
      return;
    }

    lastUiProgress = now;
    reportVideoPipelineProgress(renderedFrames, encoder.encodedFrames(),
                                "Rendering / encoding video");
    allowUiFrame();
  };

  auto renderAndQueueFrame = [&](size_t frameIndex, long long videoTimeMicros,
                                 auto &&renderFrame) -> bool {
    if (!drainReadyReadbacks()) {
      return false;
    }
    while (freeReadbackTextures.empty()) {
      if (!drainOldestReadback()) {
        return false;
      }
    }

    const auto bufferWaitStart = std::chrono::steady_clock::now();
    const int frameBufferIndex = encoder.acquireFrameBuffer(errorMessage);
    bufferWaitMicros += elapsedMicros(bufferWaitStart);
    if (frameBufferIndex < 0) {
      return false;
    }
    const size_t readbackTextureIndex = freeReadbackTextures.front();
    freeReadbackTextures.pop_front();

    const auto renderStart = std::chrono::steady_clock::now();
    renderFrame();
    bgfx::blit(rendering::readback_view, readbackTextures[readbackTextureIndex],
               0, 0, outputTexture);
    currentFrame = bgfx::frame();
    const uint32_t expectedFrame = bgfx::readTexture(
        readbackTextures[readbackTextureIndex],
        encoder.frameData(static_cast<size_t>(frameBufferIndex)));
    renderSubmitMicros += elapsedMicros(renderStart);
    pendingReadbacks.push_back(
        {.frameIndex = frameIndex,
         .frameBufferIndex = static_cast<size_t>(frameBufferIndex),
         .readbackTextureIndex = readbackTextureIndex,
         .songTimeMicros = videoTimeMicros,
         .expectedFrame = expectedFrame});
    maybeReportFrameProgress(frameIndex + 1, frameIndex + 1 == frameCount);
    return true;
  };

  size_t globalFrameIndex = 0;
  long long globalVideoTimeMicros = 0;
  long long finalStageVisualBaseMicros = 0;
  GameplayBgaMissStateTracker bgaMissTracker;
  std::uint64_t bgaFrameSerial = 2;
  replay_video_export::ReplayCourseMaximumComboPlayback
      courseMaximumComboPlayback;

  for (size_t stageIndex = 0; stageIndex < stages.size(); ++stageIndex) {
    auto &stage = stages[stageIndex];
    if (stage.chart == nullptr) {
      continue;
    }
    bgaMissTracker.reset();
    bms_parser::Chart &chart = *stage.chart;
    const ReplayData &stageReplay = stage.replay;

    if (!stage.gameplayPresentation.has_value()) {
      stage.gameplayPresentation.emplace();
    }
    if (const auto failure = preflightReplayGameplayPresentation(
            context, chart, stageReplay, settings, stage.preparationPlan,
            resolvedOptions, *stage.gameplayPresentation, log,
            stage.runtimeSkinSelection ? &*stage.runtimeSkinSelection
                                       : nullptr)) {
      bgfxCleanup.runNow();
      return *failure;
    }
    ReplayPlayfieldPresentation &presentation =
        *stage.gameplayPresentation->presentation;

    context.jukebox.setBgaDisplayMode(settings.bgaDisplayMode);
    context.jukebox.setVisualsEnabled(settings.bgaEnabled);
    context.jukebox.setEmbeddedBgaBrightnessPercent(
        settings.bgaBrightnessPercent);
    std::atomic_bool visualLoadCancelled = false;
    context.jukebox.loadVisuals(chart, visualLoadCancelled);
    if (visualLoadCancelled) {
      bgfxCleanup.runNow();
      return {.success = false,
              .outputPath = outputPath,
              .message = "Replay export visual loading was cancelled"};
    }

    const std::optional<ResultPreviousBestData> previousBest =
        result_presentation::previousBestForReplayChart(
            context.scoreRepository, chart.Meta, stageReplay);
    const auto bestScoreReplay =
        result_presentation::replayForPreviousBestChart(
            context, chart.Meta, previousBest, pacemaker::kTargetBest,
            visualLoadCancelled);
    const auto bestScoreAuthority =
        result_presentation::gameplayBestScoreAuthorityForReplay(
            chart, stageReplay, previousBest, bestScoreReplay.get());

    const auto playbackRateChangeTimelines =
        collectPlaybackRateChangeTimelines(chart);
    size_t playbackRateChangeCursor = 0;
    double currentExportBpm = chart.Meta.Bpm;
    double currentExportScrollRate = 1.0;
    auto applyExportPlaybackRate = [&](long long songTimeMicros) {
      while (playbackRateChangeCursor < playbackRateChangeTimelines.size() &&
             playbackRateChangeTimelines[playbackRateChangeCursor]->Timing <=
                 songTimeMicros) {
        const auto *timeline =
            playbackRateChangeTimelines[playbackRateChangeCursor];
        currentExportBpm = timeline->Bpm;
        currentExportScrollRate = timeline->Scroll;
        ++playbackRateChangeCursor;
      }
    };
    replay_video_export::ReplayLaneCoverPlayback laneCoverPlayback(
        settings.noteStartPositionPercent, settings.laneCoverEnabled);
    const long long visualOffsetMicros =
        static_cast<long long>(settings.visualOffsetMs) * 1000LL;
    const size_t gameplayFrameCount =
        framesForMicros(stage.gameplayDurationMicros);
    const size_t resultFrameCount =
        framesForMicros(stage.resultDurationMicros);
    size_t replayCursor = 0;
    replay_video_export::ReplayJudgementAuthorityPlayback
        replayJudgementAuthority;
    GaugeType replayGaugeType = stage.initialGaugeState.gaugeType;
    float replayGauge = stage.initialGaugeState.currentGauge;

    rendering::SimpleBatchRenderer stageResultGraphBatch;
    if (resultFrameCount > 0) {
      stageResultRoot =
          std::make_unique<View>(0, 0, rendering::window_width,
                                 rendering::window_height);
      ResultSkinData data = {&stage.resultState, &chart.Meta, &context};
      data.outGraphPlaceholder = &stageResultGraphPlaceholder;
      data.showControls = false;
      const play_options::PlayModeDisplayLabel display =
          play_options::formatPlayModeDisplayLabel(stageReplay);
      data.playModeLabel = display.mode;
      data.laneOrderLabel = display.laneOrder;
      data.difficultyLabel =
          result_presentation::difficultyLabelForChart(
              context.chartRepository, chart.Meta);
      data.currentClearLabelOverride = "NO PLAY";
      data.currentClearRankOverride = kNoClearTypeRank;
      data.previousBest = previousBest;
      DefaultSkin resultSkin;
      resultSkin.buildLayout("Result", stageResultRoot.get(), &data);
      stageResultAnalytics =
          addReplayResultAnalytics(*stageResultRoot, chart, stageReplay);
      stageResultAnalyticsMode = PracticeAnalyticsMode::Histogram;
      stageResultRoot->applyYogaLayout();
    }

    for (size_t frame = 0; frame < gameplayFrameCount; ++frame) {
      const long long stageRealTimeMicros = static_cast<long long>(
          (static_cast<long double>(frame) * 1000000.0L) / fps);
      const long long rawSongTimeMicros =
          stage.preparationPlan.chartTimeAtRealTime(stageRealTimeMicros);
      const auto frameTiming = gameplay_timing::frameTiming(
          rawSongTimeMicros, audioOffsetMicros, visualOffsetMicros);
      const auto presentationFrameState =
          replay_video_export::replayGameplayFrameState(
              stage.preparationPlan, chart, stageReplay, settings,
              bgaFrameSerial++, stageRealTimeMicros);
      while (replayCursor < stageReplay.events.size() &&
             stageReplay.events[replayCursor].songTimeMicros <=
                 frameTiming.gameplayTimeMicros) {
        const auto &event = stageReplay.events[replayCursor];
        const bool appliedHud = presentation.applyReplayEvent(
            event, makePlayfieldJudgeEventClock(event.songTimeMicros,
                                                visualOffsetMicros),
            true);
        if (event.judgement != None || event.action == ReplayEventAction::Mine ||
            event.action == ReplayEventAction::Gauge) {
          replayGaugeType = event.gaugeType;
          replayGauge = event.gauge;
        }
        if (appliedHud && event.judgement != None) {
          bgaMissTracker.onJudge(
              JudgeResult(event.judgement, event.diffMicros), event.combo,
              makePlayfieldJudgeEventClock(event.songTimeMicros,
                                           visualOffsetMicros));
          replayJudgementAuthority.recordApplied(event);
        }
        ++replayCursor;
      }
      applyExportPlaybackRate(frameTiming.gameplayTimeMicros);
      const auto laneCover = laneCoverPlayback.advance(
          stageReplay.laneCoverEvents, frameTiming.gameplayTimeMicros);
      for (const auto &transition : laneCover.transitions) {
        presentation.applyLaneCoverTransition(transition, currentExportBpm);
      }
      presentation.releaseDueClassicLongNoteTails(
          frameTiming.gameplayTimeMicros);
      presentation.applyAuthorityUpdate({
          .currentBpm = currentExportBpm,
          .currentScrollRate = currentExportScrollRate,
          .judgementCounters = replayJudgementAuthority.judgementCounters(),
          .judgementFastSlowCounters =
              replayJudgementAuthority.judgementFastSlowCounters(),
          .comboBreak = replayJudgementAuthority.comboBreak(),
          .maximumCombo = courseMaximumComboPlayback.observe(presentation),
          .bestScore = bestScoreAuthority.bestScore,
          .bestScoreTarget = bestScoreAuthority.bestScoreTarget,
          .gaugeType = replayGaugeType,
          .gaugeAutoShift = replay.gaugeAutoShift,
          .currentGauge = replayGauge,
          .gaugeRules = stage.resultState.gaugeRules(),
          .playOptionLabel = replayExportPlayOptionLabel(stageReplay),
          .autoPlayMarkVisible = stageReplay.autoPlay,
          .gameplayMode = PlayfieldGameplayMode::Replay,
          .loadingState = PlayfieldLoadingState::Loaded,
          .startLaneIndicators = stage.preparationPlan.laneIndicator.lanes,
          .startLaneIndicatorsVisible =
              stage.preparationPlan.indicatorVisibleAt(rawSongTimeMicros),
          .laneCoverPercent = laneCover.percent,
          .laneCoverEnabled = laneCover.enabled,
          .laneCoverChanged = false,
      });

      bool presentationFailed = false;
      if (!renderAndQueueFrame(globalFrameIndex, globalVideoTimeMicros,
                               [&]() {
                                 bgfx::touch(rendering::clear_view);
                                 bgfx::touch(rendering::bga_view);
                                 bgfx::touch(rendering::bga_layer_view);
                                 const auto presentationFrame =
                                     presentation.renderFrame(
                                         renderContext,
                                         presentationFrameState.clock,
                                         {.includeInvisibleNotes =
                                              settings.showInvisibleNotes});
                                 if (presentationFrame.outcome ==
                                         PresentationFrameOutcome::CriticalFailure ||
                                     presentationFrame.failure) {
                                   replay_video_export::releaseUnsubmittedReplayGameplayBga(
                                       context.jukebox, presentationFrame);
                                   errorMessage = presentationFrame.failure
                                                      ? replay_video_export::
                                                            skinExportFailureMessage(
                                                                *presentationFrame.failure)
                                                      : "Replay gameplay presentation failed "
                                                        "without a diagnostic "
                                                        "[presentation.frame_failure_missing]";
                                   presentationFailed = true;
                                   return;
                                 }
                                 if (presentationFrame.bgaCompositeMode ==
                                         GameplayBgaCompositeMode::FullscreenBuiltIn &&
                                     presentationFrame.preparedBga) {
                                   context.jukebox.submitFullscreen(
                                       *presentationFrame.preparedBga);
                                   bgaBlurPass->execute();
                                   rendering::renderFullscreenTextureTint(
                                       bgaBlurPass->outputTexture(),
                                       rendering::final_view,
                                       static_cast<float>(
                                           settings.bgaBrightnessPercent) /
                                           100.0f);
                                 }
                               })) {
        bgfxCleanup.runNow();
        return {.success = false,
                .outputPath = outputPath,
                .message = errorMessage};
      }
      if (presentationFailed) {
        bgfxCleanup.runNow();
        return {.success = false,
                .outputPath = outputPath,
                .message = errorMessage};
      }
      ++globalFrameIndex;
      globalVideoTimeMicros = static_cast<long long>(
          (static_cast<long double>(globalFrameIndex) * 1000000.0L) / fps);
    }

    for (size_t frame = 0; frame < resultFrameCount; ++frame) {
      const long long resultOffsetMicros = static_cast<long long>(
          (static_cast<long double>(frame) * 1000000.0L) / fps);
      const long long stageVideoMicros =
          stage.gameplayDurationMicros + resultOffsetMicros;
      const long long stageChartTimeMicros =
          stage.preparationPlan.chartTimeAtRealTime(stageVideoMicros);
      const long long stageBgaTimeMicros =
          gameplay_timing::gameplayTimeFromRawSongTime(
              stageChartTimeMicros, audioOffsetMicros);
      const auto analyticsMode =
          practice_analytics_presentation::analyticsModeForSlideshow(
              resultOffsetMicros, stage.resultDurationMicros);
      if (stageResultAnalytics != nullptr &&
          analyticsMode != stageResultAnalyticsMode) {
        stageResultAnalytics->setMode(analyticsMode);
        stageResultAnalyticsMode = analyticsMode;
      }
      if (!renderAndQueueFrame(globalFrameIndex, globalVideoTimeMicros,
                               [&]() {
                                 bgfx::touch(rendering::clear_view);
                                 bgfx::touch(rendering::bga_view);
                                 bgfx::touch(rendering::bga_layer_view);
                                 const auto bgaFrame =
                                     context.jukebox.prepareVisualFrameAt(
                                         bgaFrameSerial++, stageBgaTimeMicros,
                                         bgaMissTracker.snapshot());
                                 context.jukebox.submitFullscreen(bgaFrame);
                                 bgaBlurPass->execute();
                                 rendering::renderFullscreenTextureTint(
                                     bgaBlurPass->outputTexture(),
                                     rendering::final_view,
                                     static_cast<float>(
                                         settings.bgaBrightnessPercent) /
                                         100.0f);
                                 bgfx::touch(rendering::ui_view);
                                 if (stageResultRoot != nullptr) {
                                   stageResultRoot->render(renderContext);
                                   drawReplayResultGaugeGraph(
                                       stageResultGraphBatch,
                                       stage.resultState,
                                       stageResultGraphPlaceholder);
                                 }
                               })) {
        bgfxCleanup.runNow();
        return {.success = false,
                .outputPath = outputPath,
                .message = errorMessage};
      }
      ++globalFrameIndex;
      globalVideoTimeMicros = static_cast<long long>(
          (static_cast<long double>(globalFrameIndex) * 1000000.0L) / fps);
    }

    finalStageVisualBaseMicros =
        stage.gameplayDurationMicros + stage.resultDurationMicros;
    stageResultGraphPlaceholder = nullptr;
    stageResultAnalytics = nullptr;
    stageResultRoot.reset();
    if (stageIndex + 1 < stages.size() || courseResultFrameCount == 0) {
      context.jukebox.stop();
      context.jukebox.unloadVisuals();
    }
    replay_video_export::destroyReplayGameplayPresentation(
        context.rendererAccess, stage.gameplayPresentation->presentation);
    stage.gameplayPresentation.reset();
  }

  for (size_t frame = 0; frame < courseResultFrameCount; ++frame) {
    const long long resultOffsetMicros = static_cast<long long>(
        (static_cast<long double>(frame) * 1000000.0L) / fps);
    const long long stageVideoMicros =
        finalStageVisualBaseMicros + resultOffsetMicros;
    const long long stageChartTimeMicros =
        stages.back().preparationPlan.chartTimeAtRealTime(stageVideoMicros);
    const long long stageBgaTimeMicros =
        gameplay_timing::gameplayTimeFromRawSongTime(
            stageChartTimeMicros, audioOffsetMicros);
    if (!renderAndQueueFrame(globalFrameIndex, globalVideoTimeMicros,
                             [&]() {
                               bgfx::touch(rendering::clear_view);
                               bgfx::touch(rendering::bga_view);
                               bgfx::touch(rendering::bga_layer_view);
                               const auto bgaFrame =
                                   context.jukebox.prepareVisualFrameAt(
                                       bgaFrameSerial++, stageBgaTimeMicros,
                                       bgaMissTracker.snapshot());
                               context.jukebox.submitFullscreen(bgaFrame);
                               bgaBlurPass->execute();
                               rendering::renderFullscreenTextureTint(
                                   bgaBlurPass->outputTexture(),
                                   rendering::final_view,
                                   static_cast<float>(
                                       settings.bgaBrightnessPercent) /
                                       100.0f);
                               bgfx::touch(rendering::ui_view);
                               if (courseResultRoot != nullptr) {
                                 courseResultRoot->render(renderContext);
                                 drawReplayResultGaugeGraph(
                                     courseResultGraphBatch, courseState,
                                     courseResultGraphPlaceholder);
                               }
                             })) {
      bgfxCleanup.runNow();
      return {.success = false,
              .outputPath = outputPath,
              .message = errorMessage};
    }
    ++globalFrameIndex;
    globalVideoTimeMicros = static_cast<long long>(
        (static_cast<long double>(globalFrameIndex) * 1000000.0L) / fps);
  }

  while (!pendingReadbacks.empty()) {
    if (!drainOldestReadback()) {
      bgfxCleanup.runNow();
      return {.success = false,
              .outputPath = outputPath,
              .message = errorMessage};
    }
  }

  bgfxCleanup.runNow();
  bgfxAccess.release();
  auto result = encoder.finish([&](size_t encodedFrames) {
    reportVideoPipelineProgress(frameCount, encodedFrames, "Encoding video");
  });
  if (result.success) {
    reportReplayExportProgress(options, 0.97, "Finalizing video", frameCount,
                               frameCount);
  }
  if (!result.success && result.outputPath.empty()) {
    result.outputPath = outputPath;
  }
  replayExportLog(log,
                  "Course replay video export profile: %.2fs total, %.2fs "
                  "render submit, %.2fs readback wait, %.2fs encode worker, "
                  "%.2fs waiting for frame buffers",
                  static_cast<double>(elapsedMicros(exportStart)) / 1000000.0,
                  static_cast<double>(renderSubmitMicros) / 1000000.0,
                  static_cast<double>(readbackWaitMicros) / 1000000.0,
                  static_cast<double>(encoder.encodedMicros()) / 1000000.0,
                  static_cast<double>(bufferWaitMicros) / 1000000.0);
  replayExportLog(log,
                  "Replay video encoder profile: %.2fs audio, %.2fs frame "
                  "prepare, %.2fs pixel convert, %.2fs video encode/write",
                  static_cast<double>(encoder.audioEncodeMicros()) / 1000000.0,
                  static_cast<double>(encoder.framePrepareMicros()) / 1000000.0,
                  static_cast<double>(encoder.pixelConvertMicros()) / 1000000.0,
                  static_cast<double>(encoder.videoEncodeMicros()) / 1000000.0);
  replayExportLog(log,
                  "Replay video encoder detail: %.2fs send, %.2fs receive, "
                  "%.2fs mux write, %.2fs stalls, %.2fs flush, packets=%lld, "
                  "stall retries=%d",
                  static_cast<double>(encoder.videoSendMicros()) / 1000000.0,
                  static_cast<double>(encoder.videoReceiveMicros()) /
                      1000000.0,
                  static_cast<double>(encoder.videoWriteMicros()) / 1000000.0,
                  static_cast<double>(encoder.videoStallMicros()) / 1000000.0,
                  static_cast<double>(encoder.videoFlushMicros()) / 1000000.0,
                  encoder.videoPackets(), encoder.videoStalls());
  replayExportLog(log,
                  "Replay video frame buffer detail: max refcount=%d, COW "
                  "copies=%lld",
                  encoder.maxVideoFrameRefCount(),
                  encoder.videoFrameCowCopies());
  return result;
}

ReplayVideoExportResult
saveReplayVideoToPlatformLibrary(const ReplayVideoExportResult &muxResult) {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  std::string errorMessage;
  if (!SaveVideoToIOSPhotos(fspath_to_utf8(muxResult.outputPath),
                            errorMessage)) {
    return {.success = false,
            .outputPath = muxResult.outputPath,
            .message = errorMessage.empty() ? "Failed to save video to Photos"
                                            : errorMessage};
  }
  return {.success = true,
          .outputPath = muxResult.outputPath,
          .message = "Saved to Photos"};
#else
  return muxResult;
#endif
}
} // namespace

ReplayVideoExportResult
ReplayVideoExporter::Export(ApplicationContext &context,
                            bms_parser::Chart *chart, const ReplayData &replay,
                            const ReplayVideoExportOptions &options) {
  if (chart == nullptr) {
    return {.success = false, .message = "No chart selected"};
  }
  reportReplayExportProgress(options, 0.0, "Preparing export");

  const auto resolvedOptions = resolveReplayVideoExportOptions(options);
  const preparation::Plan preparationPlan = preparation::buildNormalPlan(
      *chart, context.settings.startLaneIndicatorsEnabled,
      context.settings.prepMetronomeEnabled, 0, 0, std::nullopt,
      replay.provenance.playback);
  PreparedReplayGameplayPresentation preparedGameplay;
  return replay_video_export::runPreflightGatedNormalExport(
      [&]() -> std::optional<ReplayVideoExportResult> {
        return preflightReplayGameplayPresentation(
            context, *chart, replay, context.settings, preparationPlan,
            resolvedOptions, preparedGameplay, nullptr);
      },
      [&]() -> ReplayVideoExportResult {
  auto gameplayPresentationCleanup = makeScopeExit([&]() {
    replay_video_export::destroyReplayGameplayPresentation(
        context.rendererAccess, preparedGameplay.presentation);
  });

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  reportReplayExportProgress(options, 0.01, "Requesting Photos permission");
  std::string photosErrorMessage;
  if (!RequestIOSPhotoAddAuthorization(photosErrorMessage)) {
    return {.success = false,
            .message = photosErrorMessage.empty()
                           ? "Photos permission was not granted"
                           : photosErrorMessage};
  }
#endif

  const auto outputDir = Utils::GetDocumentsPath("video_exports");
  if (const auto error = ensureReplayExportDirectoryError(
          outputDir, "Failed to create replay export directory")) {
    return {.success = false, .message = *error};
  }

  const std::string baseName =
      sanitizeFileNamePart(chart->Meta.Title) + "_" + makeTimestamp();
  const auto tempDir = outputDir / (baseName + "_tmp");
  const auto logPath = outputDir / (baseName + ".log");
  ReplayVideoExportLog exportLogFile(logPath);
  ReplayVideoExportLog *exportLog = &exportLogFile;
  replayExportLog(exportLog, "Replay export log: %s",
                  fspath_to_utf8(logPath).c_str());
  replayExportLog(exportLog, "Replay export chart: %s",
                  chart->Meta.Title.c_str());
  if (replay.randomSeed.has_value()) {
    replayExportLog(exportLog, "Replay export random seed: %u",
                    *replay.randomSeed);
  }
  if (replay.randomPrng.has_value()) {
    replayExportLog(exportLog, "Replay export random PRNG: %s",
                    replay.randomPrng->c_str());
  }
  if (replay.playOption.has_value()) {
    replayExportLog(exportLog, "Replay export play option: %s",
                    replay.playOption->c_str());
  }
  if (replay.playOptionSeed.has_value()) {
    replayExportLog(exportLog, "Replay export play option seed: %lld",
                    *replay.playOptionSeed);
  }
  if (replay.playOption2.has_value()) {
    replayExportLog(exportLog, "Replay export 2P play option: %s",
                    replay.playOption2->c_str());
  }
  if (replay.playOption2Seed.has_value()) {
    replayExportLog(exportLog, "Replay export 2P play option seed: %lld",
                    *replay.playOption2Seed);
  }
  const auto totalStart = std::chrono::steady_clock::now();

  if (const auto error = ensureReplayExportDirectoryError(
          tempDir, "Failed to create replay export work directory")) {
    replayExportLog(exportLog, "Replay export failed to create work directory: %s",
                    error->c_str());
    return {.success = false, .message = *error};
  }

  const auto wavPath = tempDir / "audio.wav";
  const auto outputPath = outputDir / (baseName + ".mp4");
  const long long audioOffsetMicros =
      static_cast<long long>(context.settings.audioOffsetMs) * 1000LL;

  replayExportLog(exportLog, "Replay export audio: %s",
                  fspath_to_utf8(wavPath).c_str());
  reportReplayExportProgress(resolvedOptions, 0.02, "Building audio track");
  const auto audioStart = std::chrono::steady_clock::now();
  auto audioResult = writeReplayAudioTrack(
      *chart, replay, preparationPlan, audioOffsetMicros, wavPath, exportLog);
  if (!audioResult.success) {
    replayExportLog(exportLog, "Replay export audio failed: %s",
                    audioResult.message.c_str());
    removeReplayExportWorkDirectory(tempDir);
    return {.success = false,
            .outputPath = audioResult.outputPath,
            .message = audioResult.message};
  }
  replayExportLog(exportLog, "Replay export audio finished in %.2fs",
                  static_cast<double>(elapsedMicros(audioStart)) / 1000000.0);
  reportReplayExportProgress(resolvedOptions, 0.05, "Audio track ready");

  replayExportLog(exportLog, "Replay export MP4: %s (%dx%d @ %dfps)",
                  fspath_to_utf8(outputPath).c_str(), resolvedOptions.width,
                  resolvedOptions.height, resolvedOptions.fps);
  const auto videoStart = std::chrono::steady_clock::now();
  const auto failureMicros = replay_result::FindGaugeFailureMicros(
      *chart, replay, GaugeProfile::Standard);
  const auto rawFailureMicros = gameplay_timing::rawSongTimeFromGameplayTime(
      failureMicros, audioOffsetMicros);
  const long long normalGameplayDurationMicros =
      preparationPlan.realTimeAtGameplayTime(
          gameplayEndMicrosForReplay(*chart, replay), audioOffsetMicros) +
      chart_playback_duration::kGameplayResultTransitionDelayMicros;
  const long long failureAudioMicros =
      rawFailureMicros.has_value()
          ? preparationPlan.realTimeAtChartTime(*rawFailureMicros)
          : normalGameplayDurationMicros;
  const long long failureFrameMicros =
      (1000000LL + resolvedOptions.fps - 1) / resolvedOptions.fps;
  const long long gameplayDurationMicros =
      failureMicros.has_value()
          ? std::min(normalGameplayDurationMicros,
                     failureAudioMicros + failureFrameMicros)
          : normalGameplayDurationMicros;
  const long long requestedAudioDurationMicros =
      failureMicros.has_value()
          ? std::min(audioResult.durationMicros, failureAudioMicros)
          : audioResult.durationMicros;
  auto muxResult = renderReplayVideoToMp4(
      context, *chart, replay, context.settings, preparationPlan,
      preparedGameplay, resolvedOptions, wavPath, outputPath,
      gameplayDurationMicros,
      requestedAudioDurationMicros, failureMicros.has_value(), exportLog);
  if (!muxResult.success) {
    replayExportLog(exportLog, "Replay export MP4 failed: %s",
                    muxResult.message.c_str());
    removeReplayExportOutputFile(outputPath);
    removeReplayExportWorkDirectory(tempDir);
    return muxResult;
  }
  replayExportLog(exportLog, "Replay export MP4 finished in %.2fs",
                  static_cast<double>(elapsedMicros(videoStart)) / 1000000.0);

  removeReplayExportWorkDirectory(tempDir, exportLog);

  reportReplayExportProgress(resolvedOptions, 0.99, "Saving video");
  auto platformSaveResult = saveReplayVideoToPlatformLibrary(muxResult);
  if (!platformSaveResult.success) {
    replayExportLog(exportLog, "Replay export platform save failed: %s",
                    platformSaveResult.message.c_str());
    return platformSaveResult;
  }
  reportReplayExportProgress(resolvedOptions, 1.0, platformSaveResult.message);
  replayExportLog(exportLog, "Replay export finished in %.2fs: %s",
                  static_cast<double>(elapsedMicros(totalStart)) / 1000000.0,
                  platformSaveResult.message.c_str());
  return platformSaveResult;
      });
}

namespace {

bool sameGaugeSnapshot(const GaugeStateSnapshot &left,
                       const GaugeStateSnapshot &right) noexcept {
  return left.gaugeType == right.gaugeType &&
         left.selectedGaugeType == right.selectedGaugeType &&
         left.gaugeAutoShiftLowerBound == right.gaugeAutoShiftLowerBound &&
         left.gaugeProfile == right.gaugeProfile &&
         left.gaugeAutoShift == right.gaugeAutoShift &&
         left.currentGauge == right.currentGauge &&
         left.gaugeValues == right.gaugeValues &&
         left.gaugeSurvivalFailed == right.gaugeSurvivalFailed;
}

using CourseMaterializedStages =
    std::vector<replay::CourseReplayMaterializedStage>;

ReplayVideoExportResult exportCourseReplayImpl(
    ApplicationContext &context, const CourseReplayData &replay,
    std::vector<std::unique_ptr<bms_parser::Chart>> *preparedCharts,
    const CourseMaterializedStages *materializedStages,
    const ReplayVideoExportOptions &options) {
  if (replay.stages.empty() ||
      (preparedCharts != nullptr &&
       preparedCharts->size() != replay.stages.size()) ||
      (materializedStages != nullptr &&
       materializedStages->size() != replay.stages.size())) {
    return {.success = false, .message = "No course replay selected"};
  }
  reportReplayExportProgress(options, 0.0, "Preparing course export");
  const auto resolvedOptions = resolveReplayVideoExportOptions(options);
  std::vector<CourseReplayVideoStage> stages;
  stages.reserve(replay.stages.size());
  std::optional<GaugeStateSnapshot> carriedGauge;
  const long long audioOffsetMicros =
      static_cast<long long>(context.settings.audioOffsetMs) * 1000LL;

  for (size_t i = 0; i < replay.stages.size(); ++i) {
    const ReplayData &stageReplay = replay.stages[i].replay;
    reportReplayExportProgress(
        resolvedOptions,
        0.02 + 0.03 * (static_cast<double>(i) /
                       static_cast<double>(replay.stages.size())),
        "Preparing course stage " + std::to_string(i + 1));
    std::atomic_bool parseCancelled = false;
    std::unique_ptr<bms_parser::Chart> chart;
    if (preparedCharts != nullptr) {
      chart = std::move((*preparedCharts)[i]);
    } else {
      chart = play_options::prepareReplayChart(
          stageReplay.chartMeta.BmsPath, stageReplay, parseCancelled);
    }
    if (chart == nullptr || parseCancelled) {
      return {.success = false, .message = "Failed to load course replay stage"};
    }

    preparation::Plan stagePreparationPlan = preparation::buildNormalPlan(
        *chart, context.settings.startLaneIndicatorsEnabled,
        context.settings.prepMetronomeEnabled, 0, 0, std::nullopt,
        course_rules::kRequiredPlaybackRate);

    const long long resultDurationMicros =
        resolvedOptions.includeResultScreen
            ? std::max(0LL, replay.stages[i].restMicrosAfterStage)
            : 0LL;
    ReplayData configuredStageReplay = stageReplay;
    configuredStageReplay.initialGaugeType = replay.initialGaugeType;
    configuredStageReplay.gaugeAutoShift = replay.gaugeAutoShift;
    configuredStageReplay.gaugeAutoShiftLowerBound =
        replay.gaugeAutoShiftLowerBound;
    const GaugeStateSnapshot *carriedGaugeState = nullptr;
    if (materializedStages != nullptr && i > 0) {
      carriedGaugeState = &(*materializedStages)[i].initialGaugeState;
    } else if (materializedStages == nullptr && carriedGauge.has_value()) {
      carriedGaugeState = &*carriedGauge;
    }
    const auto failureMicros = replay_result::FindGaugeFailureMicros(
        *chart, configuredStageReplay, replay.gaugeProfile,
        carriedGaugeState);
    ReplayData exportStageReplay =
        replayThroughFailure(configuredStageReplay, failureMicros);
    const RhythmState initialGaugeState =
        replay_result::BuildInitialGaugeState(
            *chart, exportStageReplay, replay.gaugeProfile,
            carriedGaugeState);
    RhythmState resultState = replay_result::BuildResultState(
        *chart, exportStageReplay, replay.gaugeProfile,
        carriedGaugeState);
    const GaugeStateSnapshot renderedInitial =
        initialGaugeState.gaugeSnapshot();
    const GaugeStateSnapshot renderedFinal = resultState.gaugeSnapshot();
    if (materializedStages != nullptr &&
        (!sameGaugeSnapshot(
             renderedInitial,
             (*materializedStages)[i].initialGaugeState) ||
         !sameGaugeSnapshot(renderedFinal,
                            (*materializedStages)[i].finalGaugeState))) {
      return {.success = false,
              .message =
                  "Course video state disagrees with verified continuation"};
    }
    carriedGauge = materializedStages != nullptr
                       ? (*materializedStages)[i].finalGaugeState
                       : renderedFinal;
    stages.emplace_back(
        std::move(chart), std::move(exportStageReplay),
        std::move(stagePreparationPlan),
        initialGaugeState.gaugeSnapshot(), std::move(resultState),
        failureMicros, 0, resultDurationMicros, 0);
  }

  if (const auto failure = preflightCourseReplayGameplayPresentations(
          context, stages, context.settings, resolvedOptions, nullptr)) {
    return *failure;
  }
  auto gameplayPresentationCleanup = makeScopeExit([&]() {
    for (auto &stage : stages) {
      if (stage.gameplayPresentation.has_value()) {
        replay_video_export::destroyReplayGameplayPresentation(
            context.rendererAccess,
            stage.gameplayPresentation->presentation);
      }
    }
  });

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  reportReplayExportProgress(options, 0.01, "Requesting Photos permission");
  std::string photosErrorMessage;
  if (!RequestIOSPhotoAddAuthorization(photosErrorMessage)) {
    return {.success = false,
            .message = photosErrorMessage.empty()
                           ? "Photos permission was not granted"
                           : photosErrorMessage};
  }
#endif

  const auto outputDir = Utils::GetDocumentsPath("video_exports");
  if (const auto error = ensureReplayExportDirectoryError(
          outputDir, "Failed to create replay export directory")) {
    return {.success = false, .message = *error};
  }

  const std::string baseName =
      sanitizeFileNamePart(replay.courseName.empty() ? "Course Replay"
                                                     : replay.courseName) +
      "_" + makeTimestamp();
  const auto tempDir = outputDir / (baseName + "_tmp");
  const auto wavPath = tempDir / "audio.wav";
  const auto outputPath = outputDir / (baseName + ".mp4");
  const auto logPath = outputDir / (baseName + ".log");
  ReplayVideoExportLog exportLogFile(logPath);
  ReplayVideoExportLog *exportLog = &exportLogFile;
  replayExportLog(exportLog, "Course replay export log: %s",
                  fspath_to_utf8(logPath).c_str());
  replayExportLog(exportLog, "Course replay export course: %s",
                  replay.courseName.c_str());
  replayExportLog(exportLog, "Course replay export stages: %zu",
                  replay.stages.size());
  const auto totalStart = std::chrono::steady_clock::now();

  if (const auto error = ensureReplayExportDirectoryError(
          tempDir, "Failed to create replay export work directory")) {
    return {.success = false, .message = *error};
  }

  std::vector<CourseReplayAudioSegment> audioSegments;
  audioSegments.reserve(stages.size());
  for (size_t i = 0; i < stages.size(); ++i) {
    auto &stage = stages[i];
    const auto stageWavPath =
        tempDir / ("stage_" + std::to_string(i + 1) + ".wav");
    const auto audioResult = writeReplayAudioTrack(
        *stage.chart, stage.replay, stage.preparationPlan, audioOffsetMicros,
        stageWavPath, exportLog);
    if (!audioResult.success) {
      removeReplayExportWorkDirectory(tempDir);
      return {.success = false,
              .outputPath = audioResult.outputPath,
              .message = audioResult.message};
    }
    const long long normalGameplayDurationMicros =
        courseStageGameplayDurationMicrosForReplay(
            *stage.chart, stage.replay, audioResult.durationMicros,
            resolvedOptions.includeResultScreen, stage.preparationPlan,
            audioOffsetMicros);
    const long long failureFrameMicros =
        (1000000LL + resolvedOptions.fps - 1) / resolvedOptions.fps;
    const auto rawFailureMicros = gameplay_timing::rawSongTimeFromGameplayTime(
        stage.failureMicros, audioOffsetMicros);
    stage.gameplayDurationMicros = stage.failureMicros.has_value()
                                      ? std::min(
                                            normalGameplayDurationMicros,
                                            stage.preparationPlan.realTimeAtChartTime(
                                                *rawFailureMicros) +
                                                failureFrameMicros)
                                      : normalGameplayDurationMicros;
    stage.gameplayDurationMicros =
        replay_video_export::replayGameplayDurationWithSkinTiming(
            *stage.chart, stage.replay, stage.preparationPlan,
            audioOffsetMicros, resolvedOptions.fps,
            stage.gameplayDurationMicros, stage.failureMicros.has_value(),
            stage.selectedSkinTiming);
    const long long audioContentDurationMicros =
        stage.failureMicros.has_value()
            ? std::min(audioResult.durationMicros,
                       stage.preparationPlan.realTimeAtChartTime(
                           *rawFailureMicros))
            : stage.gameplayDurationMicros + stage.resultDurationMicros;
    stage.audioDurationMicros = stage.failureMicros.has_value()
                                    ? audioContentDurationMicros
                                    : audioResult.durationMicros;
    audioSegments.push_back(CourseReplayAudioSegment{
        .wavPath = stageWavPath,
        .durationMicros = stage.gameplayDurationMicros +
                          stage.resultDurationMicros,
        .contentDurationMicros = audioContentDurationMicros,
    });
  }

  if (!audioSegments.empty()) {
    audioSegments.back().durationMicros +=
        courseResultDurationMicrosForReplayVideo(
            stages, resolvedOptions.includeResultScreen);
  }

  reportReplayExportProgress(resolvedOptions, 0.05, "Building course audio");
  const auto audioStart = std::chrono::steady_clock::now();
  const auto courseAudioResult =
      writeCourseReplayAudioTrack(audioSegments, wavPath, exportLog);
  if (!courseAudioResult.success) {
    removeReplayExportWorkDirectory(tempDir);
    return {.success = false,
            .outputPath = courseAudioResult.outputPath,
            .message = courseAudioResult.message};
  }
  replayExportLog(exportLog, "Course replay export audio finished in %.2fs",
                  static_cast<double>(elapsedMicros(audioStart)) / 1000000.0);

  replayExportLog(exportLog, "Course replay export MP4: %s (%dx%d @ %dfps)",
                  fspath_to_utf8(outputPath).c_str(), resolvedOptions.width,
                  resolvedOptions.height, resolvedOptions.fps);
  const auto videoStart = std::chrono::steady_clock::now();
  auto muxResult = renderCourseReplayVideoToMp4(
      context, replay, stages, context.settings, resolvedOptions, wavPath,
      outputPath, exportLog);
  if (!muxResult.success) {
    replayExportLog(exportLog, "Course replay export MP4 failed: %s",
                    muxResult.message.c_str());
    removeReplayExportOutputFile(outputPath);
    removeReplayExportWorkDirectory(tempDir);
    return muxResult;
  }
  replayExportLog(exportLog, "Course replay export MP4 finished in %.2fs",
                  static_cast<double>(elapsedMicros(videoStart)) / 1000000.0);

  removeReplayExportWorkDirectory(tempDir, exportLog);

  reportReplayExportProgress(resolvedOptions, 0.99, "Saving video");
  auto platformSaveResult = saveReplayVideoToPlatformLibrary(muxResult);
  if (!platformSaveResult.success) {
    replayExportLog(exportLog, "Course replay export platform save failed: %s",
                    platformSaveResult.message.c_str());
    return platformSaveResult;
  }
  reportReplayExportProgress(resolvedOptions, 1.0, platformSaveResult.message);
  replayExportLog(exportLog, "Course replay export finished in %.2fs: %s",
                  static_cast<double>(elapsedMicros(totalStart)) / 1000000.0,
                  platformSaveResult.message.c_str());
  return platformSaveResult;
}

} // namespace

ReplayVideoExportResult
ReplayVideoExporter::ExportCourseReplay(ApplicationContext &context,
                                        const CourseReplayData &replay,
                                        const ReplayVideoExportOptions &options) {
  return exportCourseReplayImpl(context, replay, nullptr, nullptr, options);
}

ReplayVideoExportResult ReplayVideoExporter::ExportCourseReplay(
    ApplicationContext &context,
    replay::CourseReplayConsumerOutcome &&verified,
    const ReplayVideoExportOptions &options) {
  if (!verified.ready()) {
    return {.success = false, .message = "No verified course replay selected"};
  }
  return exportCourseReplayImpl(context, *verified.replayData,
                                &verified.charts,
                                &verified.materializedStages, options);
}
