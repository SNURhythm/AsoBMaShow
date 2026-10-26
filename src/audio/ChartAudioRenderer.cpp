#include "ChartAudioRenderer.h"
#include "AudioFrameTiming.h"
#include "BmsonAudio.h"

#include "../ArchiveFile.h"
#include "../AtomicFile.h"
#include "../ChartPlaybackDuration.h"
#include "../RAII.h"
#include "../Utils.h"
#include "../Uuid.h"
#include "../path.h"
#include "../scene/play/ReplayKeysoundSchedule.h"
#include "ChartAssetExtensions.h"
#include "ClubBeat.h"
#include "PrepMetronomeSound.h"
#include "PcmResampler.h"
#include "SoundFileIO.h"
#include "decoder.h"

#include <SDL3/SDL.h>
#include <sndfile.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
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

DecodedSound decodedClubSound(const club_beat::StereoSound &sound) {
  DecodedSound result;
  result.info.channels = kOutputChannels;
  result.info.samplerate = sound.sampleRate;
  result.info.frames = static_cast<sf_count_t>(sound.samples.size() /
                                               kOutputChannels);
  result.pcm.reserve(sound.samples.size());
  for (const float sample : sound.samples) {
    result.pcm.push_back(static_cast<short>(
        std::clamp(sample, -1.0f, 1.0f) * static_cast<float>(INT16_MAX)));
  }
  return result;
}

DecodedSound decodedGeneratedPcm(std::vector<short> pcm, int sampleRate,
                                 int channels) {
  DecodedSound result;
  result.pcm = std::move(pcm);
  result.info.channels = channels;
  result.info.samplerate = sampleRate;
  result.info.frames = static_cast<sf_count_t>(
      result.pcm.size() / static_cast<std::size_t>(channels));
  return result;
}

using DecodedSoundCache =
    std::unordered_map<int, std::shared_ptr<DecodedSound>>;

struct ArchiveAudioBatch {
  std::filesystem::path archivePath;
  std::vector<std::filesystem::path> innerPaths;
  std::unordered_map<path_t, std::vector<int>> wavIdsByPath;
};

constexpr std::uint64_t kArchiveAudioMaxInFlightBytes =
    64ull * 1024ull * 1024ull;

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

std::optional<std::filesystem::path>
resolveSoundPath(const bms_parser::Chart &chart, int wav) {
  const auto wavIt = chart.WavTable.find(wav);
  if (wavIt == chart.WavTable.end()) {
    return std::nullopt;
  }

  const std::filesystem::path basePath =
      chart.Meta.Folder / utf8_to_path_t(wavIt->second);
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

bool readArchiveAudioBatch(const ArchiveAudioBatch &batch,
                           const std::optional<archive_file::EntryRange> &range,
                           std::vector<archive_file::FileData> &files,
                           std::string *errorMessage,
                           std::atomic_bool &isCancelled) {
  files.clear();
  const std::size_t workerCount =
      static_cast<std::size_t>(parallel_worker_count(batch.innerPaths.size()));
  if (workerCount > 1) {
    std::mutex filesMutex;
    std::vector<archive_file::FileData> concurrentFiles;
    concurrentFiles.reserve(batch.innerPaths.size());
    std::string concurrentError;
    auto onFile = [&](archive_file::FileData &&file) {
      if (isCancelled.load(std::memory_order_relaxed)) {
        return false;
      }
      std::lock_guard<std::mutex> lock(filesMutex);
      concurrentFiles.push_back(std::move(file));
      return true;
    };
    const bool readOk = archive_file::readArchiveEntriesConcurrently(
        batch.archivePath, batch.innerPaths, std::move(onFile), workerCount,
        kArchiveAudioMaxInFlightBytes, &concurrentError, [&isCancelled]() {
          return !isCancelled.load(std::memory_order_relaxed);
        }, archive_file::ConcurrentReadMemoryPolicy::AllowSingleOversizedEntry);
    if (readOk && concurrentFiles.size() == batch.innerPaths.size()) {
      files = std::move(concurrentFiles);
      return true;
    }
    if (readOk) {
      concurrentError = "Concurrent archive audio read returned " +
                        std::to_string(concurrentFiles.size()) + " of " +
                        std::to_string(batch.innerPaths.size()) +
                        " requested files.";
    }
    if (!concurrentError.empty() &&
        !isCancelled.load(std::memory_order_relaxed)) {
      archive_file::appendDebugLogLine(
          "Falling back to serial archive audio batch read: " +
          fspath_to_utf8(batch.archivePath) + ": " + concurrentError);
    }
  }

  auto pauseCallback = [&isCancelled]() {
    return !isCancelled.load(std::memory_order_relaxed);
  };
  if (range.has_value()) {
    std::string rangeError;
    if (archive_file::readArchiveEntriesInRange(batch.archivePath,
                                                batch.innerPaths, *range, files,
                                                &rangeError, pauseCallback) &&
        files.size() == batch.innerPaths.size()) {
      return true;
    }
    files.clear();
  }
  return archive_file::readArchiveEntries(batch.archivePath, batch.innerPaths,
                                          files, errorMessage, pauseCallback);
}

bool decodedSoundIsValid(const DecodedSound &decoded) {
  return decoded.info.frames > 0 && decoded.info.channels > 0 &&
         decoded.info.samplerate > 0 &&
         static_cast<std::uint64_t>(decoded.info.frames) <=
             decoded.pcm.size() / static_cast<std::size_t>(decoded.info.channels);
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
    if (!readArchiveAudioBatch(batch, range, files, &errorMessage,
                               isCancelled)) {
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
    parallel_for_each_index(files.size(), [&](std::size_t i) {
      if (isCancelled) {
        return;
      }
      const auto &file = files[i];
      const std::filesystem::path virtualPath =
          archive_file::makeVirtualPath(batch.archivePath, file.path);
      const path_t soundPath = fspath_to_path_t(virtualPath);
      const auto idsIt = batch.wavIdsByPath.find(soundPath);
      if (idsIt == batch.wavIdsByPath.end()) {
        return;
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
        return;
      }

      std::lock_guard<std::mutex> lock(decodedSoundsMutex);
      for (const int wav : idsIt->second) {
        if (decodedSounds.emplace(wav, decoded).second) {
          ++decodedInBatch;
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

struct RenderBudget {
  const RenderOptions &options;
  std::atomic_bool &isCancelled;
  std::size_t maxFrames;
  std::size_t remainingMixedFrames;
  std::string error;
  bool reportedMixProgress = false;

  bool checkpoint() {
    if (isCancelled.load(std::memory_order_relaxed)) {
      error = "Chart audio render cancelled";
      return false;
    }
    return true;
  }

  bool admitFrames(long double frames) {
    if (!checkpoint()) return false;
    if (!std::isfinite(frames) || frames < 0 || frames > maxFrames) {
      error = "Chart audio output frame limit exceeded";
      return false;
    }
    return true;
  }
};

bool ensureMixFrames(std::vector<float> &mix, std::size_t frames,
                     RenderBudget &budget) {
  if (!budget.admitFrames(frames)) return false;
  const std::size_t samples = frames * kOutputChannels;
  if (mix.capacity() < samples) {
    mix.reserve(std::min(budget.maxFrames * kOutputChannels,
                         std::max(samples, mix.capacity() + mix.capacity() / 2)));
  }
  if (mix.size() < samples) mix.resize(samples, 0.0f);
  return true;
}

long long audioMicrosForFrames(std::size_t frames) {
  if (frames == 0) {
    return 0;
  }
  return static_cast<long long>(
      std::ceil(static_cast<long double>(frames) * 1000000.0L /
                kOutputSampleRate));
}

bool mixSoundAt(std::vector<float> &mix, const DecodedSound &sound,
                long double triggerFrame, audio::PlaybackRate playback,
                RenderBudget &budget, long long sourceStartMicros = 0,
                long long sourceDurationMicros = -1, float gain = 1.0F,
                std::optional<long double> mixStartFrame = std::nullopt,
                std::optional<long double> mixEndFrame = std::nullopt) {
  if (!budget.admitFrames(std::max(0.0L, triggerFrame))) return false;
  const auto range = audio::bmson::sourceRange(sourceStartMicros, sourceDurationMicros,
      sound.info.samplerate, static_cast<std::size_t>(sound.info.frames));
  const std::size_t sourceFrames = range.count;
  if (sourceFrames == 0) return true;
  // Gameplay first slices native PCM, then normalizes that slice to the
  // device rate, then advances its Q32 playback cursor. Combining those two
  // resamplers changes both interpolated samples and the finite sound end.
  const audio::pcm_resampling::Ratio normalization(sound.info.samplerate, kOutputSampleRate);
  const auto preparedFrames = normalization.outputFrames(sourceFrames,
      std::numeric_limits<std::uint32_t>::max());
  if (!preparedFrames) {
    budget.error = "Chart audio sound-tail frame limit exceeded";
    return false;
  }
  const auto projectedFrames = audio::frame_timing::outputFrames(*preparedFrames, playback.percent);
  // Clip only output frames. Reconstructing a source slice at a gain boundary
  // rounds away the original fractional phase and changes every later sample.
  // The same cursor origin applies when the export begins inside this voice.
  const auto start = std::max({0.0L, triggerFrame, mixStartFrame.value_or(triggerFrame)});
  const auto end = std::min(triggerFrame + projectedFrames,
                            mixEndFrame.value_or(triggerFrame + projectedFrames));
  if (end <= start) return true;
  if (!budget.admitFrames(start)) return false;
  const auto startFrame = static_cast<std::size_t>(start);
  const auto skippedFrames = static_cast<std::size_t>(start - triggerFrame);
  const auto targetFrames = static_cast<std::size_t>(end - start);
  if (targetFrames >= budget.maxFrames - startFrame) {
    budget.error = "Chart audio sound-tail frame limit exceeded";
    return false;
  }
  if (targetFrames > budget.remainingMixedFrames) {
    budget.error = "Chart audio mixing work limit exceeded";
    return false;
  }
  budget.remainingMixedFrames -= targetFrames;
  if (!ensureMixFrames(mix, startFrame + targetFrames + 1, budget)) return false;
  const auto channels = static_cast<std::size_t>(sound.info.channels);
  // Fold channel pairs only after both resampling stages, matching the live
  // mixer's channel % 2 accumulation without clipping a summed int16 frame.
  // Reuse four stereo frames of scratch regardless of source length/channels.
  for (std::size_t firstChannel = 0; firstChannel < channels; firstChannel += 2) {
    const bool hasRight = channels == 1 || firstChannel + 1 < channels;
    const auto rightChannel = channels == 1 ? 0 : firstChannel + 1;
    audio::pcm_resampling::Cursor nativeCursor(normalization, sourceFrames);
    // Lazily normalize only the samples touched by this output interval. This
    // retains bounded scratch/work and never rereads a clipped prefix, nor
    // allocates another whole PCM copy for a gain segment or composite layer.
    struct PreparedFrame {
      std::size_t index = std::numeric_limits<std::size_t>::max();
      std::array<short, 2> samples{};
    };
    std::array<PreparedFrame, 4> preparedCache;
    const auto normalized = [&](std::size_t frame) {
      auto &cached = preparedCache[frame % preparedCache.size()];
      if (cached.index != frame) {
        cached.index = frame;
        nativeCursor.seek(frame);
        const auto left = (range.start + nativeCursor.left()) * channels;
        const auto right = (range.start + nativeCursor.right()) * channels;
        cached.samples = {nativeCursor.interpolate(sound.pcm[left + firstChannel], sound.pcm[right + firstChannel]),
            hasRight ? nativeCursor.interpolate(sound.pcm[left + rightChannel], sound.pcm[right + rightChannel])
                     : short{0}};
      }
      return cached.samples;
    };
    auto position = audio::frame_timing::positionAfter(skippedFrames, playback.percent);
    const auto increment = audio::frame_timing::increment(playback.percent);
    for (std::size_t begin = 0; begin < targetFrames; begin += 4096) {
      if (!budget.checkpoint()) return false;
      const auto end = std::min(begin + 4096, targetFrames);
      if (normalization.direct() && playback.percent == 100) {
        for (auto frame = begin; frame < end; ++frame) {
          const auto source = (range.start + skippedFrames + frame) * channels;
          const auto output = (startFrame + frame) * kOutputChannels;
          mix[output] += sound.pcm[source + firstChannel] * (gain / 32768.0f);
          if (hasRight) mix[output + 1] += sound.pcm[source + rightChannel] * (gain / 32768.0f);
        }
      } else {
        for (auto frame = begin; frame < end; ++frame) {
          const auto leftFrame = static_cast<std::size_t>(position >> 32);
          const auto left = normalized(leftFrame);
          const auto right = normalized(std::min(leftFrame + 1, *preparedFrames - 1));
          const float fraction = static_cast<float>(static_cast<std::uint32_t>(position)) /
                                 static_cast<float>(audio::frame_timing::one);
          const auto output = (startFrame + frame) * kOutputChannels;
          for (int channel = 0; channel < (hasRight ? kOutputChannels : 1); ++channel) {
            const float sample = left[channel] + (right[channel] - left[channel]) * fraction;
            mix[output + channel] += sample * (gain / 32768.0f);
          }
          position += increment;
        }
      }
      if (end >= 4096 && !budget.reportedMixProgress) {
        budget.reportedMixProgress = true;
        logMessage(budget.options, "Chart audio mix started: 4096 frames");
      }
    }
  }
  return budget.checkpoint();
}

bool writeWavFile(const std::filesystem::path &path,
                  const std::vector<float> &mix, RenderBudget &budget,
                  const RenderOptions &options) {
  if (!budget.checkpoint()) return false;
  const auto stagingDirectory = path.parent_path() /
                                (".chart-audio-" + uuid::generateV4());
  std::error_code error;
  if (!std::filesystem::create_directory(stagingDirectory, error)) {
    budget.error = "Failed to create chart audio staging directory: " + error.message();
    return false;
  }
  ScopeExit cleanup([&] {
    std::error_code ignored;
    std::filesystem::remove_all(stagingDirectory, ignored);
  });
  SF_INFO outputInfo{};
  outputInfo.samplerate = kOutputSampleRate;
  outputInfo.channels = kOutputChannels;
  outputInfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

  auto file =
      asobmashow::audio::openSoundFileHandle(stagingDirectory / "output.wav",
                                            SFM_WRITE, outputInfo);
  if (file == nullptr) {
    budget.error =
        std::string("Failed to open chart audio output: ") + sf_strerror(nullptr);
    return false;
  }

  std::array<short, 4096 * kOutputChannels> pcm{};
  for (std::size_t offset = 0; offset < mix.size();) {
    if (!budget.checkpoint()) return false;
    const std::size_t samples = std::min(pcm.size(), mix.size() - offset);
    for (std::size_t index = 0; index < samples; ++index) {
      const float clamped = std::clamp(mix[offset + index], -1.0f, 1.0f);
      pcm[index] = static_cast<short>(std::lrint(clamped * 32767.0f));
    }
    const auto frames = static_cast<sf_count_t>(samples / kOutputChannels);
    if (sf_writef_short(file.get(), pcm.data(), frames) != frames) {
      budget.error = "Failed to write complete chart audio track";
      return false;
    }
    if (offset == 0) {
      logMessage(options, "Chart audio output started: " + std::to_string(frames) + " frames");
    }
    offset += samples;
  }
  if (sf_close(file.release()) != 0) {
    budget.error = "Failed to close complete chart audio track";
    return false;
  }
  if (!budget.checkpoint()) return false;
  return atomic_file::defaultOperations().replace(stagingDirectory / "output.wav",
                                                  path, budget.error);
}

std::vector<AudioEvent>
resolveAudioEvents(const bms_parser::Chart &chart,
                   const RenderOptions &options) {
  std::vector<AudioEvent> events;
  switch (options.keySoundMode) {
  case KeySoundMode::BackgroundOnly:
    events = CollectBackgroundAudioEvents(chart);
    break;
  case KeySoundMode::ChartTiming:
    events = CollectChartTimedAudioEvents(chart);
    break;
  case KeySoundMode::ReplayTiming:
    if (options.replay == nullptr) {
      return {};
    }
    events = CollectReplayTimedAudioEvents(chart, *options.replay,
                                            options.keySoundOffsetMicros);
    break;
  }
  if (options.playbackEventDeadlineMicros.has_value()) {
    std::erase_if(events, [&](const AudioEvent &event) {
      return !isScheduledBeforePlaybackEnd(
          event.timeMicros, options.playbackEventDeadlineMicros);
    });
  }
  return events;
}

long long baseTimelineEndMicros(const bms_parser::Chart &chart,
                                const RenderOptions &options) {
  if (options.playbackEventDeadlineMicros.has_value())
    return *options.playbackEventDeadlineMicros;
  if (options.keySoundMode == KeySoundMode::ReplayTiming && options.replay != nullptr)
    return chart_playback_duration::ReplayTimelineEndMicros(chart, *options.replay);
  return chart_playback_duration::ChartTimelineEndMicros(chart);
}

} // namespace

std::vector<AudioEvent> ExpandAudioSlices(const bms_parser::Chart &chart,
                                         const std::vector<AudioEvent> &events) {
  struct SliceEvent { AudioEvent event; int identity; };
  std::vector<SliceEvent> expanded;
  for (const auto &event : events) {
    const auto found = chart.WavSlices.find(event.wav);
    if (event.setVoiceGain || found == chart.WavSlices.end()) expanded.push_back({event, -1});
    else for (const auto &slice : found->second) {
      auto layer = event;
      layer.wav = slice.SourceWav;
      layer.sourceStartMicros = slice.StartMicros;
      layer.sourceDurationMicros = slice.DurationMicros;
      expanded.push_back({layer, slice.SliceId});
    }
  }
  std::stable_sort(expanded.begin(), expanded.end(), [](const auto &a, const auto &b) {
    return a.event.timeMicros < b.event.timeMicros;
  });
  std::vector<AudioEvent> result;
  std::unordered_map<int, std::size_t> previous;
  std::unordered_map<std::uint64_t, std::vector<AudioEvent>> controls;
  for (const auto &[event, identity] : expanded) {
    if (event.setVoiceGain) {
      controls[event.voiceId].push_back(event);
      continue;
    }
    if (identity >= 0) {
      if (const auto found = previous.find(identity); found != previous.end()) {
        auto &prior = result[found->second];
        const auto duration = event.timeMicros - prior.timeMicros;
        if (duration == 0) {
          // One monophonic slice at a chord boundary belongs to the last
          // trigger, just as the live mixer does. Later controls of an older
          // head must not capture the replacement voice.
          prior = event;
          continue;
        }
        prior.mixEndMicros = event.timeMicros;
      }
      previous[identity] = result.size();
    }
    result.push_back(event);
  }
  std::vector<AudioEvent> segments;
  for (auto event : result) {
    if (const auto found = controls.find(event.voiceId); found != controls.end()) {
      for (const auto &control : found->second) {
        if (control.timeMicros < event.timeMicros) continue;
        if (event.mixEndMicros && control.timeMicros >= *event.mixEndMicros) break;
        if (event.gain == control.gain) continue;
        if (control.timeMicros > event.mixStartMicros.value_or(event.timeMicros)) {
          auto prefix = event;
          prefix.mixEndMicros = control.timeMicros;
          segments.push_back(prefix);
        }
        event.mixStartMicros = control.timeMicros;
        event.gain = control.gain;
      }
    }
    segments.push_back(event);
  }
  return segments;
}

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

  std::stable_sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
    return a.timeMicros < b.timeMicros;
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

  std::stable_sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
    return a.timeMicros < b.timeMicros;
  });
  return events;
}

std::vector<AudioEvent>
CollectReplayTimedAudioEvents(const bms_parser::Chart &chart,
                              const ReplayData &replay,
                              long long keySoundOffsetMicros) {
  std::vector<AudioEvent> events = CollectBackgroundAudioEvents(chart);

  const auto definition = gameplay::buildGameplayDefinition(
      chart, replay.chartMeta.LnMode);
  const auto ruleset = gameplayRulesetFromId(
      replay.playbackRuleset.value_or(replay.provenance.ruleset).id)
                           .value_or(GameplayRuleset::Beatoraja);
  for (const auto &keysound :
       resolveReplayKeysounds(definition, replay.events, std::nullopt,
                             std::nullopt, ruleset, replay.audioUpdates)) {
    events.push_back({replayEventRawTimeMicros(keysound.songTimeMicros,
                                              keySoundOffsetMicros),
                      keysound.wav, 0, -1, keysound.voiceId,
                      keysound.setVoiceGain, keysound.gain});
  }

  std::stable_sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
    return a.timeMicros < b.timeMicros;
  });
  return events;
}

RenderResult RenderChartAudioToWav(const bms_parser::Chart &chart,
                                   const std::filesystem::path &path,
                                   const RenderOptions &options) try {
  if (!options.playback.valid() ||
      options.playback.mode != audio::PlaybackMode::PitchShift) {
    return {.success = false,
            .outputPath = path,
            .message = "Chart audio export requires a supported PitchShift "
                       "playback rate"};
  }
  if (options.keySoundMode == KeySoundMode::ReplayTiming &&
      options.replay == nullptr) {
    return {.success = false,
            .outputPath = path,
            .message = "Replay-timed chart audio requires replay data"};
  }

  std::atomic_bool localCancelled = false;
  std::atomic_bool &isCancelled =
      options.isCancelled == nullptr ? localCancelled : *options.isCancelled;
  // PCM16 stereo WAV has a 36-byte RIFF payload header and 4 bytes/frame.
  // Keep both RIFF's 32-bit chunk size and the dense mix representable.
  constexpr std::size_t maxWavFrames =
      (std::numeric_limits<std::uint32_t>::max() - 36ULL) /
      (sizeof(short) * kOutputChannels);
  const auto maxFrames = std::min({options.maxOutputFrames, maxWavFrames,
                                  std::vector<float>{}.max_size() / kOutputChannels});
  RenderBudget budget{options, isCancelled, maxFrames,
                      options.maxMixedFrames, {}};
  const auto failure = [&]() -> RenderResult {
    return {.outputPath = path, .message = budget.error};
  };
  if (!budget.checkpoint()) return failure();
  const auto frameAt = [&](long long time) -> long double {
    return audio::frame_timing::difference(
        audio::frame_timing::at(time, kOutputSampleRate, options.playback.percent),
        audio::frame_timing::at(options.timelineStartMicros, kOutputSampleRate, options.playback.percent));
  };
  const long double initialFrames = std::max(1.0L, frameAt(baseTimelineEndMicros(chart, options)));
  if (!budget.admitFrames(initialFrames)) return failure();
  const auto baseDuration = audioMicrosForFrames(static_cast<std::size_t>(initialFrames));
  std::vector<float> mix;
  if (!ensureMixFrames(mix, static_cast<std::size_t>(initialFrames), budget)) return failure();
  const auto audioEvents = ExpandAudioSlices(chart, resolveAudioEvents(chart, options));
  if (!budget.checkpoint()) return failure();
  DecodedSoundCache decodedSounds;
  preloadArchivedDecodedSounds(chart, audioEvents, decodedSounds, isCancelled,
                               options);

  const auto boundaryFrame = [&](std::optional<long long> time) -> std::optional<long double> {
    return time ? std::optional(frameAt(*time)) : std::nullopt;
  };
  for (const auto &event : audioEvents) {
    if (!budget.checkpoint()) return failure();
    DecodedSound *sound =
        loadDecodedSound(chart, event.wav, decodedSounds, isCancelled);
    if (sound == nullptr) {
      continue;
    }
    if (!mixSoundAt(mix, *sound,
                    frameAt(event.timeMicros),
                    options.playback, budget, event.sourceStartMicros,
                    event.sourceDurationMicros, event.gain,
                    boundaryFrame(event.mixStartMicros),
                    boundaryFrame(event.mixEndMicros))) return failure();
  }
  if (!budget.checkpoint()) return failure();
  if (options.clubMode) {
    long double beatCount = 0;
    for (const auto *measure : chart.Measures) {
      if (!budget.checkpoint()) return failure();
      if (measure == nullptr || !std::isfinite(measure->Scale) || measure->Scale <= 0) continue;
      const long double measureBeats =
          std::ceil(static_cast<long double>(measure->Scale) * 4);
      beatCount += measureBeats;
      // buildPlan increments an int beat number after each generated beat.
      if (measureBeats >= std::numeric_limits<int>::max() ||
          beatCount > std::vector<club_beat::Event>{}.max_size() ||
          beatCount > options.maxClubBeats) {
        budget.error = "Chart audio club beat plan limit exceeded";
        return failure();
      }
    }
    const DecodedSound kick =
        decodedClubSound(club_beat::synthesizeKick(kOutputSampleRate));
    const DecodedSound clap =
        decodedClubSound(club_beat::synthesizeClap(kOutputSampleRate));
    club_beat::PlanError planError;
    const auto clubPlan = club_beat::buildPlan(chart, &isCancelled, &planError);
    if (planError != club_beat::PlanError::None) {
      budget.error = planError == club_beat::PlanError::Cancelled
                         ? "Chart audio club beat planning cancelled"
                     : planError == club_beat::PlanError::WorkLimit
                         ? "Chart audio club beat plan limit exceeded"
                         : "Chart audio club beat timing is not representable";
      return failure();
    }
    for (const auto &event : clubPlan) {
      const auto time = frameAt(event.timeMicros);
      if (!mixSoundAt(mix, kick, time, options.playback, budget)) return failure();
      if (event.clap) {
        if (!mixSoundAt(mix, clap, time, options.playback, budget)) return failure();
      }
    }
  }
  if (options.prepMetronomePlan != nullptr &&
      options.prepMetronomePlan->enabled) {
    if (!budget.checkpoint()) return failure();
    const DecodedSound accent = decodedGeneratedPcm(
        prep_metronome_audio::makeClick(true, kOutputSampleRate,
                                        kOutputChannels),
        kOutputSampleRate, kOutputChannels);
    const DecodedSound regular = decodedGeneratedPcm(
        prep_metronome_audio::makeClick(false, kOutputSampleRate,
                                        kOutputChannels),
        kOutputSampleRate, kOutputChannels);
    for (const auto &click : options.prepMetronomePlan->clicks) {
      if (!mixSoundAt(mix, click.accent ? accent : regular,
                      frameAt(click.timeMicros),
                      options.playback, budget)) return failure();
    }
  }
  if (!budget.checkpoint()) return failure();
  const long long durationMicros =
      audioMicrosForFrames(mix.size() / kOutputChannels);
  logMessage(options, "Chart audio duration: " +
                          secondsString(durationMicros, 3) +
                          "s base=" + secondsString(baseDuration, 3) +
                          "s events=" + std::to_string(audioEvents.size()));

  if (!writeWavFile(path, mix, budget, options)) {
    return {.success = false,
            .outputPath = path,
            .message = budget.error,
            .durationMicros = durationMicros,
            .eventCount = audioEvents.size()};
  }
  return {.success = true,
          .outputPath = path,
          .message = "Audio exported",
          .durationMicros = durationMicros,
          .eventCount = audioEvents.size()};
} catch (const std::length_error &) {
  return {.outputPath = path, .message = "Chart audio container size is not representable"};
} catch (const std::bad_alloc &) {
  return {.outputPath = path, .message = "Chart audio memory resource limit exceeded"};
}

} // namespace chart_audio
