#include "ReplayVideoExporter.h"

#include "Utils.h"
#include "audio/decoder.h"
#include "path.h"
#include "rendering/BlurPass.h"
#include "rendering/RenderPlan.h"
#include "rendering/common.h"
#include "scene/play/BMSRenderer.h"
#include "scene/play/Judge.h"
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
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
constexpr int kExportSampleRate = 44100;
constexpr int kExportChannels = 2;
constexpr int kDefaultExportFps = 120;
constexpr int kH264HighProfile = 100;
constexpr long long kAudioTailMicros = 3000000;
constexpr const char *kQuickTimeFullFrameRatePlaybackIntentKey =
    "com.apple.quicktime.full-frame-rate-playback-intent";
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
    height = static_cast<int>(
        std::lround(static_cast<double>(width) / aspectRatio));
  } else if (hasHeight) {
    height = options.height;
    width = static_cast<int>(
        std::lround(static_cast<double>(height) * aspectRatio));
  }

  return {.width = makeEvenExportDimension(width),
          .height = makeEvenExportDimension(height),
          .fps = std::clamp(options.fps > 0 ? options.fps : kDefaultExportFps,
                            1, 120)};
}

int64_t replayVideoBitRate(int width, int height, int fps) {
  const int64_t pixelsPerSecond =
      static_cast<int64_t>(width) * static_cast<int64_t>(height) *
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

bool replayVideoEncoderSupportsFrameThreads(const AVCodec *codec) {
  return codec != nullptr &&
         (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS) != 0;
}

int replayVideoH264Level(int width, int height, int fps) {
  const int64_t macroblocksPerFrame =
      static_cast<int64_t>((width + 15) / 16) *
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

size_t replayVideoFrameBufferCount() {
  const auto hardwareThreads = std::thread::hardware_concurrency();
  if (hardwareThreads <= 2) {
    return 2;
  }
  return std::clamp<size_t>(static_cast<size_t>(hardwareThreads / 2), 2, 4);
}

long long elapsedMicros(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

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
  bx::mtxOrtho(ortho, 0.0f, rendering::window_width,
               rendering::window_height, 0.0f, 0.0f, 100.0f, 0.0f,
               bgfx::getCaps()->homogeneousDepth);
  for (const auto view : rendering::kGameplayOrthographicOutputViews) {
    bgfx::setViewTransform(view, nullptr, ortho);
  }
  bgfx::setViewTransform(rendering::bga_view, nullptr, ortho);
  bgfx::setViewTransform(rendering::bga_layer_view, nullptr, ortho);
  rendering::game_camera.render(true);
}

void configureReplayExportRenderViews(
    int width, int height, bgfx::FrameBufferHandle outputFrameBuffer,
    const rendering::BlurPass &bgaBlurPass, const AppSettings &settings) {
  const auto exportWidth = static_cast<uint16_t>(width);
  const auto exportHeight = static_cast<uint16_t>(height);

  for (const auto view : rendering::kGameplayOutputViews) {
    bgfx::setViewFrameBuffer(view, outputFrameBuffer);
    bgfx::setViewRect(view, 0, 0, exportWidth, exportHeight);
  }
  bgfx::setViewFrameBuffer(rendering::readback_view, BGFX_INVALID_HANDLE);
  bgfx::setViewRect(rendering::readback_view, 0, 0, exportWidth,
                    exportHeight);
  bgfx::setViewRect(rendering::bga_view, 0, 0, bgaBlurPass.sceneWidth(),
                    bgaBlurPass.sceneHeight());
  bgfx::setViewRect(rendering::bga_layer_view, 0, 0,
                    bgaBlurPass.sceneWidth(), bgaBlurPass.sceneHeight());

  float ortho[16];
  bx::mtxOrtho(ortho, 0.0f, rendering::window_width,
               rendering::window_height, 0.0f, 0.0f, 100.0f, 0.0f,
               bgfx::getCaps()->homogeneousDepth);
  for (const auto view : rendering::kGameplayOrthographicOutputViews) {
    bgfx::setViewTransform(view, nullptr, ortho);
  }
  bgfx::setViewTransform(rendering::bga_view, nullptr, ortho);
  bgfx::setViewTransform(rendering::bga_layer_view, nullptr, ortho);

  constexpr float kCameraDepth = 2.1f;
  const float laneLookAtY = settings.laneLength * 0.25f;
  const float laneAngleRad = bx::toRad(settings.laneAngleDegrees);
  const bx::Vec3 at = {4.0f, laneLookAtY, 0.0f};
  const bx::Vec3 eye = {4.0f,
                        laneLookAtY - std::tan(laneAngleRad) * kCameraDepth,
                        -kCameraDepth};
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  rendering::game_camera.edit()
      .setPosition(eye)
      .setLookAt(at)
      .setAspectRatio(aspect)
      .setViewRect(0, 0, exportWidth, exportHeight)
      .commit();
  rendering::game_camera.render(true);

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

  ~ScopedReplayVideoBgfxAccess() {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
    if (restoreResetFlags) {
      bgfx::reset(rendering::render_width, rendering::render_height,
                  originalResetFlags);
      restorePrimaryRenderViews(&context);
    }
#endif
    context.replayVideoExportActive.store(false, std::memory_order_release);
  }

  ScopedReplayVideoBgfxAccess(const ScopedReplayVideoBgfxAccess &) = delete;
  ScopedReplayVideoBgfxAccess &
  operator=(const ScopedReplayVideoBgfxAccess &) = delete;

private:
  ApplicationContext &context;
  std::unique_lock<std::mutex> lock;
  uint32_t originalResetFlags = 0;
  bool restoreResetFlags = false;
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
  if (std::filesystem::exists(basePath)) {
    return basePath;
  }

  for (const auto &ext : kAudioExtensions) {
    std::filesystem::path path = basePath;
    path.replace_extension(ext);
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return std::nullopt;
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
  return std::max(0LL, durationMicros) + kAudioTailMicros;
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
    events.push_back({event.songTimeMicros - keySoundOffsetMicros,
                      noteIt->second->Wav});
  }

  std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
    if (a.timeMicros != b.timeMicros) {
      return a.timeMicros < b.timeMicros;
    }
    return a.wav < b.wav;
  });
  return events;
}

DecodedSound *loadDecodedSound(
    const bms_parser::Chart &chart, int wav,
    std::unordered_map<int, DecodedSound> &decodedSounds,
    std::atomic_bool &isCancelled) {
  if (const auto decodedIt = decodedSounds.find(wav);
      decodedIt != decodedSounds.end()) {
    return &decodedIt->second;
  }

  const auto soundPath = resolveSoundPath(chart, wav);
  if (!soundPath.has_value()) {
    SDL_Log("Replay export missing sound %d", wav);
    return nullptr;
  }

  DecodedSound decoded;
  const auto resolvedPath = soundPath.value();
  if (!decodeAudioToPCM(fspath_to_path_t(resolvedPath), decoded.pcm,
                        decoded.info, isCancelled)) {
    SDL_Log("Replay export failed to decode sound %d: %s", wav,
            resolvedPath.string().c_str());
    return nullptr;
  }
  if (decoded.info.frames <= 0 || decoded.info.channels <= 0 ||
      decoded.info.samplerate <= 0) {
    SDL_Log("Replay export decoded invalid sound %d: %s", wav,
            resolvedPath.string().c_str());
    return nullptr;
  }

  auto [insertedIt, _] = decodedSounds.emplace(wav, std::move(decoded));
  return &insertedIt->second;
}

void ensureMixFrames(std::vector<float> &mix, size_t frames) {
  const size_t samples = frames * kExportChannels;
  if (mix.size() < samples) {
    mix.resize(samples, 0.0f);
  }
}

float sampleDecodedChannel(const DecodedSound &sound, size_t frame, int channel) {
  const int sourceChannels = sound.info.channels;
  const int sourceChannel =
      sourceChannels == 1 ? 0 : std::min(channel, sourceChannels - 1);
  const size_t sampleIndex =
      frame * static_cast<size_t>(sourceChannels) +
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
    const double sourcePosition = static_cast<double>(targetFrame) *
                                  sourceToTarget;
    const size_t sourceFrame0 = std::min(
        static_cast<size_t>(sourcePosition), sourceFrames > 0 ? sourceFrames - 1 : 0);
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
  SNDFILE *file = sf_wchar_open(path.wstring().c_str(), SFM_WRITE, &outputInfo);
#else
  SNDFILE *file = sf_open(path.string().c_str(), SFM_WRITE, &outputInfo);
#endif
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
      sf_writef_short(file, pcm.data(), framesToWrite);
  sf_close(file);

  if (framesWritten != framesToWrite) {
    errorMessage = "Failed to write complete replay audio track";
    return false;
  }
  return true;
}

ReplayVideoExportResult writeReplayAudioTrack(bms_parser::Chart &chart,
                                              const ReplayData &replay,
                                              const AppSettings &settings,
                                              const std::filesystem::path &path) {
  long long durationMicros = 0;
  const long long keySoundOffsetMicros =
      static_cast<long long>(settings.visualOffsetMs) * 1000LL;
  const auto audioEvents =
      collectAudioEvents(chart, replay, keySoundOffsetMicros, durationMicros);
  const size_t initialFrames = static_cast<size_t>(
      (static_cast<long double>(std::max(0LL, durationMicros)) *
       kExportSampleRate) /
          1000000.0L +
      1.0L);
  std::vector<float> mix(initialFrames * kExportChannels, 0.0f);
  std::unordered_map<int, DecodedSound> decodedSounds;
  std::atomic_bool isCancelled = false;

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
    }
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

bms_parser::Note *
findReplayNote(const std::unordered_map<std::string, bms_parser::Note *> &lookup,
               const ReplayEvent &event) {
  if (event.noteTimeMicros < 0) {
    return nullptr;
  }
  const auto it =
      lookup.find(replayNoteKey(event.lane, event.noteTimeMicros));
  return it == lookup.end() ? nullptr : it->second;
}

void applyReplayEventForVideo(
    BMSRenderer &renderer,
    const std::unordered_map<std::string, bms_parser::Note *> &lookup,
    const ReplayEvent &event, long long visualTimeMicros,
    bool gaugeAutoShift) {
  const JudgeResult recordedJudge(event.judgement, event.diffMicros);
  auto applyHud = [&]() {
    if (event.judgement == None) {
      return;
    }
    renderer.onJudge(recordedJudge, event.combo, event.score);
    renderer.setGaugeStatus(event.gaugeType, gaugeAutoShift, event.gauge);
  };

  switch (event.action) {
  case ReplayEventAction::Press: {
    bool suppressHudForLongNoteHead = false;
    if (auto *note = findReplayNote(lookup, event);
        note != nullptr) {
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
      applyHud();
    }
    renderer.onLanePressed(event.lane, recordedJudge, visualTimeMicros);
    break;
  }
  case ReplayEventAction::Release: {
    if (auto *note = findReplayNote(lookup, event);
        note != nullptr && note->IsLongNote() && event.judgement != None) {
      auto *longNote = static_cast<bms_parser::LongNote *>(note);
      if (longNote->IsTail() && longNote->IsHolding) {
        longNote->Release(event.judgeTimeMicros);
      }
    }
    applyHud();
    renderer.onLaneReleased(event.lane, visualTimeMicros);
    break;
  }
  case ReplayEventAction::Miss:
    applyHud();
    break;
  }
}

std::string ffmpegError(int errorCode) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(errorCode, buffer.data(), buffer.size()) < 0) {
    return "Unknown FFmpeg error";
  }
  return buffer.data();
}

const AVCodec *findReplayVideoEncoder() {
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
  const std::string codecName = codec != nullptr && codec->name != nullptr
                                    ? codec->name
                                    : "";
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
                 std::string &errorMessage,
                 int64_t forcedPacketDuration = 0) {
  int ret = avcodec_send_frame(encoderContext, frame);
  if (ret < 0) {
    errorMessage = "Failed to send frame to encoder: " + ffmpegError(ret);
    return false;
  }

  while (ret >= 0) {
    av_packet_unref(packet);
    ret = avcodec_receive_packet(encoderContext, packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return true;
    }
    if (ret < 0) {
      errorMessage = "Failed to receive encoded packet: " + ffmpegError(ret);
      return false;
    }

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
  }
  return true;
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
      errorMessage = std::string("Unsupported AAC sample format: ") +
                     formatName;
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

  bool open(const std::filesystem::path &wavPath,
            const std::filesystem::path &outputPath, int width, int height,
            int fps, std::string &errorMessage) {
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
      return failOpen("H.264 encoder was not found");
    }
    const auto videoPixelFormat = chooseVideoPixelFormat(videoCodec);
    if (!videoPixelFormat.has_value()) {
      return failOpen("H.264 encoder does not support a BGRA-convertible "
                      "format");
    }

    videoStream = avformat_new_stream(formatContext, nullptr);
    if (videoStream == nullptr) {
      return failOpen("Failed to create MP4 video stream");
    }
    videoContext = avcodec_alloc_context3(videoCodec);
    if (videoContext == nullptr) {
      return failOpen("Failed to allocate H.264 encoder");
    }
    videoContext->codec_id = AV_CODEC_ID_H264;
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
    const int h264Level = replayVideoH264Level(width, height, fps);
    videoContext->profile = kH264HighProfile;
    videoContext->level = h264Level;
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
    const std::string videoCodecName =
        videoCodec->name != nullptr ? videoCodec->name : "";
    if (videoCodecName == "libx264") {
      av_dict_set(&videoOptions, "preset", "veryfast", 0);
      av_dict_set(&videoOptions, "crf", "22", 0);
      av_dict_set(&videoOptions, "profile", "high", 0);
      const std::string levelString = replayVideoH264LevelString(h264Level);
      av_dict_set(&videoOptions, "level", levelString.c_str(), 0);
      const std::string threadCount =
          std::to_string(videoContext->thread_count);
      av_dict_set(&videoOptions, "threads", threadCount.c_str(), 0);
    } else if (videoCodecName.find("videotoolbox") != std::string::npos) {
      av_dict_set(&videoOptions, "realtime", "0", 0);
      av_dict_set(&videoOptions, "allow_sw", "1", 0);
    }
    ret = avcodec_open2(videoContext, videoCodec, &videoOptions);
    av_dict_free(&videoOptions);
    if (ret < 0) {
      return failOpen("Failed to open H.264 encoder: " + ffmpegError(ret));
    }
    videoFrameDuration = 1;
    const char *pixelFormatName = av_get_pix_fmt_name(videoContext->pix_fmt);
    SDL_Log("Replay video export encoder: %s, pixel format: %s, time base: "
            "%d/%d, frame duration: %lld, H.264 level: %s",
            videoCodec->name, pixelFormatName != nullptr ? pixelFormatName
                                                         : "unknown",
            videoContext->time_base.num, videoContext->time_base.den,
            static_cast<long long>(videoFrameDuration),
            replayVideoH264LevelString(h264Level).c_str());
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
    ret =
        av_channel_layout_copy(&audioFrame->ch_layout, &audioContext->ch_layout);
    if (ret < 0) {
      return failOpen("Failed to configure audio frame layout: " +
                      ffmpegError(ret));
    }
    ret = av_frame_get_buffer(audioFrame, 0);
    if (ret < 0) {
      return failOpen("Failed to allocate audio frame buffer: " +
                      ffmpegError(ret));
    }

    if (videoContext->pix_fmt != AV_PIX_FMT_BGRA) {
      swsContext = sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height,
                                  videoContext->pix_fmt, SWS_FAST_BILINEAR,
                                  nullptr, nullptr, nullptr);
      if (swsContext == nullptr) {
        return failOpen("Failed to create video pixel converter");
      }
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

    ret = av_dict_set(&formatContext->metadata,
                      kQuickTimeFullFrameRatePlaybackIntentKey, "1", 0);
    if (ret < 0) {
      return failOpen("Failed to set full-frame-rate playback metadata: " +
                      ffmpegError(ret));
    }

    AVDictionary *formatOptions = nullptr;
    av_dict_set(&formatOptions, "movflags", "+faststart+use_metadata_tags", 0);
    const std::string videoTrackTimescale = std::to_string(fps);
    av_dict_set(&formatOptions, "video_track_timescale",
                videoTrackTimescale.c_str(), 0);
    ret = avformat_write_header(formatContext, &formatOptions);
    av_dict_free(&formatOptions);
    if (ret < 0) {
      return failOpen("Failed to write MP4 header: " + ffmpegError(ret));
    }
    SDL_Log("Replay video export MP4 stream time base: %d/%d",
            videoStream->time_base.num, videoStream->time_base.den);

    return true;
  }

  bool encodeVideoFrame(const uint8_t *bgraFrame, size_t frameIndex,
                        long long videoTimeMicros,
                        std::string &errorMessage) {
    while (!audioFinished &&
           (nextAudioPts * 1000000LL) / kExportSampleRate <=
               videoTimeMicros) {
      if (!encodeNextAudioFrame(audioFile, audioContext, formatContext,
                                audioStream, audioFrame, audioPacket,
                                audioBuffer, nextAudioPts, audioFinished,
                                errorMessage)) {
        return false;
      }
    }

    int ret = av_frame_make_writable(videoFrame);
    if (ret < 0) {
      errorMessage = "Failed to prepare video frame: " + ffmpegError(ret);
      return false;
    }
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
    videoFrame->pts = static_cast<int64_t>(frameIndex);
    videoFrame->duration = videoFrameDuration;
    return encodeFrame(videoContext, formatContext, videoStream, videoFrame,
                       videoPacket, errorMessage, videoFrameDuration);
  }

  ReplayVideoExportResult finish() {
    std::string errorMessage;
    auto fail = [&](const std::string &message) {
      cleanup();
      return ReplayVideoExportResult{
          .success = false, .outputPath = outputPath, .message = message};
    };

    while (!audioFinished) {
      if (!encodeNextAudioFrame(audioFile, audioContext, formatContext,
                                audioStream, audioFrame, audioPacket,
                                audioBuffer, nextAudioPts, audioFinished,
                                errorMessage)) {
        return fail(errorMessage);
      }
    }
    if (!encodeFrame(videoContext, formatContext, videoStream, nullptr,
                     videoPacket, errorMessage, videoFrameDuration)) {
      return fail(errorMessage);
    }
    if (!encodeFrame(audioContext, formatContext, audioStream, nullptr,
                     audioPacket, errorMessage)) {
      return fail(errorMessage);
    }

    const int ret = av_write_trailer(formatContext);
    if (ret < 0) {
      return fail("Failed to write MP4 trailer: " + ffmpegError(ret));
    }

    cleanup();
    return {.success = true,
            .outputPath = outputPath,
            .message = "MP4 exported"};
  }

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
  int64_t nextAudioPts = 0;
  bool audioFinished = false;
};

class ReplayAsyncFrameEncoder {
public:
  ~ReplayAsyncFrameEncoder() { cancel(); }

  bool start(const std::filesystem::path &wavPath,
             const std::filesystem::path &outputPath, int width, int height,
             int fps, size_t frameBytes, size_t bufferCount,
             std::string &errorMessage) {
    if (!writer.open(wavPath, outputPath, width, height, fps, errorMessage)) {
      return false;
    }

    frameBuffers.assign(bufferCount, std::vector<uint8_t>(frameBytes));
    {
      std::lock_guard<std::mutex> lock(mutex);
      acceptingFrames = true;
      failed = false;
      cancelled = false;
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

  ReplayVideoExportResult finish() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      acceptingFrames = false;
    }
    condition.notify_all();
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

  ReplayMp4StreamWriter writer;
  std::vector<std::vector<uint8_t>> frameBuffers;
  std::deque<size_t> freeBuffers;
  std::deque<PendingFrame> pendingFrames;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::thread worker;
  std::atomic_llong encodedMicrosTotal{0};
  bool acceptingFrames = false;
  bool failed = false;
  bool cancelled = false;
  std::string failureMessage;
};

ReplayVideoExportResult renderReplayVideoToMp4(
    ApplicationContext &context, bms_parser::Chart &chart,
    const ReplayData &replay, const AppSettings &settings,
    const ReplayVideoExportOptions &options, const std::filesystem::path &wavPath,
    const std::filesystem::path &outputPath) {
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
  const uint64_t requiredCaps =
      BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK;
  if ((bgfx::getCaps()->supported & requiredCaps) != requiredCaps) {
    return {.success = false,
            .outputPath = outputPath,
            .message = "Renderer does not support texture readback"};
  }

  context.jukebox.setVisualOffsetMs(settings.visualOffsetMs);
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
  bgfx::TextureHandle readbackTexture = BGFX_INVALID_HANDLE;
  std::unique_ptr<rendering::BlurPass> bgaBlurPass;
  auto cleanupBgfx = [&]() {
    context.jukebox.stop();
    context.jukebox.unloadVisuals();
    restorePrimaryRenderViews(&context);
    if (bgaBlurPass != nullptr) {
      bgaBlurPass->shutdown();
      bgaBlurPass.reset();
    }
    if (bgfx::isValid(readbackTexture)) {
      bgfx::destroy(readbackTexture);
      readbackTexture = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(outputFrameBuffer)) {
      bgfx::destroy(outputFrameBuffer);
      outputFrameBuffer = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(outputTexture)) {
      bgfx::destroy(outputTexture);
    }
  };

  outputFrameBuffer = bgfx::createFrameBuffer(1, &outputTexture, false);
  if (!bgfx::isValid(outputFrameBuffer)) {
    cleanupBgfx();
    return {.success = false,
            .outputPath = outputPath,
            .message = "Failed to create replay export framebuffer"};
  }

  readbackTexture = bgfx::createTexture2D(
      static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
      bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_BLIT_DST |
                                      BGFX_TEXTURE_READ_BACK);
  if (!bgfx::isValid(readbackTexture)) {
    cleanupBgfx();
    return {.success = false,
            .outputPath = outputPath,
            .message = "Failed to create replay export readback texture"};
  }

  bgaBlurPass = std::make_unique<rendering::BlurPass>(2, 0.6f);
  bgaBlurPass->init(static_cast<uint16_t>(width),
                    static_cast<uint16_t>(height));
  bgaBlurPass->setInputViews(
      std::vector<bgfx::ViewId>(rendering::kGameplayBgaInputViews.begin(),
                                rendering::kGameplayBgaInputViews.end()));
  bgaBlurPass->setCompositeEnabled(false);
  bgaBlurPass->setBlurStrength(settings.bgaBlurStrength);

  configureReplayExportRenderViews(width, height, outputFrameBuffer,
                                   *bgaBlurPass, settings);

  ScopedChartNoteReset chartReset(chart);
  Judge judge(chart.Meta.Rank);
  BMSRenderer renderer(&chart, judge.timingWindows[Bad].second,
                       settings.visibleTimeGreenNumber);
  renderer.setLaneBeamClockUsesRenderTime(true);
  renderer.setGaugeStatus(replay.initialGaugeType, replay.gaugeAutoShift,
                          gaugeInitialValue(replay.initialGaugeType));
  renderer.setReplayData(&replay);

  const auto replayNotes = buildReplayNoteLookup(chart);
  const long long durationMicros = calculateExportDurationMicros(chart, replay);
  const long long visualOffsetMicros =
      static_cast<long long>(settings.visualOffsetMs) * 1000LL;
  const size_t frameCount = static_cast<size_t>(std::ceil(
      static_cast<long double>(durationMicros) * fps / 1000000.0L));
  const size_t frameBytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  ReplayAsyncFrameEncoder encoder;
  std::string errorMessage;
  const size_t frameBufferCount = replayVideoFrameBufferCount();
  if (!encoder.start(wavPath, outputPath, width, height, fps, frameBytes,
                     frameBufferCount, errorMessage)) {
    cleanupBgfx();
    return {.success = false, .outputPath = outputPath, .message = errorMessage};
  }
  SDL_Log("Replay video export frame buffers: %zu, encoder threads: %d",
          frameBufferCount, replayVideoEncoderThreadCount());

  RenderContext renderContext;
  size_t replayCursor = 0;
  uint32_t currentFrame = bgfx::frame();
  const auto exportStart = std::chrono::steady_clock::now();
  long long bufferWaitMicros = 0;
  long long renderReadbackMicros = 0;

  for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
    const auto bufferWaitStart = std::chrono::steady_clock::now();
    const int frameBufferIndex = encoder.acquireFrameBuffer(errorMessage);
    bufferWaitMicros += elapsedMicros(bufferWaitStart);
    if (frameBufferIndex < 0) {
      cleanupBgfx();
      return {.success = false,
              .outputPath = outputPath,
              .message = errorMessage};
    }

    const long long songTimeMicros = static_cast<long long>(
        (static_cast<long double>(frameIndex) * 1000000.0L) / fps);
    const long long visualTimeMicros =
        std::max(0LL, songTimeMicros - visualOffsetMicros);
    while (replayCursor < replay.events.size() &&
           replay.events[replayCursor].songTimeMicros <= songTimeMicros) {
      applyReplayEventForVideo(renderer, replayNotes,
                               replay.events[replayCursor],
                               visualTimeMicros, replay.gaugeAutoShift);
      ++replayCursor;
    }

    const auto renderStart = std::chrono::steady_clock::now();
    bgfx::touch(rendering::clear_view);
    bgfx::touch(rendering::bga_view);
    bgfx::touch(rendering::bga_layer_view);
    context.jukebox.renderVisualsAt(songTimeMicros);
    bgaBlurPass->execute();
    rendering::renderFullscreenTextureTint(
        bgaBlurPass->outputTexture(), rendering::final_view,
        static_cast<float>(settings.bgaBrightnessPercent) / 100.0f);
    renderer.render(renderContext, visualTimeMicros);
    bgfx::blit(rendering::readback_view, readbackTexture, 0, 0,
               outputTexture);
    currentFrame = bgfx::frame();
    const uint32_t expectedFrame =
        bgfx::readTexture(readbackTexture,
                          encoder.frameData(static_cast<size_t>(
                              frameBufferIndex)));
    while (currentFrame < expectedFrame) {
      currentFrame = bgfx::frame();
    }
    renderReadbackMicros += elapsedMicros(renderStart);

    if (!encoder.submitFrame(static_cast<size_t>(frameBufferIndex), frameIndex,
                             songTimeMicros, errorMessage)) {
      cleanupBgfx();
      return {.success = false,
              .outputPath = outputPath,
              .message = errorMessage};
    }

    if (frameIndex == 0 || (frameIndex + 1) % static_cast<size_t>(fps) == 0 ||
        frameIndex + 1 == frameCount) {
      SDL_Log("Replay video export encoded frame %zu/%zu", frameIndex + 1,
              frameCount);
    }
  }

  cleanupBgfx();
  auto result = encoder.finish();
  if (!result.success && result.outputPath.empty()) {
    result.outputPath = outputPath;
  }
  SDL_Log("Replay video export profile: %.2fs total, %.2fs render/readback, "
          "%.2fs encode worker, %.2fs waiting for frame buffers",
          static_cast<double>(elapsedMicros(exportStart)) / 1000000.0,
          static_cast<double>(renderReadbackMicros) / 1000000.0,
          static_cast<double>(encoder.encodedMicros()) / 1000000.0,
          static_cast<double>(bufferWaitMicros) / 1000000.0);
  return result;
}

ReplayVideoExportResult saveReplayVideoToPlatformLibrary(
    const ReplayVideoExportResult &muxResult) {
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
ReplayVideoExporter::Export(ApplicationContext &context, bms_parser::Chart *chart,
                            const ReplayData &replay,
                            const ReplayVideoExportOptions &options) {
  if (chart == nullptr) {
    return {.success = false, .message = "No chart selected"};
  }

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
  std::filesystem::create_directories(tempDir, ec);
  if (ec) {
    return {.success = false,
            .message = "Failed to create replay export work directory"};
  }

  const auto resolvedOptions = resolveReplayVideoExportOptions(options);

  const auto wavPath = tempDir / "audio.wav";
  const auto outputPath = outputDir / (baseName + ".mp4");

  SDL_Log("Replay export audio: %s", wavPath.string().c_str());
  auto audioResult =
      writeReplayAudioTrack(*chart, replay, context.settings, wavPath);
  if (!audioResult.success) {
    std::filesystem::remove_all(tempDir, ec);
    return audioResult;
  }

  SDL_Log("Replay export MP4: %s (%dx%d @ %dfps)",
          outputPath.string().c_str(), resolvedOptions.width,
          resolvedOptions.height, resolvedOptions.fps);
  auto muxResult = renderReplayVideoToMp4(
      context, *chart, replay, context.settings, resolvedOptions, wavPath,
      outputPath);
  if (!muxResult.success) {
    std::filesystem::remove(outputPath, ec);
    std::filesystem::remove_all(tempDir, ec);
    return muxResult;
  }

  std::filesystem::remove_all(tempDir, ec);
  if (ec) {
    SDL_Log("Replay export could not clean work directory: %s",
            tempDir.string().c_str());
  }

  auto platformSaveResult = saveReplayVideoToPlatformLibrary(muxResult);
  if (!platformSaveResult.success) {
    return platformSaveResult;
  }
  return platformSaveResult;
}
