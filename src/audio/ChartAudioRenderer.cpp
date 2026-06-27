#include "ChartAudioRenderer.h"

#include "../ArchiveFile.h"
#include "../ChartPlaybackDuration.h"
#include "../Utils.h"
#include "../path.h"
#include "ChartAssetExtensions.h"
#include "SoundFileIO.h"
#include "decoder.h"

#include <SDL2/SDL.h>
#include <sndfile.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace chart_audio {
namespace {

struct DecodedSound {
  std::vector<short> pcm;
  SF_INFO info{};
};

using DecodedSoundCache =
    std::unordered_map<int, std::shared_ptr<DecodedSound>>;

struct ArchiveAudioBatch {
  std::filesystem::path archivePath;
  std::vector<std::filesystem::path> innerPaths;
  std::unordered_map<path_t, std::vector<int>> wavIdsByPath;
};

long long elapsedMicros(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

std::string secondsString(long long micros, int precision = 2) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision)
         << static_cast<double>(micros) / 1000000.0;
  return stream.str();
}

void logMessage(const RenderOptions &options, const std::string &message) {
  if (options.log) {
    options.log(message);
    return;
  }
  SDL_Log("%s", message.c_str());
}

std::string replayNoteKey(int lane, long long noteTimeMicros) {
  return std::to_string(lane) + ":" + std::to_string(noteTimeMicros);
}

std::optional<std::filesystem::path>
resolveSoundPath(const bms_parser::Chart &chart, int wav) {
  const auto wavIt = chart.WavTable.find(wav);
  if (wavIt == chart.WavTable.end()) {
    return std::nullopt;
  }

  const std::filesystem::path basePath = chart.Meta.Folder / wavIt->second;
  std::vector<std::string_view> extensions(
      asobmshow::chart_assets::kAudioExtensions.begin(),
      asobmshow::chart_assets::kAudioExtensions.end());
  return archive_file::findFileWithExtensions(basePath, extensions);
}

std::optional<archive_file::EntryRange>
entryRangeForChartArchive(const bms_parser::Chart &chart,
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

bool addArchiveAudioTarget(
    std::unordered_map<path_t, ArchiveAudioBatch> &batches,
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
            .emplace(archiveKey, ArchiveAudioBatch{
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

bool readArchiveAudioBatch(
    const ArchiveAudioBatch &batch,
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

std::unordered_map<std::string, const bms_parser::Note *>
buildReplayNoteLookup(const bms_parser::Chart &chart) {
  std::unordered_map<std::string, const bms_parser::Note *> lookup;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (const auto *note : timeline->Notes) {
        if (note == nullptr) {
          continue;
        }
        lookup[replayNoteKey(note->Lane, timeline->Timing)] = note;
      }
      for (const auto *note : timeline->LandmineNotes) {
        if (note == nullptr) {
          continue;
        }
        lookup[replayNoteKey(note->Lane, timeline->Timing)] = note;
      }
    }
  }
  return lookup;
}

bool decodedSoundIsValid(const DecodedSound &decoded) {
  return decoded.info.frames > 0 && decoded.info.channels > 0 &&
         decoded.info.samplerate > 0;
}

void preloadArchivedDecodedSounds(const bms_parser::Chart &chart,
                                  const std::vector<AudioEvent> &audioEvents,
                                  DecodedSoundCache &decodedSounds,
                                  std::atomic_bool &isCancelled,
                                  const RenderOptions &options) {
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

  std::unordered_map<path_t, ArchiveAudioBatch> archiveBatches;
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
    if (addArchiveAudioTarget(archiveBatches, archiveBatchOrder, *soundPath,
                              wav)) {
      ++archivedWavCount;
    }
  }

  if (archiveBatches.empty()) {
    return;
  }

  logMessage(options, "Chart audio archived preload: " +
                          std::to_string(archivedWavCount) + " sounds, " +
                          std::to_string(archiveBatches.size()) +
                          " archive batch(es)");
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

    const ArchiveAudioBatch &batch = batchIt->second;
    std::vector<archive_file::FileData> files;
    std::string errorMessage;
    const auto range = entryRangeForChartArchive(chart, batch.archivePath);
    const auto readStart = std::chrono::steady_clock::now();
    if (!readArchiveAudioBatch(batch, range, files, &errorMessage)) {
      logMessage(options, "Chart audio archived preload failed: " +
                              fspath_to_utf8(batch.archivePath) +
                              ": " + errorMessage);
      continue;
    }
    logMessage(options, "Chart audio archive batch read: " +
                            fspath_to_utf8(batch.archivePath) +
                            " files=" + std::to_string(files.size()) +
                            " time=" + secondsString(elapsedMicros(readStart)) +
                            "s");

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
      logMessage(options, "Chart audio archive decode failures: " +
                              fspath_to_utf8(batch.archivePath) +
                              " count=" +
                              std::to_string(failedInBatch.load(
                                  std::memory_order_relaxed)));
    }
    logMessage(options, "Chart audio archive batch decode: " +
                            fspath_to_utf8(batch.archivePath) +
                            " time=" +
                            secondsString(elapsedMicros(decodeStart)) + "s");
  }
  logMessage(options, "Chart audio archived preload finished: decoded=" +
                          std::to_string(decodedCount) + " time=" +
                          secondsString(elapsedMicros(preloadStart)) + "s");
}

DecodedSound *loadDecodedSound(const bms_parser::Chart &chart, int wav,
                               DecodedSoundCache &decodedSounds,
                               std::atomic_bool &isCancelled) {
  if (const auto decodedIt = decodedSounds.find(wav);
      decodedIt != decodedSounds.end()) {
    return decodedIt->second.get();
  }

  const auto soundPath = resolveSoundPath(chart, wav);
  if (!soundPath.has_value()) {
    SDL_Log("Chart audio missing sound %d", wav);
    decodedSounds.emplace(wav, std::shared_ptr<DecodedSound>{});
    return nullptr;
  }

  auto decoded = std::make_shared<DecodedSound>();
  const auto resolvedPath = soundPath.value();
  if (!decodeAudioToPCM(fspath_to_path_t(resolvedPath), decoded->pcm,
                        decoded->info, isCancelled)) {
    SDL_Log("Chart audio failed to decode sound %d: %s", wav,
            fspath_to_utf8(resolvedPath).c_str());
    if (!isCancelled) {
      decodedSounds.emplace(wav, std::shared_ptr<DecodedSound>{});
    }
    return nullptr;
  }
  if (!decodedSoundIsValid(*decoded)) {
    SDL_Log("Chart audio decoded invalid sound %d: %s", wav,
            fspath_to_utf8(resolvedPath).c_str());
    decodedSounds.emplace(wav, std::shared_ptr<DecodedSound>{});
    return nullptr;
  }

  auto [insertedIt, _] = decodedSounds.emplace(wav, std::move(decoded));
  return insertedIt->second.get();
}

void ensureMixFrames(std::vector<float> &mix, std::size_t frames) {
  const std::size_t samples = frames * kOutputChannels;
  if (mix.size() < samples) {
    mix.resize(samples, 0.0f);
  }
}

std::size_t audioFramesForMicros(long long durationMicros) {
  if (durationMicros <= 0) {
    return 0;
  }
  return static_cast<std::size_t>(
      std::ceil(static_cast<long double>(durationMicros) * kOutputSampleRate /
                1000000.0L));
}

long long audioMicrosForFrames(std::size_t frames) {
  if (frames == 0) {
    return 0;
  }
  return static_cast<long long>(
      std::ceil(static_cast<long double>(frames) * 1000000.0L /
                kOutputSampleRate));
}

float sampleDecodedChannel(const DecodedSound &sound, std::size_t frame,
                           int channel) {
  const int sourceChannels = sound.info.channels;
  const int sourceChannel =
      sourceChannels == 1 ? 0 : std::min(channel, sourceChannels - 1);
  const std::size_t sampleIndex =
      frame * static_cast<std::size_t>(sourceChannels) +
      static_cast<std::size_t>(sourceChannel);
  if (sampleIndex >= sound.pcm.size()) {
    return 0.0f;
  }
  return static_cast<float>(sound.pcm[sampleIndex]) / 32768.0f;
}

void mixSoundAt(std::vector<float> &mix, const DecodedSound &sound,
                long long timeMicros) {
  const long long clampedTime = std::max(0LL, timeMicros);
  const std::size_t startFrame = static_cast<std::size_t>(
      (static_cast<long double>(clampedTime) * kOutputSampleRate) /
      1000000.0L);
  const std::size_t sourceFrames = static_cast<std::size_t>(sound.info.frames);
  const double sourceToTarget =
      static_cast<double>(sound.info.samplerate) / kOutputSampleRate;
  const std::size_t targetFrames = static_cast<std::size_t>(
      std::ceil(static_cast<double>(sourceFrames) / sourceToTarget));

  ensureMixFrames(mix, startFrame + targetFrames + 1);
  for (std::size_t targetFrame = 0; targetFrame < targetFrames;
       ++targetFrame) {
    const double sourcePosition =
        static_cast<double>(targetFrame) * sourceToTarget;
    const std::size_t sourceFrame0 =
        std::min(static_cast<std::size_t>(sourcePosition),
                 sourceFrames > 0 ? sourceFrames - 1 : 0);
    const std::size_t sourceFrame1 =
        std::min(sourceFrame0 + 1, sourceFrames > 0 ? sourceFrames - 1 : 0);
    const float fraction =
        static_cast<float>(sourcePosition - static_cast<double>(sourceFrame0));

    for (int channel = 0; channel < kOutputChannels; ++channel) {
      const float s0 = sampleDecodedChannel(sound, sourceFrame0, channel);
      const float s1 = sampleDecodedChannel(sound, sourceFrame1, channel);
      const float sample = s0 + (s1 - s0) * fraction;
      mix[(startFrame + targetFrame) * kOutputChannels + channel] += sample;
    }
  }
}

bool writeWavFile(const std::filesystem::path &path,
                  const std::vector<float> &mix, std::string &errorMessage) {
  SF_INFO outputInfo{};
  outputInfo.samplerate = kOutputSampleRate;
  outputInfo.channels = kOutputChannels;
  outputInfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

  auto file =
      asobmashow::audio::openSoundFileHandle(path, SFM_WRITE, outputInfo);
  if (file == nullptr) {
    errorMessage =
        std::string("Failed to open chart audio output: ") + sf_strerror(nullptr);
    return false;
  }

  std::vector<short> pcm;
  pcm.reserve(mix.size());
  for (const float sample : mix) {
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    pcm.push_back(static_cast<short>(std::lrint(clamped * 32767.0f)));
  }

  const sf_count_t framesToWrite =
      static_cast<sf_count_t>(pcm.size() / kOutputChannels);
  const sf_count_t framesWritten =
      sf_writef_short(file.get(), pcm.data(), framesToWrite);

  if (framesWritten != framesToWrite) {
    errorMessage = "Failed to write complete chart audio track";
    return false;
  }
  return true;
}

std::vector<AudioEvent>
resolveAudioEvents(const bms_parser::Chart &chart,
                   const RenderOptions &options) {
  switch (options.keySoundMode) {
  case KeySoundMode::BackgroundOnly:
    return CollectBackgroundAudioEvents(chart);
  case KeySoundMode::ChartTiming:
    return CollectChartTimedAudioEvents(chart);
  case KeySoundMode::ReplayTiming:
    if (options.replay == nullptr) {
      return {};
    }
    return CollectReplayTimedAudioEvents(chart, *options.replay,
                                         options.keySoundOffsetMicros);
  }
  return {};
}

long long baseDurationMicros(const bms_parser::Chart &chart,
                             const RenderOptions &options) {
  if (options.keySoundMode == KeySoundMode::ReplayTiming &&
      options.replay != nullptr) {
    return chart_playback_duration::ReplayTimelineEndMicros(chart,
                                                            *options.replay);
  }
  return chart_playback_duration::ChartTimelineEndMicros(chart);
}

} // namespace

std::vector<AudioEvent>
CollectBackgroundAudioEvents(const bms_parser::Chart &chart) {
  std::vector<AudioEvent> events;

  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (const auto *note : timeline->BackgroundNotes) {
        if (note == nullptr || note->Wav == bms_parser::Parser::NoWav) {
          continue;
        }
        events.push_back({timeline->Timing, note->Wav});
      }
    }
  }

  std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
    if (a.timeMicros != b.timeMicros) {
      return a.timeMicros < b.timeMicros;
    }
    return a.wav < b.wav;
  });
  return events;
}

std::vector<AudioEvent>
CollectChartTimedAudioEvents(const bms_parser::Chart &chart) {
  std::vector<AudioEvent> events;

  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (const auto *note : timeline->Notes) {
        if (note == nullptr || note->Wav == bms_parser::Parser::NoWav) {
          continue;
        }
        events.push_back({timeline->Timing, note->Wav});
      }
      for (const auto *note : timeline->BackgroundNotes) {
        if (note == nullptr || note->Wav == bms_parser::Parser::NoWav) {
          continue;
        }
        events.push_back({timeline->Timing, note->Wav});
      }
    }
  }

  std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
    if (a.timeMicros != b.timeMicros) {
      return a.timeMicros < b.timeMicros;
    }
    return a.wav < b.wav;
  });
  return events;
}

std::vector<AudioEvent>
CollectReplayTimedAudioEvents(const bms_parser::Chart &chart,
                              const ReplayData &replay,
                              long long keySoundOffsetMicros) {
  std::vector<AudioEvent> events = CollectBackgroundAudioEvents(chart);

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

RenderResult RenderChartAudioToWav(const bms_parser::Chart &chart,
                                   const std::filesystem::path &path,
                                   const RenderOptions &options) {
  if (options.keySoundMode == KeySoundMode::ReplayTiming &&
      options.replay == nullptr) {
    return {.success = false,
            .outputPath = path,
            .message = "Replay-timed chart audio requires replay data"};
  }

  std::atomic_bool localCancelled = false;
  std::atomic_bool &isCancelled =
      options.isCancelled == nullptr ? localCancelled : *options.isCancelled;

  const auto audioEvents = resolveAudioEvents(chart, options);
  const long long baseDuration = baseDurationMicros(chart, options);
  const std::size_t initialFrames = audioFramesForMicros(baseDuration);
  std::vector<float> mix(initialFrames * kOutputChannels, 0.0f);
  DecodedSoundCache decodedSounds;
  preloadArchivedDecodedSounds(chart, audioEvents, decodedSounds, isCancelled,
                               options);

  for (const auto &event : audioEvents) {
    if (isCancelled) {
      return {.success = false,
              .outputPath = path,
              .message = "Chart audio render cancelled",
              .durationMicros = audioMicrosForFrames(mix.size() /
                                                     kOutputChannels),
              .eventCount = audioEvents.size()};
    }
    DecodedSound *sound =
        loadDecodedSound(chart, event.wav, decodedSounds, isCancelled);
    if (sound == nullptr) {
      continue;
    }
    mixSoundAt(mix, *sound, event.timeMicros);
  }
  if (mix.empty()) {
    ensureMixFrames(mix, 1);
  }
  const long long durationMicros =
      audioMicrosForFrames(mix.size() / kOutputChannels);
  logMessage(options, "Chart audio duration: " +
                          secondsString(durationMicros, 3) +
                          "s base=" + secondsString(baseDuration, 3) +
                          "s events=" + std::to_string(audioEvents.size()));

  std::string errorMessage;
  if (!writeWavFile(path, mix, errorMessage)) {
    return {.success = false,
            .outputPath = path,
            .message = errorMessage,
            .durationMicros = durationMicros,
            .eventCount = audioEvents.size()};
  }
  return {.success = true,
          .outputPath = path,
          .message = "Audio exported",
          .durationMicros = durationMicros,
          .eventCount = audioEvents.size()};
}

} // namespace chart_audio
