#include "ReplayVideoExporter.h"

#include "Utils.h"
#include "audio/decoder.h"
#include "path.h"

#include <SDL2/SDL.h>
#include <sndfile.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
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

std::vector<AudioEvent> collectAudioEvents(bms_parser::Chart &chart,
                                           const ReplayData &replay,
                                           long long &durationMicros) {
  std::vector<AudioEvent> events;
  durationMicros = std::max(chart.Meta.TotalLength, chart.Meta.PlayLength);

  for (const auto &measure : chart.Measures) {
    for (const auto &timeline : measure->TimeLines) {
      durationMicros = std::max(durationMicros, timeline->Timing);
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
    durationMicros = std::max(durationMicros, event.songTimeMicros);
    durationMicros = std::max(durationMicros, event.noteTimeMicros);
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
  durationMicros += kAudioTailMicros;
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
} // namespace

ReplayVideoExportResult
ReplayVideoExporter::Export(ApplicationContext &context, bms_parser::Chart *chart,
                            const ReplayData &replay,
                            const ReplayVideoExportOptions &options) {
  (void)context;
  (void)options;
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
  const auto wavPath = outputDir / (baseName + ".wav");
  return writeReplayAudioTrack(*chart, replay, wavPath);
}
