#include "ReplayVideoExporter.h"

#include "Utils.h"
#include "audio/decoder.h"
#include "path.h"
#include "rendering/RenderPlan.h"
#include "rendering/common.h"
#include "scene/play/BMSRenderer.h"
#include "scene/play/Judge.h"
#include "targets.h"

#include <SDL2/SDL.h>
#include <bgfx/bgfx.h>
#include <sndfile.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
constexpr int kExportSampleRate = 44100;
constexpr int kExportChannels = 2;
constexpr long long kAudioTailMicros = 3000000;
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
    events.push_back({event.songTimeMicros, noteIt->second->Wav});
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
                                              const std::filesystem::path &path) {
  long long durationMicros = 0;
  const auto audioEvents = collectAudioEvents(chart, replay, durationMicros);
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
    const ReplayEvent &event, long long visualTimeMicros) {
  const JudgeResult recordedJudge(event.judgement, event.diffMicros);
  switch (event.action) {
  case ReplayEventAction::Press: {
    if (auto *note = findReplayNote(lookup, event);
        note != nullptr && recordedJudge.isNotePlayed()) {
      if (note->IsLongNote()) {
        auto *longNote = static_cast<bms_parser::LongNote *>(note);
        if (!longNote->IsTail()) {
          longNote->Press(event.judgeTimeMicros);
        }
      } else {
        note->Press(event.judgeTimeMicros);
      }
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
    renderer.onLaneReleased(event.lane, visualTimeMicros);
    break;
  }
  case ReplayEventAction::Miss:
    break;
  }
}

ReplayVideoExportResult captureReplayFrames(bms_parser::Chart &chart,
                                            const ReplayData &replay,
                                            const AppSettings &settings,
                                            const ReplayVideoExportOptions &options,
                                            const std::filesystem::path &rawPath) {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  (void)chart;
  (void)replay;
  (void)settings;
  (void)options;
  (void)rawPath;
  return {.success = false,
          .message = "Replay video export is not supported on iOS yet"};
#else
  const uint64_t requiredCaps =
      BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK;
  if ((bgfx::getCaps()->supported & requiredCaps) != requiredCaps) {
    return {.success = false,
            .message = "Renderer does not support texture readback"};
  }

  int width = options.width > 0 ? options.width : rendering::render_width;
  int height = options.height > 0 ? options.height : rendering::render_height;
  int fps = options.fps > 0 ? options.fps : 60;
  width = std::max(2, width & ~1);
  height = std::max(2, height & ~1);

  if (width > UINT16_MAX || height > UINT16_MAX) {
    return {.success = false, .message = "Replay export size is too large"};
  }

  const auto outputTexture = bgfx::createTexture2D(
      static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
      bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
  if (!bgfx::isValid(outputTexture)) {
    return {.success = false,
            .message = "Failed to create replay export render target"};
  }

  bgfx::FrameBufferHandle outputFrameBuffer = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle readbackTexture = BGFX_INVALID_HANDLE;
  auto cleanupBgfx = [&]() {
    bgfx::setViewFrameBuffer(rendering::clear_view, BGFX_INVALID_HANDLE);
    bgfx::setViewFrameBuffer(rendering::main_view, BGFX_INVALID_HANDLE);
    bgfx::setViewRect(rendering::clear_view, 0, 0,
                      static_cast<uint16_t>(rendering::render_width),
                      static_cast<uint16_t>(rendering::render_height));
    bgfx::setViewRect(rendering::main_view, rendering::ui_offset_x,
                      rendering::ui_offset_y,
                      static_cast<uint16_t>(rendering::ui_view_width),
                      static_cast<uint16_t>(rendering::ui_view_height));
    rendering::game_camera.render(true);
    if (bgfx::isValid(readbackTexture)) {
      bgfx::destroy(readbackTexture);
    }
    if (bgfx::isValid(outputFrameBuffer)) {
      bgfx::destroy(outputFrameBuffer);
    }
    if (bgfx::isValid(outputTexture)) {
      bgfx::destroy(outputTexture);
    }
  };

  outputFrameBuffer = bgfx::createFrameBuffer(1, &outputTexture, false);
  if (!bgfx::isValid(outputFrameBuffer)) {
    cleanupBgfx();
    return {.success = false,
            .message = "Failed to create replay export framebuffer"};
  }

  readbackTexture = bgfx::createTexture2D(
      static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
      bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_BLIT_DST |
                                      BGFX_TEXTURE_READ_BACK);
  if (!bgfx::isValid(readbackTexture)) {
    cleanupBgfx();
    return {.success = false,
            .message = "Failed to create replay export readback texture"};
  }

  std::ofstream rawOutput(rawPath, std::ios::binary);
  if (!rawOutput) {
    cleanupBgfx();
    return {.success = false,
            .message = "Failed to open replay export frame file"};
  }

  ScopedChartNoteReset chartReset(chart);
  Judge judge(chart.Meta.Rank);
  BMSRenderer renderer(&chart, judge.timingWindows[Bad].second,
                       settings.visibleTimeGreenNumber, false);
  renderer.setLaneBeamsEnabled(false);
  renderer.setReplayData(&replay);

  const auto replayNotes = buildReplayNoteLookup(chart);
  const long long durationMicros = calculateExportDurationMicros(chart, replay);
  const size_t frameCount = static_cast<size_t>(std::ceil(
      static_cast<long double>(durationMicros) * fps / 1000000.0L));
  const size_t frameBytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  std::vector<uint8_t> frameBuffer(frameBytes);
  RenderContext renderContext;
  size_t replayCursor = 0;
  uint32_t currentFrame = bgfx::frame();

  bgfx::setViewFrameBuffer(rendering::clear_view, outputFrameBuffer);
  bgfx::setViewFrameBuffer(rendering::main_view, outputFrameBuffer);
  bgfx::setViewRect(rendering::clear_view, 0, 0, static_cast<uint16_t>(width),
                    static_cast<uint16_t>(height));
  bgfx::setViewRect(rendering::main_view, 0, 0, static_cast<uint16_t>(width),
                    static_cast<uint16_t>(height));

  for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
    const long long songTimeMicros = static_cast<long long>(
        (static_cast<long double>(frameIndex) * 1000000.0L) / fps);
    while (replayCursor < replay.events.size() &&
           replay.events[replayCursor].songTimeMicros <= songTimeMicros) {
      applyReplayEventForVideo(renderer, replayNotes, replay.events[replayCursor],
                               songTimeMicros);
      ++replayCursor;
    }

    bgfx::touch(rendering::clear_view);
    renderer.render(renderContext, songTimeMicros);
    bgfx::blit(rendering::ui_view, readbackTexture, 0, 0, outputTexture);
    currentFrame = bgfx::frame();
    const uint32_t expectedFrame =
        bgfx::readTexture(readbackTexture, frameBuffer.data());
    while (currentFrame < expectedFrame) {
      currentFrame = bgfx::frame();
    }
    rawOutput.write(reinterpret_cast<const char *>(frameBuffer.data()),
                    static_cast<std::streamsize>(frameBuffer.size()));
    if (!rawOutput) {
      cleanupBgfx();
      return {.success = false,
              .message = "Failed while writing replay export frames"};
    }

    if (frameIndex == 0 || (frameIndex + 1) % static_cast<size_t>(fps) == 0 ||
        frameIndex + 1 == frameCount) {
      SDL_Log("Replay video export frame %zu/%zu", frameIndex + 1,
              frameCount);
    }
  }

  rawOutput.close();
  cleanupBgfx();
  return {.success = true, .outputPath = rawPath, .message = "Frames exported"};
#endif
}

std::string shellQuote(const std::string &value) {
#ifdef _WIN32
  std::string quoted = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      quoted += "\\\"";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('"');
  return quoted;
#else
  std::string quoted = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('\'');
  return quoted;
#endif
}

std::string shellQuote(const std::filesystem::path &path) {
  return shellQuote(path.string());
}

std::optional<std::string> findFfmpegExecutable() {
  if (const char *envPath = std::getenv("ASOBMASHOW_FFMPEG");
      envPath != nullptr && envPath[0] != '\0') {
    if (std::filesystem::exists(envPath)) {
      return std::string(envPath);
    }
  }

#ifdef _WIN32
  const std::array<std::string, 1> candidates = {"ffmpeg.exe"};
#else
  const std::array<std::string, 4> candidates = {
      "ffmpeg", "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg",
      "/usr/bin/ffmpeg"};
#endif
  for (const auto &candidate : candidates) {
    if (candidate.find('/') != std::string::npos ||
        candidate.find('\\') != std::string::npos) {
      if (std::filesystem::exists(candidate)) {
        return candidate;
      }
      continue;
    }

#ifdef _WIN32
    const std::string command = shellQuote(candidate) + " -version >NUL 2>NUL";
#else
    const std::string command =
        shellQuote(candidate) + " -version >/dev/null 2>&1";
#endif
    if (std::system(command.c_str()) == 0) {
      return candidate;
    }
  }
  return std::nullopt;
}

int runFfmpegCommand(const std::string &ffmpeg,
                     const std::filesystem::path &rawPath,
                     const std::filesystem::path &wavPath,
                     const std::filesystem::path &outputPath, int width,
                     int height, int fps, const std::string &videoCodec) {
  std::ostringstream command;
  command << shellQuote(ffmpeg) << " -y -v error"
          << " -f rawvideo -pix_fmt bgra -s " << width << "x" << height
          << " -r " << fps << " -i " << shellQuote(rawPath) << " -i "
          << shellQuote(wavPath) << " -c:v " << videoCodec;
  if (videoCodec == "libx264") {
    command << " -preset veryfast -crf 18";
  } else {
    command << " -b:v 8M";
  }
  command << " -pix_fmt yuv420p"
          << " -c:a aac -b:a 192k -shortest -movflags +faststart "
          << shellQuote(outputPath);
  return std::system(command.str().c_str());
}

ReplayVideoExportResult muxReplayVideo(const std::filesystem::path &rawPath,
                                        const std::filesystem::path &wavPath,
                                        const std::filesystem::path &outputPath,
                                        int width, int height, int fps) {
  const auto ffmpeg = findFfmpegExecutable();
  if (!ffmpeg.has_value()) {
    return {.success = false,
            .message = "ffmpeg executable was not found in PATH"};
  }

  int result = runFfmpegCommand(*ffmpeg, rawPath, wavPath, outputPath, width,
                                height, fps, "libx264");
  if (result != 0) {
    result = runFfmpegCommand(*ffmpeg, rawPath, wavPath, outputPath, width,
                              height, fps, "h264");
  }
  if (result != 0 || !std::filesystem::exists(outputPath)) {
    return {.success = false,
            .outputPath = outputPath,
            .message = "ffmpeg failed to mux replay video"};
  }
  return {.success = true, .outputPath = outputPath, .message = "MP4 exported"};
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

  int width = options.width > 0 ? options.width : rendering::render_width;
  int height = options.height > 0 ? options.height : rendering::render_height;
  int fps = options.fps > 0 ? options.fps : 60;
  width = std::max(2, width & ~1);
  height = std::max(2, height & ~1);

  const auto wavPath = tempDir / "audio.wav";
  const auto rawPath = tempDir / "video.bgra";
  const auto outputPath = outputDir / (baseName + ".mp4");

  SDL_Log("Replay export audio: %s", wavPath.string().c_str());
  auto audioResult = writeReplayAudioTrack(*chart, replay, wavPath);
  if (!audioResult.success) {
    return audioResult;
  }

  SDL_Log("Replay export video frames: %s", rawPath.string().c_str());
  auto frameResult =
      captureReplayFrames(*chart, replay, context.settings,
                          {.width = width, .height = height, .fps = fps},
                          rawPath);
  if (!frameResult.success) {
    frameResult.outputPath = tempDir;
    return frameResult;
  }

  SDL_Log("Replay export mux: %s", outputPath.string().c_str());
  auto muxResult = muxReplayVideo(rawPath, wavPath, outputPath, width, height, fps);
  if (!muxResult.success) {
    muxResult.outputPath = tempDir;
    return muxResult;
  }

  std::filesystem::remove_all(tempDir, ec);
  if (ec) {
    SDL_Log("Replay export could not clean work directory: %s",
            tempDir.string().c_str());
  }
  return muxResult;
}
