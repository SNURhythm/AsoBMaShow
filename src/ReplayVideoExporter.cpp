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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
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
}

std::string ffmpegError(int errorCode) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(errorCode, buffer.data(), buffer.size()) < 0) {
    return "Unknown FFmpeg error";
  }
  return buffer.data();
}

const AVCodec *findReplayVideoEncoder() {
  if (const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
      codec != nullptr) {
    return codec;
  }
  if (const AVCodec *codec = avcodec_find_encoder_by_name("h264_videotoolbox");
      codec != nullptr) {
    return codec;
  }
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
  const std::array<AVPixelFormat, 3> preferredFormats = {
      AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_BGRA};
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
                 std::string &errorMessage) {
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

ReplayVideoExportResult muxReplayVideo(const std::filesystem::path &rawPath,
                                        const std::filesystem::path &wavPath,
                                        const std::filesystem::path &outputPath,
                                        int width, int height, int fps) {
  std::error_code ec;
  const auto rawSize = std::filesystem::file_size(rawPath, ec);
  const size_t frameBytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  if (ec || frameBytes == 0 || rawSize % frameBytes != 0) {
    return {.success = false,
            .outputPath = rawPath,
            .message = "Replay export frame file is invalid"};
  }
  const size_t frameCount = static_cast<size_t>(rawSize / frameBytes);
  if (frameCount == 0) {
    return {.success = false,
            .outputPath = rawPath,
            .message = "Replay export has no video frames"};
  }

  const std::string outputPathString = outputPath.string();
  AVFormatContext *formatContext = nullptr;
  int ret = avformat_alloc_output_context2(&formatContext, nullptr, "mp4",
                                           outputPathString.c_str());
  if (ret < 0 || formatContext == nullptr) {
    return {.success = false,
            .outputPath = outputPath,
            .message = "Failed to create MP4 muxer: " + ffmpegError(ret)};
  }

  AVCodecContext *videoContext = nullptr;
  AVCodecContext *audioContext = nullptr;
  AVFrame *videoFrame = nullptr;
  AVFrame *audioFrame = nullptr;
  AVPacket *videoPacket = nullptr;
  AVPacket *audioPacket = nullptr;
  SwsContext *swsContext = nullptr;
  SNDFILE *audioFile = nullptr;
  std::ifstream rawInput;

  auto cleanup = [&]() {
    if (audioFile != nullptr) {
      sf_close(audioFile);
    }
    if (swsContext != nullptr) {
      sws_freeContext(swsContext);
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
  };

  std::string errorMessage;
  auto fail = [&](const std::string &message) {
    cleanup();
    return ReplayVideoExportResult{
        .success = false, .outputPath = outputPath, .message = message};
  };

  const AVCodec *videoCodec = findReplayVideoEncoder();
  if (videoCodec == nullptr) {
    return fail("H.264 encoder was not found");
  }
  const auto videoPixelFormat = chooseVideoPixelFormat(videoCodec);
  if (!videoPixelFormat.has_value()) {
    return fail("H.264 encoder does not support a BGRA-convertible format");
  }

  AVStream *videoStream = avformat_new_stream(formatContext, nullptr);
  if (videoStream == nullptr) {
    return fail("Failed to create MP4 video stream");
  }
  videoContext = avcodec_alloc_context3(videoCodec);
  if (videoContext == nullptr) {
    return fail("Failed to allocate H.264 encoder");
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
  videoContext->bit_rate = 8000000;
  if (formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
    videoContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  AVDictionary *videoOptions = nullptr;
  if (std::string(videoCodec->name) == "libx264") {
    av_dict_set(&videoOptions, "preset", "veryfast", 0);
    av_dict_set(&videoOptions, "crf", "18", 0);
  }
  ret = avcodec_open2(videoContext, videoCodec, &videoOptions);
  av_dict_free(&videoOptions);
  if (ret < 0) {
    return fail("Failed to open H.264 encoder: " + ffmpegError(ret));
  }
  ret = avcodec_parameters_from_context(videoStream->codecpar, videoContext);
  if (ret < 0) {
    return fail("Failed to configure MP4 video stream: " + ffmpegError(ret));
  }
  videoStream->time_base = videoContext->time_base;

  const AVCodec *audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (audioCodec == nullptr) {
    return fail("AAC encoder was not found");
  }
  const auto audioSampleFormat = chooseAudioSampleFormat(audioCodec);
  if (!audioSampleFormat.has_value()) {
    return fail("AAC encoder does not support replay export sample formats");
  }

  AVStream *audioStream = avformat_new_stream(formatContext, nullptr);
  if (audioStream == nullptr) {
    return fail("Failed to create MP4 audio stream");
  }
  audioContext = avcodec_alloc_context3(audioCodec);
  if (audioContext == nullptr) {
    return fail("Failed to allocate AAC encoder");
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
    return fail("Failed to configure AAC channel layout: " + ffmpegError(ret));
  }
  if (formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
    audioContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  ret = avcodec_open2(audioContext, audioCodec, nullptr);
  if (ret < 0) {
    return fail("Failed to open AAC encoder: " + ffmpegError(ret));
  }
  ret = avcodec_parameters_from_context(audioStream->codecpar, audioContext);
  if (ret < 0) {
    return fail("Failed to configure MP4 audio stream: " + ffmpegError(ret));
  }
  audioStream->time_base = audioContext->time_base;

  if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&formatContext->pb, outputPathString.c_str(),
                   AVIO_FLAG_WRITE);
    if (ret < 0) {
      return fail("Failed to open MP4 output: " + ffmpegError(ret));
    }
  }

  AVDictionary *formatOptions = nullptr;
  av_dict_set(&formatOptions, "movflags", "+faststart", 0);
  ret = avformat_write_header(formatContext, &formatOptions);
  av_dict_free(&formatOptions);
  if (ret < 0) {
    return fail("Failed to write MP4 header: " + ffmpegError(ret));
  }

  rawInput.open(rawPath, std::ios::binary);
  if (!rawInput) {
    return fail("Failed to open replay export frame file");
  }
  SF_INFO audioInfo{};
#ifdef _WIN32
  audioFile = sf_wchar_open(wavPath.wstring().c_str(), SFM_READ, &audioInfo);
#else
  audioFile = sf_open(wavPath.string().c_str(), SFM_READ, &audioInfo);
#endif
  if (audioFile == nullptr) {
    return fail(std::string("Failed to open replay audio track: ") +
                sf_strerror(nullptr));
  }
  if (audioInfo.channels != kExportChannels ||
      audioInfo.samplerate != kExportSampleRate) {
    return fail("Replay audio track format is invalid");
  }

  videoFrame = av_frame_alloc();
  if (videoFrame == nullptr) {
    return fail("Failed to allocate video frame");
  }
  videoFrame->format = videoContext->pix_fmt;
  videoFrame->width = width;
  videoFrame->height = height;
  ret = av_frame_get_buffer(videoFrame, 32);
  if (ret < 0) {
    return fail("Failed to allocate video frame buffer: " + ffmpegError(ret));
  }

  const int audioFrameSize =
      audioContext->frame_size > 0 ? audioContext->frame_size : 1024;
  audioFrame = av_frame_alloc();
  if (audioFrame == nullptr) {
    return fail("Failed to allocate audio frame");
  }
  audioFrame->format = audioContext->sample_fmt;
  audioFrame->sample_rate = audioContext->sample_rate;
  audioFrame->nb_samples = audioFrameSize;
  ret = av_channel_layout_copy(&audioFrame->ch_layout, &audioContext->ch_layout);
  if (ret < 0) {
    return fail("Failed to configure audio frame layout: " + ffmpegError(ret));
  }
  ret = av_frame_get_buffer(audioFrame, 0);
  if (ret < 0) {
    return fail("Failed to allocate audio frame buffer: " + ffmpegError(ret));
  }

  swsContext = sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height,
                              videoContext->pix_fmt, SWS_BILINEAR, nullptr,
                              nullptr, nullptr);
  if (swsContext == nullptr) {
    return fail("Failed to create video pixel converter");
  }

  videoPacket = av_packet_alloc();
  audioPacket = av_packet_alloc();
  if (videoPacket == nullptr || audioPacket == nullptr) {
    return fail("Failed to allocate encoder packet");
  }

  std::vector<uint8_t> rawFrame(frameBytes);
  std::vector<float> audioBuffer(
      static_cast<size_t>(audioFrameSize) * kExportChannels, 0.0f);
  int64_t nextAudioPts = 0;
  bool audioFinished = false;

  for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
    const long long videoTimeMicros = static_cast<long long>(
        (static_cast<long double>(frameIndex) * 1000000.0L) / fps);
    while (!audioFinished &&
           (nextAudioPts * 1000000LL) / kExportSampleRate <=
               videoTimeMicros) {
      if (!encodeNextAudioFrame(audioFile, audioContext, formatContext,
                                audioStream, audioFrame, audioPacket,
                                audioBuffer, nextAudioPts, audioFinished,
                                errorMessage)) {
        return fail(errorMessage);
      }
    }

    rawInput.read(reinterpret_cast<char *>(rawFrame.data()),
                  static_cast<std::streamsize>(rawFrame.size()));
    if (rawInput.gcount() != static_cast<std::streamsize>(rawFrame.size())) {
      return fail("Replay export frame file ended unexpectedly");
    }

    ret = av_frame_make_writable(videoFrame);
    if (ret < 0) {
      return fail("Failed to prepare video frame: " + ffmpegError(ret));
    }
    const uint8_t *sourceData[4] = {rawFrame.data(), nullptr, nullptr, nullptr};
    const int sourceLinesize[4] = {width * 4, 0, 0, 0};
    sws_scale(swsContext, sourceData, sourceLinesize, 0, height,
              videoFrame->data, videoFrame->linesize);
    videoFrame->pts = static_cast<int64_t>(frameIndex);
    if (!encodeFrame(videoContext, formatContext, videoStream, videoFrame,
                     videoPacket, errorMessage)) {
      return fail(errorMessage);
    }
  }

  while (!audioFinished) {
    if (!encodeNextAudioFrame(audioFile, audioContext, formatContext,
                              audioStream, audioFrame, audioPacket,
                              audioBuffer, nextAudioPts, audioFinished,
                              errorMessage)) {
      return fail(errorMessage);
    }
  }
  if (!encodeFrame(videoContext, formatContext, videoStream, nullptr,
                   videoPacket, errorMessage)) {
    return fail(errorMessage);
  }
  if (!encodeFrame(audioContext, formatContext, audioStream, nullptr,
                   audioPacket, errorMessage)) {
    return fail(errorMessage);
  }

  ret = av_write_trailer(formatContext);
  if (ret < 0) {
    return fail("Failed to write MP4 trailer: " + ffmpegError(ret));
  }

  cleanup();
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
