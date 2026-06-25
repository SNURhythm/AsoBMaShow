#include "ReplayVideoExporter.h"

#include "ArchiveFile.h"
#include "ChartDBHelper.h"
#include "PlayOptionUtils.h"
#include "RAII.h"
#include "ReplayResultStateBuilder.h"
#include "ScoreDBHelper.h"
#include "Utils.h"
#include "audio/decoder.h"
#include "main.h"
#include "path.h"
#include "rendering/BlurPass.h"
#include "rendering/Color.h"
#include "rendering/RenderPlan.h"
#include "rendering/SimpleBatchRenderer.h"
#include "rendering/common.h"
#include "scene/play/BMSRenderer.h"
#include "scene/play/Judge.h"
#include "skin/DefaultSkin.h"
#include "view/UiTheme.h"
#include "view/View.h"
#include "targets.h"
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
#include "iOSNatives.hpp"
#endif

#include <SDL2/SDL.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <sndfile.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
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
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr int kExportSampleRate = 44100;
constexpr int kExportChannels = 2;
constexpr int kDefaultExportFps = 120;
constexpr int kH264HighProfile = 100;
constexpr long long kAudioTailMicros = 3000000;
constexpr long long kResultSceneTailMicros = 10000000;
const std::array<std::string, 4> kAudioExtensions = {"flac", "wav", "ogg",
                                                     "mp3"};

struct AudioEvent {
  long long timeMicros = 0;
  int wav = bms_parser::Parser::NoWav;
};

struct DecodedSound {
  std::vector<short> pcm;
  SF_INFO info{};
};

using DecodedSoundCache =
    std::unordered_map<int, std::shared_ptr<DecodedSound>>;

struct ReplayArchiveAudioBatch {
  std::filesystem::path archivePath;
  std::vector<std::filesystem::path> innerPaths;
  std::unordered_map<path_t, std::vector<int>> wavIdsByPath;
};

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

size_t replayVideoFrameBufferCount(int width, int height) {
  constexpr size_t kMaxFrameBufferMemoryBytes = 128ULL * 1024ULL * 1024ULL;
  const size_t frameBytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4ULL;
  const size_t memoryLimitedBuffers =
      frameBytes == 0 ? 3 : kMaxFrameBufferMemoryBytes / frameBytes;
  const auto hardwareThreads = std::thread::hardware_concurrency();
  if (hardwareThreads <= 2) {
    return std::clamp(memoryLimitedBuffers, static_cast<size_t>(3),
                      static_cast<size_t>(4));
  }
  const size_t hardwareLimitedBuffers =
      std::clamp(static_cast<size_t>(hardwareThreads), static_cast<size_t>(4),
                 static_cast<size_t>(8));
  return std::clamp(std::min(memoryLimitedBuffers, hardwareLimitedBuffers),
                    static_cast<size_t>(3), static_cast<size_t>(8));
}

long long elapsedMicros(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
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
              path.string().c_str());
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
  } else {
    SDL_Log("%s", message.c_str());
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

void applyReplayVideoRenderGeometry(int exportWidth, int exportHeight) {
  rendering::render_width = std::max(1, exportWidth);
  rendering::render_height = std::max(1, exportHeight);

  // Rendering below the 1920px design width used to make 1px UI strokes and
  // world-space line accents subpixel-thin. Keep export UI scale at least 1:1
  // so 1080p exports on narrower display aspects do not drop thin lines.
  const float uiScale = std::max(
      1.0f, static_cast<float>(rendering::render_width) /
                static_cast<float>(rendering::design_width));
  rendering::ui_scale_x = uiScale;
  rendering::ui_scale_y = uiScale;
  rendering::widthScale = uiScale;
  rendering::heightScale = uiScale;
  rendering::window_width = std::max(
      1, static_cast<int>(
             std::lround(static_cast<float>(rendering::render_width) /
                         uiScale)));
  rendering::window_height = std::max(
      1, static_cast<int>(
             std::lround(static_cast<float>(rendering::render_height) /
                         uiScale)));
  rendering::ui_view_width = rendering::render_width;
  rendering::ui_view_height = rendering::render_height;
  rendering::ui_offset_x = 0;
  rendering::ui_offset_y = 0;
}

class ScopedReplayVideoRenderGeometry {
public:
  ScopedReplayVideoRenderGeometry(int exportWidth, int exportHeight)
      : exportWidth(exportWidth), exportHeight(exportHeight) {}

  ~ScopedReplayVideoRenderGeometry() { restorePrimary(); }

  void applyExport() {
    applyReplayVideoRenderGeometry(exportWidth, exportHeight);
  }

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
      : context(context), lock(context.bgfxRenderMutex, std::defer_lock) {
    context.replayVideoExportActive.store(true, std::memory_order_release);
    lock.lock();
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
    context.replayVideoExportActive.store(false, std::memory_order_release);
    if (lock.owns_lock()) {
      lock.unlock();
    }
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
    lock.unlock();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(16);
    while (!context.quitFlag.load(std::memory_order_acquire) &&
           context.replayVideoExportUiFrameSerial.load(
               std::memory_order_acquire) == previousFrame &&
           std::chrono::steady_clock::now() < deadline) {
      SDL_Delay(1);
    }

    lock.lock();
    context.replayVideoExportUiFrameRequested.store(false,
                                                    std::memory_order_release);
    restoreExportViews();
  }

  ScopedReplayVideoBgfxAccess(const ScopedReplayVideoBgfxAccess &) = delete;
  ScopedReplayVideoBgfxAccess &
  operator=(const ScopedReplayVideoBgfxAccess &) = delete;

private:
  ApplicationContext &context;
  std::unique_lock<std::mutex> lock;
  uint32_t originalResetFlags = 0;
  bool restoreResetFlags = false;
  bool released = false;
};

std::string replayNoteKey(int lane, long long noteTimeMicros) {
  return std::to_string(lane) + ":" + std::to_string(noteTimeMicros);
}

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

std::optional<std::filesystem::path>
resolveSoundPath(const bms_parser::Chart &chart, int wav) {
  const auto wavIt = chart.WavTable.find(wav);
  if (wavIt == chart.WavTable.end()) {
    return std::nullopt;
  }

  const std::filesystem::path basePath = chart.Meta.Folder / wavIt->second;
  std::vector<std::string_view> extensions;
  extensions.reserve(kAudioExtensions.size());
  for (const auto &ext : kAudioExtensions) {
    extensions.emplace_back(ext);
  }
  return archive_file::findFileWithExtensions(basePath, extensions);
}

std::string replayExportPlayOptionLabel(const ReplayData &replay) {
  const std::string label = play_options::formatPlayOptionLabel(
      replay.playOption, replay.playOptionSeed, replay.playOption2,
      replay.playOption2Seed);
  return label.empty() ? "" : "Option: " + label;
}

std::optional<archive_file::EntryRange>
entryRangeForReplayChartArchive(const bms_parser::Chart &chart,
                                const std::filesystem::path &archivePath) {
  std::filesystem::path chartArchivePath;
  std::filesystem::path chartInnerPath;
  if (!archive_file::splitVirtualPath(chart.Meta.BmsPath, chartArchivePath,
                                      chartInnerPath)) {
    return std::nullopt;
  }
  if (fspath_to_path_t(chartArchivePath.lexically_normal()) !=
      fspath_to_path_t(archivePath.lexically_normal())) {
    return std::nullopt;
  }
  return archive_file::entryRangeForFolder(chart.Meta.Folder);
}

bool addReplayArchiveAudioTarget(
    std::unordered_map<path_t, ReplayArchiveAudioBatch> &batches,
    std::vector<path_t> &batchOrder, const std::filesystem::path &path,
    int wav) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!archive_file::splitVirtualPath(path, archivePath, innerPath)) {
    return false;
  }

  const path_t archiveKey = fspath_to_path_t(archivePath);
  auto batchIt = batches.find(archiveKey);
  if (batchIt == batches.end()) {
    batchOrder.push_back(archiveKey);
    batchIt =
        batches
            .emplace(archiveKey, ReplayArchiveAudioBatch{
                                     .archivePath = archivePath,
                                     .innerPaths = {},
                                     .wavIdsByPath = {},
                                 })
            .first;
  }

  const path_t pathKey = fspath_to_path_t(path);
  auto &wavIds = batchIt->second.wavIdsByPath[pathKey];
  if (wavIds.empty()) {
    batchIt->second.innerPaths.push_back(innerPath);
  }
  wavIds.push_back(wav);
  return true;
}

bool readReplayArchiveAudioBatch(
    const ReplayArchiveAudioBatch &batch,
    const std::optional<archive_file::EntryRange> &range,
    std::vector<archive_file::FileData> &files, std::string *errorMessage) {
  if (range.has_value()) {
    std::string rangeError;
    if (archive_file::readArchiveEntriesInRange(
            batch.archivePath, batch.innerPaths, *range, files, &rangeError) &&
        files.size() == batch.innerPaths.size()) {
      return true;
    }
    files.clear();
  }
  return archive_file::readArchiveEntries(batch.archivePath, batch.innerPaths,
                                          files, errorMessage);
}

std::unordered_map<std::string, bms_parser::Note *>
buildReplayNoteLookup(bms_parser::Chart &chart) {
  std::unordered_map<std::string, bms_parser::Note *> lookup;
  for (const auto &measure : chart.Measures) {
    for (const auto &timeline : measure->TimeLines) {
      for (auto *note : timeline->Notes) {
        if (note == nullptr) {
          continue;
        }
        lookup[replayNoteKey(note->Lane, timeline->Timing)] = note;
      }
      for (auto *note : timeline->LandmineNotes) {
        if (note == nullptr) {
          continue;
        }
        lookup[replayNoteKey(note->Lane, timeline->Timing)] = note;
      }
    }
  }
  return lookup;
}

long long calculateExportDurationMicros(bms_parser::Chart &chart,
                                        const ReplayData &replay) {
  long long durationMicros =
      std::max(chart.Meta.TotalLength, chart.Meta.PlayLength);
  for (const auto &measure : chart.Measures) {
    for (const auto &timeline : measure->TimeLines) {
      durationMicros = std::max(durationMicros, timeline->Timing);
    }
  }
  for (const auto &event : replay.events) {
    durationMicros = std::max(durationMicros, event.songTimeMicros);
    durationMicros = std::max(durationMicros, event.noteTimeMicros);
  }
  for (const auto &sample : replay.touchSamples) {
    durationMicros = std::max(durationMicros, sample.songTimeMicros);
  }
  return std::max(0LL, durationMicros) + kAudioTailMicros;
}

std::vector<const bms_parser::TimeLine *>
collectBpmChangeTimelines(const bms_parser::Chart &chart) {
  std::vector<const bms_parser::TimeLine *> timelines;
  for (const auto &measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr || !timeline->BpmChange ||
          !std::isfinite(timeline->Bpm) || timeline->Bpm <= 0.0) {
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

std::vector<AudioEvent> collectAudioEvents(bms_parser::Chart &chart,
                                           const ReplayData &replay,
                                           long long keySoundOffsetMicros,
                                           long long &durationMicros) {
  std::vector<AudioEvent> events;
  durationMicros = calculateExportDurationMicros(chart, replay);

  for (const auto &measure : chart.Measures) {
    for (const auto &timeline : measure->TimeLines) {
      for (auto *note : timeline->BackgroundNotes) {
        if (note == nullptr || note->Wav == bms_parser::Parser::NoWav) {
          continue;
        }
        events.push_back({timeline->Timing, note->Wav});
      }
    }
  }

  const auto replayNotes = buildReplayNoteLookup(chart);
  for (const auto &event : replay.events) {
    if (event.action != ReplayEventAction::Press || event.noteTimeMicros < 0) {
      continue;
    }
    const auto noteIt =
        replayNotes.find(replayNoteKey(event.lane, event.noteTimeMicros));
    if (noteIt == replayNotes.end() ||
        noteIt->second->Wav == bms_parser::Parser::NoWav) {
      continue;
    }
    events.push_back(
        {event.songTimeMicros - keySoundOffsetMicros, noteIt->second->Wav});
  }

  std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
    if (a.timeMicros != b.timeMicros) {
      return a.timeMicros < b.timeMicros;
    }
    return a.wav < b.wav;
  });
  return events;
}

bool decodedSoundIsValid(const DecodedSound &decoded) {
  return decoded.info.frames > 0 && decoded.info.channels > 0 &&
         decoded.info.samplerate > 0;
}

void preloadArchivedDecodedSounds(const bms_parser::Chart &chart,
                                  const std::vector<AudioEvent> &audioEvents,
                                  DecodedSoundCache &decodedSounds,
                                  std::atomic_bool &isCancelled,
                                  ReplayVideoExportLog *log) {
  std::unordered_set<int> seenWavs;
  std::vector<int> wavOrder;
  wavOrder.reserve(audioEvents.size());
  for (const auto &event : audioEvents) {
    if (event.wav == bms_parser::Parser::NoWav) {
      continue;
    }
    if (seenWavs.insert(event.wav).second) {
      wavOrder.push_back(event.wav);
    }
  }

  std::unordered_map<path_t, ReplayArchiveAudioBatch> archiveBatches;
  std::vector<path_t> archiveBatchOrder;
  std::size_t archivedWavCount = 0;
  for (const int wav : wavOrder) {
    if (isCancelled || decodedSounds.contains(wav)) {
      continue;
    }

    const auto soundPath = resolveSoundPath(chart, wav);
    if (!soundPath.has_value()) {
      continue;
    }
    if (addReplayArchiveAudioTarget(archiveBatches, archiveBatchOrder,
                                    *soundPath, wav)) {
      ++archivedWavCount;
    }
  }

  if (archiveBatches.empty()) {
    return;
  }

  replayExportLog(log, "Replay export archived audio preload: %zu sounds, %zu "
                       "archive batch(es)",
                  archivedWavCount, archiveBatches.size());
  const auto preloadStart = std::chrono::steady_clock::now();
  std::size_t decodedCount = 0;
  for (const auto &archiveKey : archiveBatchOrder) {
    if (isCancelled) {
      break;
    }
    const auto batchIt = archiveBatches.find(archiveKey);
    if (batchIt == archiveBatches.end()) {
      continue;
    }

    const ReplayArchiveAudioBatch &batch = batchIt->second;
    std::vector<archive_file::FileData> files;
    std::string errorMessage;
    const auto range = entryRangeForReplayChartArchive(chart, batch.archivePath);
    const auto readStart = std::chrono::steady_clock::now();
    if (!readReplayArchiveAudioBatch(batch, range, files, &errorMessage)) {
      replayExportLog(log, "Replay export archived audio preload failed: %s: %s",
                      path_t_to_utf8(fspath_to_path_t(batch.archivePath)).c_str(),
                      errorMessage.c_str());
      continue;
    }
    replayExportLog(log,
                    "Replay export archived audio batch read: %s files=%zu "
                    "time=%.2fs",
                    path_t_to_utf8(fspath_to_path_t(batch.archivePath)).c_str(),
                    files.size(),
                    static_cast<double>(elapsedMicros(readStart)) / 1000000.0);

    const auto decodeStart = std::chrono::steady_clock::now();
    std::mutex decodedSoundsMutex;
    std::atomic_size_t decodedInBatch = 0;
    std::atomic_size_t failedInBatch = 0;
    parallel_for(files.size(), [&](int start, int end) {
      for (int i = start; i < end; ++i) {
        if (isCancelled) {
          return;
        }
        const auto &file = files[static_cast<std::size_t>(i)];
        const std::filesystem::path virtualPath =
            archive_file::makeVirtualPath(batch.archivePath, file.path);
        const path_t soundPath = fspath_to_path_t(virtualPath);
        const auto idsIt = batch.wavIdsByPath.find(soundPath);
        if (idsIt == batch.wavIdsByPath.end()) {
          continue;
        }

        auto decoded = std::make_shared<DecodedSound>();
        if (!decodeAudioBytesToPCM(soundPath, file.bytes, decoded->pcm,
                                   decoded->info, isCancelled) ||
            !decodedSoundIsValid(*decoded)) {
          std::lock_guard<std::mutex> lock(decodedSoundsMutex);
          for (const int wav : idsIt->second) {
            decodedSounds.emplace(wav, std::shared_ptr<DecodedSound>{});
          }
          ++failedInBatch;
          continue;
        }

        std::lock_guard<std::mutex> lock(decodedSoundsMutex);
        for (const int wav : idsIt->second) {
          if (decodedSounds.emplace(wav, decoded).second) {
            ++decodedInBatch;
          }
        }
      }
    });
    decodedCount += decodedInBatch.load(std::memory_order_relaxed);
    if (failedInBatch.load(std::memory_order_relaxed) > 0) {
      replayExportLog(log,
                      "Replay export archived audio decode failures: %s "
                      "count=%zu",
                      path_t_to_utf8(fspath_to_path_t(batch.archivePath)).c_str(),
                      failedInBatch.load(std::memory_order_relaxed));
    }
    replayExportLog(log,
                    "Replay export archived audio batch decode: %s time=%.2fs",
                    path_t_to_utf8(fspath_to_path_t(batch.archivePath)).c_str(),
                    static_cast<double>(elapsedMicros(decodeStart)) / 1000000.0);
  }
  replayExportLog(log,
                  "Replay export archived audio preload finished: decoded=%zu "
                  "time=%.2fs",
                  decodedCount,
                  static_cast<double>(elapsedMicros(preloadStart)) / 1000000.0);
}

DecodedSound *
loadDecodedSound(const bms_parser::Chart &chart, int wav,
                 DecodedSoundCache &decodedSounds,
                 std::atomic_bool &isCancelled) {
  if (const auto decodedIt = decodedSounds.find(wav);
      decodedIt != decodedSounds.end()) {
    return decodedIt->second.get();
  }

  const auto soundPath = resolveSoundPath(chart, wav);
  if (!soundPath.has_value()) {
    SDL_Log("Replay export missing sound %d", wav);
    decodedSounds.emplace(wav, std::shared_ptr<DecodedSound>{});
    return nullptr;
  }

  auto decoded = std::make_shared<DecodedSound>();
  const auto resolvedPath = soundPath.value();
  if (!decodeAudioToPCM(fspath_to_path_t(resolvedPath), decoded->pcm,
                        decoded->info, isCancelled)) {
    SDL_Log("Replay export failed to decode sound %d: %s", wav,
            resolvedPath.string().c_str());
    if (!isCancelled) {
      decodedSounds.emplace(wav, std::shared_ptr<DecodedSound>{});
    }
    return nullptr;
  }
  if (!decodedSoundIsValid(*decoded)) {
    SDL_Log("Replay export decoded invalid sound %d: %s", wav,
            resolvedPath.string().c_str());
    decodedSounds.emplace(wav, std::shared_ptr<DecodedSound>{});
    return nullptr;
  }

  auto [insertedIt, _] = decodedSounds.emplace(wav, std::move(decoded));
  return insertedIt->second.get();
}

void ensureMixFrames(std::vector<float> &mix, size_t frames) {
  const size_t samples = frames * kExportChannels;
  if (mix.size() < samples) {
    mix.resize(samples, 0.0f);
  }
}

float sampleDecodedChannel(const DecodedSound &sound, size_t frame,
                           int channel) {
  const int sourceChannels = sound.info.channels;
  const int sourceChannel =
      sourceChannels == 1 ? 0 : std::min(channel, sourceChannels - 1);
  const size_t sampleIndex = frame * static_cast<size_t>(sourceChannels) +
                             static_cast<size_t>(sourceChannel);
  if (sampleIndex >= sound.pcm.size()) {
    return 0.0f;
  }
  return static_cast<float>(sound.pcm[sampleIndex]) / 32768.0f;
}

void mixSoundAt(std::vector<float> &mix, const DecodedSound &sound,
                long long timeMicros) {
  const long long clampedTime = std::max(0LL, timeMicros);
  const size_t startFrame = static_cast<size_t>(
      (static_cast<long double>(clampedTime) * kExportSampleRate) / 1000000.0L);
  const size_t sourceFrames = static_cast<size_t>(sound.info.frames);
  const double sourceToTarget =
      static_cast<double>(sound.info.samplerate) / kExportSampleRate;
  const size_t targetFrames = static_cast<size_t>(
      std::ceil(static_cast<double>(sourceFrames) / sourceToTarget));

  ensureMixFrames(mix, startFrame + targetFrames + 1);
  for (size_t targetFrame = 0; targetFrame < targetFrames; ++targetFrame) {
    const double sourcePosition =
        static_cast<double>(targetFrame) * sourceToTarget;
    const size_t sourceFrame0 =
        std::min(static_cast<size_t>(sourcePosition),
                 sourceFrames > 0 ? sourceFrames - 1 : 0);
    const size_t sourceFrame1 =
        std::min(sourceFrame0 + 1, sourceFrames > 0 ? sourceFrames - 1 : 0);
    const float fraction =
        static_cast<float>(sourcePosition - static_cast<double>(sourceFrame0));

    for (int channel = 0; channel < kExportChannels; ++channel) {
      const float s0 = sampleDecodedChannel(sound, sourceFrame0, channel);
      const float s1 = sampleDecodedChannel(sound, sourceFrame1, channel);
      const float sample = s0 + (s1 - s0) * fraction;
      mix[(startFrame + targetFrame) * kExportChannels + channel] += sample;
    }
  }
}

bool writeWavFile(const std::filesystem::path &path,
                  const std::vector<float> &mix, std::string &errorMessage) {
  SF_INFO outputInfo{};
  outputInfo.samplerate = kExportSampleRate;
  outputInfo.channels = kExportChannels;
  outputInfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

#ifdef _WIN32
  SNDFILE *rawFile =
      sf_wchar_open(path.wstring().c_str(), SFM_WRITE, &outputInfo);
#else
  SNDFILE *rawFile = sf_open(path.string().c_str(), SFM_WRITE, &outputInfo);
#endif
  UniqueResource<SNDFILE, sf_close> file(rawFile);
  if (file == nullptr) {
    errorMessage = std::string("Failed to open replay audio output: ") +
                   sf_strerror(nullptr);
    return false;
  }

  std::vector<short> pcm;
  pcm.reserve(mix.size());
  for (const float sample : mix) {
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    pcm.push_back(static_cast<short>(std::lrint(clamped * 32767.0f)));
  }

  const sf_count_t framesToWrite =
      static_cast<sf_count_t>(pcm.size() / kExportChannels);
  const sf_count_t framesWritten =
      sf_writef_short(file.get(), pcm.data(), framesToWrite);

  if (framesWritten != framesToWrite) {
    errorMessage = "Failed to write complete replay audio track";
    return false;
  }
  return true;
}

ReplayVideoExportResult
writeReplayAudioTrack(bms_parser::Chart &chart, const ReplayData &replay,
                      const std::filesystem::path &path,
                      ReplayVideoExportLog *log) {
  long long durationMicros = 0;
  constexpr long long keySoundOffsetMicros = 0;
  const auto audioEvents =
      collectAudioEvents(chart, replay, keySoundOffsetMicros, durationMicros);
  const size_t initialFrames = static_cast<size_t>(
      (static_cast<long double>(std::max(0LL, durationMicros)) *
       kExportSampleRate) /
          1000000.0L +
      1.0L);
  std::vector<float> mix(initialFrames * kExportChannels, 0.0f);
  DecodedSoundCache decodedSounds;
  std::atomic_bool isCancelled = false;
  preloadArchivedDecodedSounds(chart, audioEvents, decodedSounds, isCancelled,
                               log);

  for (const auto &event : audioEvents) {
    DecodedSound *sound =
        loadDecodedSound(chart, event.wav, decodedSounds, isCancelled);
    if (sound == nullptr) {
      continue;
    }
    mixSoundAt(mix, *sound, event.timeMicros);
  }

  std::string errorMessage;
  if (!writeWavFile(path, mix, errorMessage)) {
    return {.success = false, .outputPath = path, .message = errorMessage};
  }
  return {.success = true, .outputPath = path, .message = "Audio exported"};
}

void resetChartNotes(bms_parser::Chart &chart) {
  for (const auto &measure : chart.Measures) {
    for (const auto &timeline : measure->TimeLines) {
      for (auto *note : timeline->Notes) {
        if (note != nullptr) {
          note->Reset();
        }
      }
      for (auto *note : timeline->InvisibleNotes) {
        if (note != nullptr) {
          note->Reset();
        }
      }
      for (auto *note : timeline->LandmineNotes) {
        if (note != nullptr) {
          note->Reset();
        }
      }
    }
  }
}

std::vector<bms_parser::LongNote *>
collectReplayAutoReleaseTails(bms_parser::Chart &chart) {
  std::vector<bms_parser::LongNote *> tails;
  for (const auto &measure : chart.Measures) {
    for (const auto &timeline : measure->TimeLines) {
      for (auto *note : timeline->Notes) {
        if (note == nullptr || !note->IsLongNote()) {
          continue;
        }
        auto *longNote = static_cast<bms_parser::LongNote *>(note);
        if (longNote->IsTail() && longNote->Timeline != nullptr) {
          tails.push_back(longNote);
        }
      }
    }
  }

  std::sort(tails.begin(), tails.end(),
            [](const bms_parser::LongNote *a, const bms_parser::LongNote *b) {
              if (a->Timeline->Timing != b->Timeline->Timing) {
                return a->Timeline->Timing < b->Timeline->Timing;
              }
              return a->Lane < b->Lane;
            });
  return tails;
}

void releaseDueReplayLongNoteTails(
    const std::vector<bms_parser::LongNote *> &tails, size_t &cursor,
    long long songTimeMicros) {
  while (cursor < tails.size()) {
    auto *tail = tails[cursor];
    if (tail == nullptr || tail->Timeline == nullptr) {
      ++cursor;
      continue;
    }
    if (tail->Timeline->Timing > songTimeMicros) {
      break;
    }
    if (!tail->IsPlayed && tail->IsHolding) {
      tail->Release(tail->Timeline->Timing);
    }
    ++cursor;
  }
}

class ScopedChartNoteReset {
public:
  explicit ScopedChartNoteReset(bms_parser::Chart &chart) : chart(chart) {
    resetChartNotes(chart);
  }

  ~ScopedChartNoteReset() { resetChartNotes(chart); }

private:
  bms_parser::Chart &chart;
};

bms_parser::Note *findReplayNote(
    const std::unordered_map<std::string, bms_parser::Note *> &lookup,
    const ReplayEvent &event) {
  if (event.noteTimeMicros < 0) {
    return nullptr;
  }
  const auto it = lookup.find(replayNoteKey(event.lane, event.noteTimeMicros));
  return it == lookup.end() ? nullptr : it->second;
}

bool applyReplayEventForVideo(
    BMSRenderer &renderer,
    const std::unordered_map<std::string, bms_parser::Note *> &lookup,
    const ReplayEvent &event, long long visualTimeMicros, bool gaugeAutoShift) {
  const JudgeResult recordedJudge(event.judgement, event.diffMicros);
  auto applyHud = [&]() -> bool {
    if (event.judgement == None) {
      return false;
    }
    renderer.onJudge(recordedJudge, event.combo, event.score,
                     visualTimeMicros,
                     event.action != ReplayEventAction::Miss);
    renderer.setGaugeStatus(event.gaugeType, gaugeAutoShift, event.gauge);
    return true;
  };

  switch (event.action) {
  case ReplayEventAction::Press: {
    bool suppressHudForLongNoteHead = false;
    if (auto *note = findReplayNote(lookup, event); note != nullptr) {
      if (note->IsLongNote()) {
        auto *longNote = static_cast<bms_parser::LongNote *>(note);
        suppressHudForLongNoteHead =
            !longNote->IsTail() && recordedJudge.isNotePlayed();
        if (recordedJudge.isNotePlayed() && !longNote->IsTail()) {
          longNote->Press(event.judgeTimeMicros);
        }
      } else if (recordedJudge.isNotePlayed()) {
        note->Press(event.judgeTimeMicros);
      }
    }
    if (!suppressHudForLongNoteHead) {
      const bool appliedHud = applyHud();
      renderer.onLanePressed(event.lane, recordedJudge, visualTimeMicros);
      return appliedHud;
    }
    renderer.onLanePressed(event.lane, recordedJudge, visualTimeMicros);
    return false;
  }
  case ReplayEventAction::Release: {
    if (auto *note = findReplayNote(lookup, event);
        note != nullptr && note->IsLongNote() && event.judgement != None) {
      auto *longNote = static_cast<bms_parser::LongNote *>(note);
      if (longNote->IsTail() && longNote->IsHolding) {
        longNote->Release(event.judgeTimeMicros);
      }
    }
    const bool appliedHud = applyHud();
    renderer.onLaneReleased(event.lane, visualTimeMicros);
    return appliedHud;
  }
  case ReplayEventAction::Miss:
    return applyHud();
  case ReplayEventAction::Mine:
    if (auto *note = findReplayNote(lookup, event); note != nullptr) {
      note->IsPlayed = true;
      note->IsDead = true;
      note->PlayedTime = event.judgeTimeMicros;
    }
    renderer.setGaugeStatus(event.gaugeType, gaugeAutoShift, event.gauge);
    return false;
  }
  return false;
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
  auto valueY = [&](float value) {
    const float clamped = std::clamp(value, 0.0f, 100.0f);
    return graphY + graphH - (clamped / 100.0f) * graphH;
  };

  const uint32_t guideColor = ui_theme::hairlineSubtle().toABGR();
  batch.addLine(graphX, valueY(80.0f), graphX + graphW, valueY(80.0f), 1.0f,
                guideColor);
  batch.addLine(graphX, valueY(30.0f), graphX + graphW, valueY(30.0f), 1.0f,
                guideColor);

  const size_t count = resultState.gaugeHistory.size();
  if (count == 1) {
    const float value = std::clamp(resultState.gaugeHistory.front(), 0.0f,
                                   100.0f);
    batch.addCircle(graphX, valueY(value), 3.5f,
                    resultGaugeLineColor(value).toABGR());
    return;
  }

  for (size_t i = 1; i < count; ++i) {
    const float prevValue =
        std::clamp(resultState.gaugeHistory[i - 1], 0.0f, 100.0f);
    const float value = std::clamp(resultState.gaugeHistory[i], 0.0f, 100.0f);
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
    const float value = std::clamp(resultState.gaugeHistory[i], 0.0f, 100.0f);
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
#endif
  if (const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
      codec != nullptr) {
    return codec;
  }
#if !__APPLE__
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

bool encodeFrame(AVCodecContext *encoderContext, AVFormatContext *formatContext,
                 AVStream *stream, AVFrame *frame, AVPacket *packet,
                 std::string &errorMessage, int64_t forcedPacketDuration = 0) {
  auto receiveAvailablePackets = [&](bool *wrotePacket = nullptr) {
    if (wrotePacket != nullptr) {
      *wrotePacket = false;
    }
    av_packet_unref(packet);
    int ret = avcodec_receive_packet(encoderContext, packet);
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
      ret = av_interleaved_write_frame(formatContext, packet);
      av_packet_unref(packet);
      if (ret < 0) {
        errorMessage = "Failed to write encoded packet: " + ffmpegError(ret);
        return false;
      }
      if (wrotePacket != nullptr) {
        *wrotePacket = true;
      }

      ret = avcodec_receive_packet(encoderContext, packet);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return true;
      }
    }

    errorMessage = "Failed to receive encoded packet: " + ffmpegError(ret);
    return false;
  };

  int stalledRetries = 0;
  while (true) {
    const int ret = avcodec_send_frame(encoderContext, frame);
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
        SDL_Delay(1);
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

    const std::string outputPathString = outputPath.string();
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
    if (replayVideoEncoderSupportsFrameThreads(videoCodec)) {
      videoContext->thread_count = replayVideoEncoderThreadCount();
      videoContext->thread_type = FF_THREAD_FRAME;
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
#ifdef _WIN32
    audioFile = sf_wchar_open(wavPath.wstring().c_str(), SFM_READ, &audioInfo);
#else
    audioFile = sf_open(wavPath.string().c_str(), SFM_READ, &audioInfo);
#endif
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

    pixelConvertThreadCount = replayVideoPixelConvertThreadCount();
    if (videoContext->pix_fmt != AV_PIX_FMT_BGRA) {
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
    replayExportLog(
        log, "Replay video export pixel converter threads: %d",
        videoContext->pix_fmt == AV_PIX_FMT_BGRA ? 1 : pixelConvertThreadCount);

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
    int ret = av_frame_make_writable(videoFrame);
    framePrepareMicrosTotal += elapsedMicros(framePrepareStart);
    if (ret < 0) {
      errorMessage = "Failed to prepare video frame: " + ffmpegError(ret);
      return false;
    }

    const auto pixelConvertStart = std::chrono::steady_clock::now();
    if (videoContext->pix_fmt == AV_PIX_FMT_BGRA) {
      const int sourceLinesize = width * 4;
      for (int y = 0; y < height; ++y) {
        std::memcpy(videoFrame->data[0] + y * videoFrame->linesize[0],
                    bgraFrame + y * sourceLinesize,
                    static_cast<size_t>(sourceLinesize));
      }
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
    const bool success =
        encodeFrame(videoContext, formatContext, videoStream, videoFrame,
                    videoPacket, errorMessage, videoFrameDuration);
    videoEncodeMicrosTotal += elapsedMicros(videoEncodeStart);
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
    if (!encodeFrame(videoContext, formatContext, videoStream, nullptr,
                     videoPacket, errorMessage, videoFrameDuration)) {
      videoEncodeMicrosTotal += elapsedMicros(videoEncodeFlushStart);
      return fail(errorMessage);
    }
    videoEncodeMicrosTotal += elapsedMicros(videoEncodeFlushStart);
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
  long long audioEncodeMicrosTotal = 0;
  long long framePrepareMicrosTotal = 0;
  long long pixelConvertMicrosTotal = 0;
  long long videoEncodeMicrosTotal = 0;
  int64_t nextAudioPts = 0;
  int pixelConvertThreadCount = 1;
  bool audioFinished = false;
};

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
// FIXME: Re-enable this native AVAssetWriter path after the iOS export stall is
// resolved. Large 120fps exports currently stop making progress while the
// writer stays in AVAssetWriterStatusWriting, so iOS uses ReplayMp4StreamWriter
// for now.
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
    writer =
        CreateIOSReplayVideoWriter(wavPath.string(), outputPath.string(), width,
                                   height, fps, bitRate, nativeErrorMessage);
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
                       const ReplayVideoExportOptions &options,
                       const std::filesystem::path &wavPath,
                       const std::filesystem::path &outputPath,
                       ReplayVideoExportLog *log) {
  const auto resolvedOptions = resolveReplayVideoExportOptions(options);
  const int width = resolvedOptions.width;
  const int height = resolvedOptions.height;
  const int fps = resolvedOptions.fps;

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
  const size_t frameBytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  const size_t frameBufferCount = replayVideoFrameBufferCount(width, height);
  std::vector<bgfx::TextureHandle> readbackTextures(frameBufferCount,
                                                    BGFX_INVALID_HANDLE);
  std::unique_ptr<rendering::BlurPass> bgaBlurPass;
  std::unique_ptr<View> resultRoot;
  View *resultGraphPlaceholder = nullptr;
  auto cleanupBgfx = [&]() {
    resultGraphPlaceholder = nullptr;
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

  ScopedChartNoteReset chartReset(chart);
  Judge judge(chart.Meta.Rank);
  BMSRenderer renderer(&chart, judge.timingWindows,
                       settings.visibleTimeGreenNumber);
  renderer.setVisibleTimeBpmStrategy(settings.visibleTimeBpmStrategy);
  renderer.setVisibleTimeUseMilliseconds(settings.visibleTimeUseMilliseconds);
  renderer.setPlayAreaWidth(
      settings.playAreaWidthForKeyMode(chart.Meta.KeyMode));
  renderer.setLaneBeamLengthPercent(settings.laneBeamLengthPercent);
  renderer.setNoteStartPositionPercent(settings.noteStartPositionPercent);
  renderer.setLaneBeamClockUsesRenderTime(true);
  renderer.setShowInvisibleNotes(settings.showInvisibleNotes);
  const auto bpmChangeTimelines = collectBpmChangeTimelines(chart);
  size_t bpmChangeCursor = 0;
  double currentExportBpm = chart.Meta.Bpm;
  renderer.setCurrentBpm(currentExportBpm);
  auto applyExportBpm = [&](long long songTimeMicros) {
    while (bpmChangeCursor < bpmChangeTimelines.size() &&
           bpmChangeTimelines[bpmChangeCursor]->Timing <= songTimeMicros) {
      const double bpm = bpmChangeTimelines[bpmChangeCursor]->Bpm;
      if (std::abs(currentExportBpm - bpm) > 0.0001) {
        currentExportBpm = bpm;
        renderer.setCurrentBpm(currentExportBpm);
      }
      ++bpmChangeCursor;
    }
  };
  size_t laneCoverEventCursor = 0;
  auto applyReplayLaneCoverEvents = [&](long long songTimeMicros) {
    while (laneCoverEventCursor < replay.laneCoverEvents.size() &&
           replay.laneCoverEvents[laneCoverEventCursor].songTimeMicros <=
               songTimeMicros) {
      const auto &event = replay.laneCoverEvents[laneCoverEventCursor];
      renderer.applyLaneCoverState(event.noteStartPositionPercent,
                                   event.resetVisibleTimeReference);
      ++laneCoverEventCursor;
    }
  };
  const bool judgementIndicatorHudMode =
      settings.judgementIndicatorRenderMode ==
      AppSettings::JudgementIndicatorRenderMode::Hud2D;
  renderer.setJudgementIndicatorConfig(settings.judgementIndicatorEnabled,
                                       settings.judgementIndicatorY,
                                       settings.judgementIndicatorWidthScale,
                                       judgementIndicatorHudMode);
  renderer.setJudgementTextY(settings.judgementTextY);
  renderer.setJudgementCounterEnabled(settings.judgementCounterEnabled);
  renderer.setJudgementCounterPosition(settings.judgementCounterPosition);
  renderer.setGaugeBarPosition(settings.gaugeBarPosition);
  renderer.setGaugeStatus(replay.initialGaugeType, replay.gaugeAutoShift,
                          gaugeInitialValue(replay.initialGaugeType));
  renderer.setPlayOptionStatus(replayExportPlayOptionLabel(replay));
  renderer.setReplayData(&replay);
  renderer.setAutoPlayMarkVisible(replay.autoPlay);
  renderer.setTouchVisualizationEnabled(resolvedOptions.renderTouchPoints);
  renderer.setReplayGhostRenderingEnabled(resolvedOptions.renderReplayGhosts);

  const auto replayNotes = buildReplayNoteLookup(chart);
  const auto replayAutoReleaseTails = collectReplayAutoReleaseTails(chart);
  size_t replayAutoReleaseTailCursor = 0;
  const long long gameplayDurationMicros =
      calculateExportDurationMicros(chart, replay);
  const long long scheduledVisualEndMicros =
      context.jukebox.getScheduledVisualEndMicros();
  const long long visualTailMicros =
      std::max(0LL, scheduledVisualEndMicros - gameplayDurationMicros);
  const long long resultTailMicros =
      resolvedOptions.includeResultScreen
          ? std::max(kResultSceneTailMicros, visualTailMicros)
          : visualTailMicros;
  const long long totalDurationMicros =
      gameplayDurationMicros + resultTailMicros;
  const long long visualOffsetMicros =
      static_cast<long long>(settings.visualOffsetMs) * 1000LL;
  const size_t gameplayFrameCount = static_cast<size_t>(std::ceil(
      static_cast<long double>(gameplayDurationMicros) * fps / 1000000.0L));
  const size_t resultFrameCount = static_cast<size_t>(
      std::ceil(static_cast<long double>(resultTailMicros) * fps / 1000000.0L));
  const size_t frameCount = gameplayFrameCount + resultFrameCount;
  const RhythmState replayResultState =
      replay_result::BuildResultState(chart, replay);
  rendering::SimpleBatchRenderer resultGraphBatch;
  if (resultFrameCount > 0 && resolvedOptions.includeResultScreen) {
    resultRoot = std::make_unique<View>(0, 0, rendering::window_width,
                                        rendering::window_height);
    std::optional<ResultPreviousBestData> previousBest;
    std::optional<std::string> beforeCreatedAt;
    if (!replay.autoPlay && !replay.createdAt.empty()) {
      beforeCreatedAt = replay.createdAt;
    }
    if (const auto best =
            ScoreDBHelper::GetInstance().LoadBestScore(chart.Meta, beforeCreatedAt);
        best.has_value()) {
      previousBest = {.score = best->score,
                      .maxScore = best->maxScore,
                      .maxCombo = best->maxCombo,
                      .comboBreak = best->comboBreak,
                      .finalGauge = best->finalGauge,
                      .clearType = best->clearType,
                      .createdAt = best->createdAt};
    }
    std::string difficultyLabel;
    auto &dbHelper = ChartDBHelper::GetInstance();
    sqlite3 *db = dbHelper.Connect();
    if (db != nullptr) {
      difficultyLabel = dbHelper.DifficultyTableLabelsForChart(db, chart.Meta);
      dbHelper.Close(db);
    }
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
    DefaultSkin resultSkin;
    resultSkin.buildLayout("Result", resultRoot.get(), &resultSkinData);
    resultRoot->applyYogaLayout();
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
  replayExportLog(log,
                  "Replay video export workload: %zu frames, %.2fs, %.2f GiB "
                  "BGRA readback",
                  frameCount, durationSeconds, rawFrameGiB);
  if (scheduledVisualEndMicros > gameplayDurationMicros) {
    replayExportLog(log,
                    "Replay video export BGA tail: visual end %.2fs, "
                    "gameplay end %.2fs",
                    static_cast<double>(scheduledVisualEndMicros) / 1000000.0,
                    static_cast<double>(gameplayDurationMicros) / 1000000.0);
  }
  replayExportLog(log,
                  "Replay video export frame buffers/readbacks: %zu, encoder "
                  "threads: %d",
                  frameBufferCount, replayVideoEncoderThreadCount());
  RenderContext renderContext;
  size_t replayCursor = 0;
  std::map<Judgement, int> replayJudgeCounts;
  for (int i = 0; i < JudgementCount; i++) {
    replayJudgeCounts[static_cast<Judgement>(i)] = 0;
  }
  int replayComboBreak = 0;
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
    const long long songTimeMicros = static_cast<long long>(
        (static_cast<long double>(frameIndex) * 1000000.0L) / fps);
    const long long visualTimeMicros =
        std::max(0LL, songTimeMicros - visualOffsetMicros);
    while (replayCursor < replay.events.size() &&
           replay.events[replayCursor].songTimeMicros <= songTimeMicros) {
      const auto &event = replay.events[replayCursor];
      const bool appliedHud =
          applyReplayEventForVideo(renderer, replayNotes, event,
                                   visualTimeMicros, replay.gaugeAutoShift);
      if (appliedHud && event.judgement != None) {
        replayJudgeCounts[event.judgement]++;
        if (JudgeResult(event.judgement, event.diffMicros).isComboBreak()) {
          replayComboBreak++;
        }
        renderer.setJudgementCounters(replayJudgeCounts, replayComboBreak);
      }
      ++replayCursor;
    }
    releaseDueReplayLongNoteTails(replayAutoReleaseTails,
                                  replayAutoReleaseTailCursor, songTimeMicros);
    applyExportBpm(songTimeMicros);
    applyReplayLaneCoverEvents(songTimeMicros);

    if (!renderAndQueueFrame(frameIndex, songTimeMicros, [&]() {
          bgfx::touch(rendering::clear_view);
          bgfx::touch(rendering::bga_view);
          bgfx::touch(rendering::bga_layer_view);
          context.jukebox.renderVisualsAt(songTimeMicros);
          bgaBlurPass->execute();
          rendering::renderFullscreenTextureTint(
              bgaBlurPass->outputTexture(), rendering::final_view,
              static_cast<float>(settings.bgaBrightnessPercent) / 100.0f);
          renderer.render(renderContext, visualTimeMicros, songTimeMicros);
        })) {
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
    if (!renderAndQueueFrame(frameIndex, videoTimeMicros, [&]() {
          bgfx::touch(rendering::clear_view);
          bgfx::touch(rendering::bga_view);
          bgfx::touch(rendering::bga_layer_view);
          context.jukebox.renderVisualsAt(videoTimeMicros);
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
  return result;
}

ReplayVideoExportResult
saveReplayVideoToPlatformLibrary(const ReplayVideoExportResult &muxResult) {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  std::string errorMessage;
  if (!SaveVideoToIOSPhotos(muxResult.outputPath.string(), errorMessage)) {
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

  std::error_code ec;
  const auto outputDir = Utils::GetDocumentsPath("video_exports");
  std::filesystem::create_directories(outputDir, ec);
  if (ec) {
    return {.success = false,
            .message = "Failed to create replay export directory"};
  }

  const std::string baseName =
      sanitizeFileNamePart(chart->Meta.Title) + "_" + makeTimestamp();
  const auto tempDir = outputDir / (baseName + "_tmp");
  const auto logPath = outputDir / (baseName + ".log");
  ReplayVideoExportLog exportLog(logPath);
  replayExportLog(&exportLog, "Replay export log: %s",
                  logPath.string().c_str());
  replayExportLog(&exportLog, "Replay export chart: %s",
                  chart->Meta.Title.c_str());
  if (replay.randomSeed.has_value()) {
    replayExportLog(&exportLog, "Replay export random seed: %u",
                    *replay.randomSeed);
  }
  if (replay.randomPrng.has_value()) {
    replayExportLog(&exportLog, "Replay export random PRNG: %s",
                    replay.randomPrng->c_str());
  }
  if (replay.playOption.has_value()) {
    replayExportLog(&exportLog, "Replay export play option: %s",
                    replay.playOption->c_str());
  }
  if (replay.playOptionSeed.has_value()) {
    replayExportLog(&exportLog, "Replay export play option seed: %lld",
                    *replay.playOptionSeed);
  }
  if (replay.playOption2.has_value()) {
    replayExportLog(&exportLog, "Replay export 2P play option: %s",
                    replay.playOption2->c_str());
  }
  if (replay.playOption2Seed.has_value()) {
    replayExportLog(&exportLog, "Replay export 2P play option seed: %lld",
                    *replay.playOption2Seed);
  }
  const auto totalStart = std::chrono::steady_clock::now();

  std::filesystem::create_directories(tempDir, ec);
  if (ec) {
    replayExportLog(&exportLog,
                    "Replay export failed to create work directory: %s",
                    tempDir.string().c_str());
    return {.success = false,
            .message = "Failed to create replay export work directory"};
  }

  const auto resolvedOptions = resolveReplayVideoExportOptions(options);

  const auto wavPath = tempDir / "audio.wav";
  const auto outputPath = outputDir / (baseName + ".mp4");

  replayExportLog(&exportLog, "Replay export audio: %s",
                  wavPath.string().c_str());
  reportReplayExportProgress(resolvedOptions, 0.02, "Building audio track");
  const auto audioStart = std::chrono::steady_clock::now();
  auto audioResult = writeReplayAudioTrack(*chart, replay, wavPath, &exportLog);
  if (!audioResult.success) {
    replayExportLog(&exportLog, "Replay export audio failed: %s",
                    audioResult.message.c_str());
    std::filesystem::remove_all(tempDir, ec);
    return audioResult;
  }
  replayExportLog(&exportLog, "Replay export audio finished in %.2fs",
                  static_cast<double>(elapsedMicros(audioStart)) / 1000000.0);
  reportReplayExportProgress(resolvedOptions, 0.05, "Audio track ready");

  replayExportLog(&exportLog, "Replay export MP4: %s (%dx%d @ %dfps)",
                  outputPath.string().c_str(), resolvedOptions.width,
                  resolvedOptions.height, resolvedOptions.fps);
  const auto videoStart = std::chrono::steady_clock::now();
  auto muxResult =
      renderReplayVideoToMp4(context, *chart, replay, context.settings,
                             resolvedOptions, wavPath, outputPath, &exportLog);
  if (!muxResult.success) {
    replayExportLog(&exportLog, "Replay export MP4 failed: %s",
                    muxResult.message.c_str());
    std::filesystem::remove(outputPath, ec);
    std::filesystem::remove_all(tempDir, ec);
    return muxResult;
  }
  replayExportLog(&exportLog, "Replay export MP4 finished in %.2fs",
                  static_cast<double>(elapsedMicros(videoStart)) / 1000000.0);

  std::filesystem::remove_all(tempDir, ec);
  if (ec) {
    replayExportLog(&exportLog,
                    "Replay export could not clean work directory: %s",
                    tempDir.string().c_str());
  }

  reportReplayExportProgress(resolvedOptions, 0.99, "Saving video");
  auto platformSaveResult = saveReplayVideoToPlatformLibrary(muxResult);
  if (!platformSaveResult.success) {
    replayExportLog(&exportLog, "Replay export platform save failed: %s",
                    platformSaveResult.message.c_str());
    return platformSaveResult;
  }
  reportReplayExportProgress(resolvedOptions, 1.0, platformSaveResult.message);
  replayExportLog(&exportLog, "Replay export finished in %.2fs: %s",
                  static_cast<double>(elapsedMicros(totalStart)) / 1000000.0,
                  platformSaveResult.message.c_str());
  return platformSaveResult;
}
