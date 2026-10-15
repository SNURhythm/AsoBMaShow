#include "ProgressiveMusicSource.h"

#include "../ArchiveFile.h"
#include "../ChartPlaybackDuration.h"
#include "../ThreadCompat.h"
#include "../Uuid.h"
#include "../path.h"
#include "../targets.h"
#include "ChartAssetExtensions.h"
#include "ChartAudioRenderer.h"
#include "ClubBeat.h"
#include "DecodedAssetCache.h"
#include "PcmResampler.h"
#include "SoundFileIO.h"
#include "VorbisDurationBound.h"
#include "decoder.h"
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#include <unistd.h>
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace music_stream {
namespace {
constexpr std::size_t kBlockFrames = 4096;
constexpr std::size_t kMaximumOpenAssets = 32;
constexpr std::size_t kMaximumEncodedAssetBytes = 256U * 1024U * 1024U;
constexpr std::uint64_t kMaximumFrames =
    (std::numeric_limits<std::uint32_t>::max() - 36ULL) / (kChannels * sizeof(short));
constexpr int kKick = -2;
constexpr int kClap = -3;

std::uint64_t framesForTime(long long micros, bool roundUp) {
  const auto time = static_cast<std::uint64_t>(std::max(0LL, micros));
  const auto seconds = time / 1000000;
  if (seconds > kMaximumFrames / kSampleRate)
    throw std::runtime_error("Music stream duration exceeds the WAV frame limit");
  const auto remainder = (time % 1000000) * kSampleRate;
  const auto frames = seconds * kSampleRate + remainder / 1000000 +
                      (roundUp && remainder % 1000000 != 0);
  if (frames > kMaximumFrames)
    throw std::runtime_error("Music stream duration exceeds the WAV frame limit");
  return frames;
}

std::array<char, 44> wavHeader(std::uint64_t frames) {
  std::array<char, 44> result{};
  const auto put = [&](std::size_t offset, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i)
      result[offset + i] = static_cast<char>((value >> (8 * i)) & 0xff);
  };
  std::copy_n("RIFF", 4, result.begin());
  put(4, 36 + static_cast<std::uint32_t>(frames * 4), 4);
  std::copy_n("WAVEfmt ", 8, result.begin() + 8);
  put(16, 16, 4);
  put(20, 1, 2);
  put(22, kChannels, 2);
  put(24, kSampleRate, 4);
  put(28, kSampleRate * 4, 4);
  put(32, 4, 2);
  put(34, 16, 2);
  std::copy_n("data", 4, result.begin() + 36);
  put(40, static_cast<std::uint32_t>(frames * 4), 4);
  return result;
}

// One decoder window per hot asset. Inactive assets retain only metadata and a
// path, so a song with thousands of keysounds does not retain thousands of FDs
// or full decoded clips. Generated Club Beat sounds are small and resident.
struct Asset {
  std::filesystem::path path;
  SF_INFO info{};
  asobmashow::audio::SoundFileHandle file;
  std::vector<short> generated;
  std::vector<short> window;
  std::vector<double> scratch;
  std::uint64_t windowStart = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t lastUse = 0;
  std::shared_ptr<audio::DecodedAssetCache> decodedAssets;
  std::atomic_bool *cancelled = nullptr;
  std::stop_token stop;
  std::atomic<std::uint64_t> *decoderOpens = nullptr;

  bool open() {
    if (file || !generated.empty() || (decodedAssets && info.frames > 0)) return true;
    SF_INFO opened{};
#if TARGET_OS_ANDROID
    if (IsAndroidTreePath(path)) {
      std::string error;
      const auto fd = OpenAndroidTreeFileDescriptor(path, error);
      if (!fd) return false;
      file.reset(sf_open_fd(*fd, SFM_READ, &opened, SF_TRUE));
      if (!file) close(*fd);
    } else
#endif
    {
      file = asobmashow::audio::openSoundFileHandle(path, SFM_READ, opened);
    }
    if (!file || opened.frames <= 0 || opened.channels <= 0 ||
        opened.channels > 64 || opened.samplerate <= 0) {
      file.reset();
      return false;
    }
    if (info.frames != 0 && (info.frames != opened.frames ||
                            info.channels != opened.channels ||
                            info.samplerate != opened.samplerate))
      throw std::runtime_error("Music source changed during playback");
    info = opened;
    if (decoderOpens) ++*decoderOpens;
    return true;
  }

  void closeDecoder() {
    file.reset();
    std::vector<short>().swap(window);
    std::vector<double>().swap(scratch);
    windowStart = std::numeric_limits<std::uint64_t>::max();
  }

  short sample(std::uint64_t frame, std::size_t channel) {
    const auto channels = static_cast<std::size_t>(info.channels);
    if (!generated.empty()) return generated[frame * channels + channel];
    if (frame < windowStart || frame - windowStart >= window.size() / channels) {
      windowStart = frame / kBlockFrames * kBlockFrames;
      const auto frames = std::min<std::uint64_t>(
          kBlockFrames, static_cast<std::uint64_t>(info.frames) - windowStart);
      if (decodedAssets) {
        SF_INFO currentInfo{};
        std::string error;
        if (!decodedAssets->ReadRange(path, windowStart, frames, window,
                                      currentInfo, *cancelled, stop, &error))
          throw std::runtime_error("Could not prepare preview keysound: " + error);
        if (currentInfo.frames != info.frames || currentInfo.channels != info.channels ||
            currentInfo.samplerate != info.samplerate || window.size() != frames * channels)
          throw std::runtime_error("Preview source changed during playback");
        return window[(frame - windowStart) * channels + channel];
      }
      window.assign(static_cast<std::size_t>(frames) * channels, 0);
      scratch.resize(window.size());
      if (!file || sf_seek(file.get(), static_cast<sf_count_t>(windowStart), SEEK_SET) < 0)
        throw std::runtime_error("Could not seek music keysound");
      const auto read = sf_readf_double(file.get(), scratch.data(), frames);
      if (read < 0 || sf_error(file.get()) != SF_ERR_NO_ERROR)
        throw std::runtime_error("Could not decode music keysound");
      // Preserve decoder.cpp's conversion, including short-read silence.
      for (std::size_t i = 0; i < static_cast<std::size_t>(read) * channels; ++i)
        window[i] = audioSampleToPcm16(scratch[i]);
    }
    return window[(frame - windowStart) * channels + channel];
  }
};

struct Event {
  std::uint64_t frame;
  int wav;
  std::size_t order;
  float gain = 1.0f;
};
struct Voice {
  std::shared_ptr<Asset> asset;
  std::uint64_t start;
  std::uint64_t end;
  std::size_t order;
  float gain;
  audio::pcm_resampling::Ratio ratio;
  audio::pcm_resampling::Cursor cursor;
  Voice(std::shared_ptr<Asset> value, const Event &event, std::uint64_t frames)
      : asset(std::move(value)), start(event.frame), end(start + frames), order(event.order),
        gain(event.gain),
        ratio(asset->info.samplerate, kSampleRate),
        cursor(ratio, static_cast<std::size_t>(asset->info.frames)) {}
};
} // namespace

struct Source::Impl {
  std::unique_ptr<bms_parser::Chart> chart;
  std::filesystem::path stagingParent;
  std::filesystem::path stagingDirectory;
  std::filesystem::path readPath;
  bool clubMode = false;
  bool preview = false;
  std::uint64_t firstFrame = 0;
  std::uint64_t previewTimelineFrames = 0;
  std::vector<Event> previewEvents;
  std::shared_ptr<audio::DecodedAssetCache> decodedAssets;
  std::vector<std::filesystem::path> previewAssetPaths;
  mutable std::mutex mutex;
  std::condition_variable changed;
  State state;
  // Only these intervals contain published PCM. The spool may be sparse after
  // a seek; its holes must never be read or published as a complete song.
  std::map<std::uint64_t, std::uint64_t> preparedRanges;
  bool endKnown = false;
  bool enabled = true;
  std::uint64_t requestedFrame = 0;
  std::optional<std::uint64_t> pendingSeek;
  std::atomic<std::uint64_t> requestRevision{0};
  std::uint64_t workRevision = 0;
  std::uint64_t activeBlockFirst = 0, activeBlockEnd = 0;
  std::atomic_bool cancelled{false};
  std::atomic<std::uint64_t> decoderOpens{0};
  std::atomic<std::uint64_t> archiveReads{0};
  std::unordered_map<int, std::shared_ptr<Asset>> assets;
  std::unordered_map<int, std::uint64_t> durationBounds;
  std::unordered_map<int, std::filesystem::path> stagedAssets;
  std::uint64_t useCounter = 0;
  std::jthread worker;

  // Caller holds mutex. A missing frame has an empty prepared range.
  std::uint64_t preparedEnd(std::uint64_t frame) const {
    auto next = preparedRanges.upper_bound(frame);
    if (next != preparedRanges.begin()) {
      const auto previous = std::prev(next);
      if (frame < previous->second) return previous->second;
    }
    return frame;
  }

  void publishRange(std::uint64_t first, std::uint64_t last) {
    auto next = preparedRanges.lower_bound(first);
    if (next != preparedRanges.begin()) {
      auto previous = std::prev(next);
      if (previous->second >= first) {
        first = previous->first;
        last = std::max(last, previous->second);
        next = preparedRanges.erase(previous);
      }
    }
    while (next != preparedRanges.end() && next->first <= last) {
      last = std::max(last, next->second);
      next = preparedRanges.erase(next);
    }
    preparedRanges.emplace(first, last);
    state.availableFrames = preparedEnd(firstFrame);
  }

  bool superseded() const {
    return requestRevision.load() != workRevision;
  }

  bool checkpoint() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return enabled || cancelled.load() || superseded(); });
    return !cancelled.load() && !superseded();
  }

  void makeRoom(const std::shared_ptr<Asset> &current) {
    if (current->file || !current->generated.empty() || !current->window.empty()) return;
    std::size_t openCount = 0;
    std::shared_ptr<Asset> oldest;
    for (const auto &[id, asset] : assets) {
      if (!asset || (!asset->file && asset->window.empty())) continue;
      ++openCount;
      if (asset != current && (!oldest || asset->lastUse < oldest->lastUse)) oldest = asset;
    }
    if (openCount >= kMaximumOpenAssets && oldest) oldest->closeDecoder();
  }

  std::optional<std::filesystem::path> stageAsset(
      int wav, std::span<const unsigned char> encoded) {
    const auto path = stagingDirectory / ("asset-" + std::to_string(wav));
    std::ofstream extracted(path, std::ios::binary);
    for (std::size_t offset = 0; offset < encoded.size();) {
      if (!checkpoint()) return {};
      const auto count = std::min<std::size_t>(65536, encoded.size() - offset);
      extracted.write(reinterpret_cast<const char *>(encoded.data() + offset), count);
      if (!extracted) throw std::runtime_error("Could not stage archived music keysound");
      offset += count;
    }
    extracted.close();
    if (!extracted) throw std::runtime_error("Could not stage archived music keysound");
    return path;
  }

  void prepareArchivedAssets(const std::vector<Event> &events, std::size_t first,
                             std::uint64_t blockEnd, std::uint64_t skipEnd) {
    // The shared preview cache owns its original virtual-path identity.
    if (decodedAssets || first >= events.size() || events[first].frame >= blockEnd) return;
    using Requests = std::map<std::filesystem::path, std::map<int, std::uint64_t>>;
    std::map<std::filesystem::path, Requests> archives;
    const std::vector<std::string_view> extensions(
        asobmshow::chart_assets::kAudioExtensions.begin(),
        asobmshow::chart_assets::kAudioExtensions.end());
    for (auto index = first; index < events.size() && events[index].frame < blockEnd; ++index) {
      if (!checkpoint()) return;
      const auto &event = events[index];
      if (assets.contains(event.wav) || stagedAssets.contains(event.wav)) continue;
      const auto skipBefore = skipEnd > event.frame ? skipEnd - event.frame : 0;
      if (const auto bound = durationBounds.find(event.wav);
          bound != durationBounds.end() && bound->second < skipBefore) continue;
      const auto sound = chart->WavTable.find(event.wav);
      if (sound == chart->WavTable.end()) continue;
      const auto path = chart->Meta.Folder / utf8_to_path_t(sound->second);
      std::filesystem::path archive, inner;
      if (!archive_file::splitVirtualPath(path, archive, inner)) continue;
      const auto resolved = archive_file::findFileWithExtensions(path, extensions);
      if (!resolved || !archive_file::splitVirtualPath(*resolved, archive, inner)) continue;
      auto &requests = archives[archive][inner];
      const auto [request, inserted] = requests.emplace(event.wav, skipBefore);
      if (!inserted) request->second = std::min(request->second, skipBefore);
    }
    for (const auto &[archive, requests] : archives) {
      if (!checkpoint()) return;
      // When accessing multiple files in one archive, use the existing archive
      // index and a batch reader. Repeated single-file reads reopen ZIP central
      // directories or replay solid decompression for each file. A single asset
      // can continue through the existing bounded reader.
      if (requests.size() < 2) continue;
      std::vector<std::filesystem::path> paths;
      for (const auto &[path, sounds] : requests) paths.push_back(path);
      std::string error;
      std::exception_ptr failure;
      ++archiveReads;
      const bool read = archive_file::readArchiveEntriesStreamingBounded(
          archive, paths, [&](archive_file::FileData &&file) {
            // Do not propagate C++ exceptions through archive SDK callbacks.
            try {
              if (!checkpoint()) return false;
              const auto found = requests.find(file.path);
              if (found == requests.end()) return true;
              std::optional<std::uint64_t> bound;
              // This batch already extracted the bytes. Probe larger keysounds
              // without another archive read or expensive Vorbis decoder setup.
              if (file.bytes.size() <= 1024U * 1024U) {
                if (const auto hint = audio::duration_bound::InspectVorbis(file.bytes))
                  bound = audio::pcm_resampling::Ratio(hint->sampleRate, kSampleRate)
                              .outputFrames(hint->frames, kMaximumFrames);
              }
              std::vector<int> needed;
              for (const auto &[wav, skipBefore] : found->second) {
                if (bound) durationBounds.emplace(wav, *bound);
                if (!bound || *bound >= skipBefore) needed.push_back(wav);
              }
              if (needed.empty()) return true;
              const auto staged = stageAsset(needed.front(), file.bytes);
              if (!staged) return false;
              for (const auto wav : needed) stagedAssets.emplace(wav, *staged);
              return true;
            } catch (...) {
              failure = std::current_exception();
              return false;
            }
          }, kMaximumEncodedAssetBytes, &error, [this] { return checkpoint(); });
      if (failure) std::rethrow_exception(failure);
      if (cancelled.load() || superseded()) return;
      if (!read) throw std::runtime_error("Could not extract music keysounds: " + error);
    }
  }

  std::shared_ptr<Asset> loadAsset(int wav, std::stop_token stop,
                                  std::uint64_t skipBefore = 0) {
    if (const auto found = assets.find(wav); found != assets.end()) return found->second;
    if (const auto found = durationBounds.find(wav);
        found != durationBounds.end() && found->second < skipBefore) return {};
    auto asset = std::make_shared<Asset>();
    asset->decoderOpens = &decoderOpens;
    if (wav == kKick || wav == kClap) {
      const auto sound = wav == kKick ? club_beat::synthesizeKick(kSampleRate)
                                     : club_beat::synthesizeClap(kSampleRate);
      asset->info.samplerate = kSampleRate;
      asset->info.channels = kChannels;
      asset->info.frames = sound.samples.size() / kChannels;
      asset->generated.reserve(sound.samples.size());
      for (float sample : sound.samples)
        asset->generated.push_back(static_cast<short>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f));
    } else {
      const auto found = chart->WavTable.find(wav);
      if (found == chart->WavTable.end()) return assets.emplace(wav, nullptr).first->second;
      const std::vector<std::string_view> extensions(
          asobmshow::chart_assets::kAudioExtensions.begin(),
          asobmshow::chart_assets::kAudioExtensions.end());
      const auto staged = stagedAssets.find(wav);
      const auto resolved = staged != stagedAssets.end()
          ? std::optional(staged->second)
          : archive_file::findFileWithExtensions(
                chart->Meta.Folder / utf8_to_path_t(found->second), extensions);
      if (!resolved) return assets.emplace(wav, nullptr).first->second;
      asset->path = *resolved;
      // A cold seek need not build Vorbis codebooks for every earlier tap.
      // Only skip sounds proven to end before both the target and an already
      // known timeline end, so conservative bounds never change exact duration.
      if (skipBefore > 0 && !durationBounds.contains(wav)) {
        auto extension = asset->path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; });
        if (extension == ".ogg") {
          std::vector<unsigned char> encoded;
          if (archive_file::isVirtualPath(asset->path)) ++archiveReads;
          if (archive_file::readFileBoundedWithCheckpoint(
                  asset->path, encoded, 64U * 1024U, nullptr, stop,
                  [this] { return checkpoint(); })) {
            if (const auto hint = audio::duration_bound::InspectVorbis(encoded)) {
              if (const auto frames = audio::pcm_resampling::Ratio(
                      hint->sampleRate, kSampleRate).outputFrames(hint->frames, kMaximumFrames)) {
                durationBounds.emplace(wav, *frames);
                if (*frames < skipBefore) return {};
              }
            }
          }
          if (!checkpoint()) return {};
        }
      }
      if (decodedAssets) {
        asset->decodedAssets = decodedAssets;
        asset->cancelled = &cancelled;
        asset->stop = stop;
        std::string error;
        audio::DecodedAssetCache::FailureKind failure;
        if (!decodedAssets->Inspect(asset->path, asset->info, cancelled, stop, &error, &failure)) {
          if (failure == audio::DecodedAssetCache::FailureKind::Operation)
            throw std::runtime_error("Could not prepare preview asset: " + error);
          if (!cancelled.load()) SDL_Log("Preview skipped invalid keysound: %s", error.c_str());
          return assets.emplace(wav, nullptr).first->second;
        }
        asset->lastUse = ++useCounter;
        assets.emplace(wav, asset);
        return asset;
      }
      std::filesystem::path archive, inner;
      if (archive_file::splitVirtualPath(asset->path, archive, inner)) {
        ++archiveReads;
        std::vector<unsigned char> encoded;
        std::string error;
        if (!archive_file::readFileBoundedWithCheckpoint(
                asset->path, encoded, kMaximumEncodedAssetBytes, &error, stop,
                [this] { return checkpoint(); })) {
          if (cancelled.load() || superseded()) return {};
          throw std::runtime_error("Could not extract music keysound: " + error);
        }
        const auto path = stageAsset(wav, encoded);
        if (!path) return {};
        asset->path = *path;
        stagedAssets.emplace(wav, *path);
      }
      makeRoom(asset);
      if (!asset->open()) {
        SDL_Log("Music stream skipped invalid keysound: %s", fspath_to_utf8(asset->path).c_str());
        return assets.emplace(wav, nullptr).first->second;
      }
    }
    asset->lastUse = ++useCounter;
    assets.emplace(wav, asset);
    return asset;
  }

  void render(std::stop_token stop) {
    if (!chart) throw std::runtime_error("Music stream chart is missing");
    std::uint64_t totalFrames = std::max<std::uint64_t>(
        std::max<std::uint64_t>(1, firstFrame), preview ? previewTimelineFrames :
        framesForTime(chart_playback_duration::ChartTimelineEndMicros(*chart), true));
    std::vector<Event> events = std::move(previewEvents);
    if (!preview) {
      for (const auto &event : chart_audio::CollectChartTimedAudioEvents(*chart)) {
        events.push_back({framesForTime(event.timeMicros, false), event.wav, events.size()});
      }
    }
    if (clubMode) {
      club_beat::PlanError error;
      const auto plan = club_beat::buildPlan(*chart, &cancelled, &error);
      if (error != club_beat::PlanError::None)
        throw std::runtime_error("Could not prepare music Club Beat plan");
      for (const auto &event : plan) {
        const auto frame = framesForTime(event.timeMicros, false);
        if (event.kick) events.push_back({frame, kKick, events.size()});
        if (event.clap) events.push_back({frame, kClap, events.size()});
      }
    }
    // Playback needs only the event snapshot and asset names. Do not retain the
    // parser's entire note/timeline graph while a stopped music source is held
    // for resume, especially when gameplay is preparing another chart.
    if (!preview) {
      auto assetsOnly = std::make_unique<bms_parser::Chart>();
      assetsOnly->Meta = std::move(chart->Meta);
      assetsOnly->WavTable = std::move(chart->WavTable);
      chart = std::move(assetsOnly);
    }
    std::stable_sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
      return a.frame < b.frame;
    });
    {
      std::unique_lock lock(mutex);
      changed.wait(lock, [&] { return enabled || cancelled.load(); });
      if (cancelled.load()) return;
    }
    std::filesystem::create_directories(stagingParent);
    stagingDirectory = stagingParent / (".music-stream-" + uuid::generateV4());
    if (!std::filesystem::create_directory(stagingDirectory))
      throw std::runtime_error("Could not create music stream staging directory");
    const auto partialPath = stagingDirectory / "audio.wav";
    std::ofstream output(partialPath, std::ios::binary);
    const auto header = wavHeader(0);
    output.write(header.data(), header.size());
    output.flush();
    if (!output) throw std::runtime_error("Could not open music stream output");
    {
      std::lock_guard lock(mutex);
      state.durationFrames = totalFrames;
      readPath = partialPath;
    }
    std::vector<std::unique_ptr<Voice>> voices;
    std::array<float, kBlockFrames * kChannels> mix{};
    std::array<char, kBlockFrames * kChannels * sizeof(short)> pcm{};
    std::size_t eventIndex = 0;
    std::uint64_t position = firstFrame;
    const auto discardObsoleteBlock = [&] {
      if (!superseded()) return false;
      voices.clear();
      eventIndex = 0;
      position = std::numeric_limits<std::uint64_t>::max();
      return true;
    };
    if (!events.empty()) totalFrames = std::max(totalFrames, events.back().frame + 1);
    for (;;) {
      std::uint64_t blockEnd;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
          if (cancelled.load() || (endKnown && state.availableFrames >= totalFrames))
            return true;
          if (!enabled) return false;
          const auto next = preparedEnd(requestedFrame);
          // A selected chart prepares gameplay audio even if the preview device
          // stops consuming. Music-only streams retain their bounded read-ahead.
          return (!endKnown || next < totalFrames) &&
                 (preview || next < requestedFrame + kReadAheadFrames);
        });
        if (cancelled.load()) return;
        if (endKnown && state.availableFrames >= totalFrames) break;
        workRevision = requestRevision.load();
        const auto next = preparedEnd(requestedFrame);
        if (position != next) {
          position = next;
          voices.clear();
          eventIndex = 0;
        }
        blockEnd = std::min<std::uint64_t>(position + kBlockFrames, kMaximumFrames);
        // Stop at the next prepared interval, avoiding writes over PCM that a
        // reader may currently be consuming on another thread.
        const auto following = preparedRanges.upper_bound(position);
        if (following != preparedRanges.end()) blockEnd = std::min(blockEnd, following->first);
        activeBlockFirst = position;
        activeBlockEnd = blockEnd;
      }
      prepareArchivedAssets(events, eventIndex, blockEnd, std::min(position, totalFrames));
      if (cancelled.load()) return;
      if (discardObsoleteBlock()) continue;
      while (eventIndex < events.size() && events[eventIndex].frame < blockEnd) {
        if (!checkpoint()) break;
        const auto event = events[eventIndex++];
        const auto skipEnd = std::min(position, totalFrames);
        auto asset = loadAsset(event.wav, stop,
            skipEnd > event.frame ? skipEnd - event.frame : 0);
        if (!asset) continue;
        const auto targetFrames = audio::pcm_resampling::Ratio(
            asset->info.samplerate, kSampleRate).outputFrames(asset->info.frames, kMaximumFrames);
        if (!targetFrames || *targetFrames >= kMaximumFrames - event.frame)
          throw std::runtime_error("Music stream sound tail exceeds WAV frame limit");
        totalFrames = std::max(totalFrames, event.frame + *targetFrames + 1);
        // Prior events may still be sounding at the selected highlight. Seek
        // their exact resampler phase; do not render the skipped chart prefix.
        if (event.frame + *targetFrames <= position) continue;
        auto voice = std::make_unique<Voice>(std::move(asset), event, *targetFrames);
        voice->cursor.seek(position > event.frame ? position - event.frame : 0);
        voices.push_back(std::move(voice));
      }
      if (cancelled.load()) return;
      if (discardObsoleteBlock()) continue;
      if (position >= totalFrames && eventIndex == events.size()) {
        std::lock_guard lock(mutex);
        endKnown = true;
        state.durationFrames = totalFrames;
        changed.notify_all();
        continue;
      }
      if (blockEnd <= position) throw std::runtime_error("Music stream exceeded WAV frame limit");
      // Match the full renderer's accumulation order (all chart voices, then
      // Club Beat), independently of which block first activates each voice.
      std::sort(voices.begin(), voices.end(), [](const auto &a, const auto &b) {
        return a->order < b->order;
      });
      const auto end = std::min(blockEnd, totalFrames);
      std::fill(mix.begin(), mix.end(), 0.0f);
      for (auto &voice : voices) {
        if (!checkpoint()) break;
        auto &asset = *voice->asset;
        makeRoom(voice->asset);
        if (!asset.open()) throw std::runtime_error("Could not reopen music keysound");
        asset.lastUse = ++useCounter;
        const auto from = std::max(position, voice->start);
        const auto to = std::min(end, voice->end);
        const std::size_t rightChannel = asset.info.channels == 1 ? 0 : 1;
        for (auto frame = from; frame < to; ++frame) {
          const auto leftFrame = voice->cursor.left();
          short left, right;
          if (voice->ratio.direct()) {
            left = asset.sample(leftFrame, 0);
            right = rightChannel == 0 ? left : asset.sample(leftFrame, rightChannel);
          } else {
            const auto rightFrame = voice->cursor.right();
            // Read the left pair before advancing the decoder window at a boundary.
            const auto left0 = asset.sample(leftFrame, 0);
            const auto right0 = rightChannel == 0 ? left0 : asset.sample(leftFrame, rightChannel);
            const auto left1 = asset.sample(rightFrame, 0);
            const auto right1 = rightChannel == 0 ? left1 : asset.sample(rightFrame, rightChannel);
            left = voice->cursor.interpolate(left0, left1);
            right = voice->cursor.interpolate(right0, right1);
          }
          const auto index = static_cast<std::size_t>(frame - position) * kChannels;
          mix[index] += left * (1.0f / 32768.0f) * voice->gain;
          mix[index + 1] += right * (1.0f / 32768.0f) * voice->gain;
          voice->cursor.advance();
        }
      }
      if (cancelled.load()) return;
      if (discardObsoleteBlock()) continue;
      std::erase_if(voices, [&](const auto &voice) { return voice->end <= end; });
      const auto samples = static_cast<std::size_t>(end - position) * kChannels;
      for (std::size_t i = 0; i < samples; ++i) {
        const auto sample = static_cast<std::uint16_t>(static_cast<short>(
            std::lrint(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f)));
        pcm[i * 2] = static_cast<char>(sample & 0xff);
        pcm[i * 2 + 1] = static_cast<char>(sample >> 8);
      }
      output.seekp(static_cast<std::streamoff>(44 + (position - firstFrame) * 4));
      output.write(pcm.data(), samples * sizeof(short));
      output.flush();
      if (!output) throw std::runtime_error("Could not write music stream audio");
      {
        std::lock_guard lock(mutex);
        publishRange(position, end);
        state.durationFrames = totalFrames;
        endKnown = endKnown || eventIndex == events.size();
      }
      position = end;
      changed.notify_all();
    }
    const auto finalHeader = wavHeader(totalFrames - firstFrame);
    output.seekp(0);
    output.write(finalHeader.data(), finalHeader.size());
    output.close();
    if (!output) throw std::runtime_error("Could not finalize music stream");
    {
      std::lock_guard lock(mutex);
      if (cancelled.load()) return;
      state.complete = true;
      state.durationFrames = totalFrames;
    }
    changed.notify_all();
    assets.clear();
    chart.reset();
  }

  void run(std::stop_token stop) noexcept {
    try {
      render(stop);
      // Playback can finish independently of gameplay preloading. In particular,
      // EOF disables rendering, but only cancellation should stop cache warming.
      if (preview && decodedAssets) {
        for (const auto &path : previewAssetPaths) {
          if (cancelled.load() || stop.stop_requested()) return;
          std::string error;
          audio::DecodedAssetCache::FailureKind failure;
          if (!decodedAssets->Preload(path, cancelled, stop, &error, &failure)) {
            if (failure == audio::DecodedAssetCache::FailureKind::Cancelled) return;
            SDL_Log("Preview could not preload keysound: %s", error.c_str());
            // Optional warming must not turn already prepared playback into an
            // error. Stop on operational failures, skip missing/invalid assets.
            if (failure == audio::DecodedAssetCache::FailureKind::Operation) return;
          }
        }
      }
    }
    catch (const std::exception &error) {
      std::lock_guard lock(mutex);
      if (!cancelled.load()) state.error = error.what();
    } catch (...) {
      std::lock_guard lock(mutex);
      if (!cancelled.load()) state.error = "Music stream preparation failed";
    }
    changed.notify_all();
  }
};

Source::Source(std::unique_ptr<Impl> value) : impl(std::move(value)) {}
Source::~Source() {
  Cancel();
  if (impl->worker.joinable()) impl->worker.join();
  // Close extracted-asset decoders before removing their files (required on Windows).
  impl->assets.clear();
  std::error_code ignored;
  if (!impl->stagingDirectory.empty()) std::filesystem::remove_all(impl->stagingDirectory, ignored);
}
std::shared_ptr<Source> Source::Create(std::unique_ptr<bms_parser::Chart> chart,
                                     std::filesystem::path stagingParent, bool clubMode) {
  auto impl = std::make_unique<Impl>();
  impl->chart = std::move(chart);
  impl->stagingParent = std::move(stagingParent);
  impl->clubMode = clubMode;
  auto source = std::shared_ptr<Source>(new Source(std::move(impl)));
  source->impl->worker = std::jthread([state = source->impl.get()](std::stop_token stop) {
    state->run(stop);
  });
  return source;
}
std::shared_ptr<Source> Source::CreatePreview(
    const bms_parser::Chart &chart, std::filesystem::path stagingParent,
    std::shared_ptr<audio::DecodedAssetCache> decodedAssets, std::uint64_t firstFrame,
    PreviewGains gains) {
  auto impl = std::make_unique<Impl>();
  // Timeline objects stay owned by the menu for the eventual gameplay handoff.
  impl->chart = std::make_unique<bms_parser::Chart>();
  impl->chart->Meta = chart.Meta;
  impl->chart->WavTable = chart.WavTable;
  impl->preview = true;
  impl->firstFrame = std::min(firstFrame, kMaximumFrames);
  impl->state.firstFrame = impl->firstFrame;
  impl->state.availableFrames = impl->firstFrame;
  impl->requestedFrame = impl->firstFrame;
  impl->previewTimelineFrames = framesForTime(
      chart_playback_duration::ChartTimelineEndMicros(chart), true);
  const auto background = chart_audio::CollectBackgroundAudioEvents(chart);
  auto bgm = background.begin();
  for (const auto &event : chart_audio::CollectChartTimedAudioEvents(chart)) {
    const bool isBackground = bgm != background.end() &&
        bgm->timeMicros == event.timeMicros && bgm->wav == event.wav;
    if (isBackground) ++bgm;
    impl->previewEvents.push_back({framesForTime(event.timeMicros, false),
                                  event.wav, impl->previewEvents.size(),
                                  isBackground ? gains.bgm : gains.keysound});
  }
  impl->stagingParent = std::move(stagingParent);
  impl->decodedAssets = std::move(decodedAssets);
  if (impl->decodedAssets) {
    for (const auto &[wav, name] : chart.ReferencedWavTable)
      impl->previewAssetPaths.push_back(chart.Meta.Folder / utf8_to_path_t(name));
  }
  auto source = std::shared_ptr<Source>(new Source(std::move(impl)));
  source->impl->worker = std::jthread([state = source->impl.get()](std::stop_token stop) {
    state->run(stop);
  });
  return source;
}
ReadResult Source::ReadFrames(std::uint64_t firstFrame, std::span<short> interleaved,
                              std::chrono::milliseconds wait) {
  if (interleaved.size() < kChannels || interleaved.size() % kChannels != 0)
    return {.status = ReadStatus::Error, .error = "Music read requires complete stereo frames"};
  std::unique_lock lock(impl->mutex);
  if (firstFrame < impl->firstFrame)
    return {.status = ReadStatus::Error, .error = "Requested audio precedes preview range"};
  firstFrame = std::min(firstFrame, kMaximumFrames);
  if (impl->state.cancelled) return {.status = ReadStatus::Cancelled};
  if (!impl->state.error.empty()) return {.status = ReadStatus::Error, .error = impl->state.error};
  // A feeder may enter its old read just after the transport has sought. Do
  // not let that stale read replace the target before the new epoch reads it.
  if (impl->pendingSeek && firstFrame != *impl->pendingSeek)
    return {.status = ReadStatus::Pending};
  impl->pendingSeek.reset();
  if (firstFrame != impl->requestedFrame && impl->preparedEnd(firstFrame) == firstFrame &&
      (firstFrame < impl->activeBlockFirst || firstFrame >= impl->activeBlockEnd))
    ++impl->requestRevision;
  impl->requestedFrame = firstFrame;
  const auto revision = impl->requestRevision.load();
  impl->changed.notify_all();
  const auto atEnd = [&] {
    // A sought suffix can end while earlier ranges remain missing. When all
    // ranges exist, wait for spool finalization before reporting EOF.
    return impl->state.complete || (impl->endKnown &&
        firstFrame >= impl->state.durationFrames &&
        impl->state.availableFrames < impl->state.durationFrames);
  };
  impl->changed.wait_for(lock, std::max(wait, std::chrono::milliseconds::zero()), [&] {
    return impl->state.cancelled || !impl->state.error.empty() ||
           impl->requestRevision.load() != revision ||
           atEnd() || impl->preparedEnd(firstFrame) > firstFrame;
  });
  if (impl->state.cancelled) return {.status = ReadStatus::Cancelled};
  if (!impl->state.error.empty()) return {.status = ReadStatus::Error, .error = impl->state.error};
  if (impl->requestRevision.load() != revision) return {.status = ReadStatus::Pending};
  const auto availableEnd = impl->preparedEnd(firstFrame);
  if (firstFrame >= availableEnd)
    return {.status = atEnd() ? ReadStatus::End : ReadStatus::Pending};
  const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>(
      interleaved.size() / kChannels, availableEnd - firstFrame));
  const auto path = impl->readPath;
  lock.unlock();
  std::ifstream input(path, std::ios::binary);
  input.seekg(static_cast<std::streamoff>(44 + (firstFrame - impl->firstFrame) * 4));
  // Native targets are little-endian, but keep the spool's PCM portable.
  std::array<unsigned char, kBlockFrames * kChannels * sizeof(short)> bytes{};
  std::size_t offset = 0;
  while (offset < frames * kChannels) {
    const auto count = std::min(kBlockFrames * kChannels, frames * kChannels - offset);
    input.read(reinterpret_cast<char *>(bytes.data()), count * sizeof(short));
    if (!input) return {.status = ReadStatus::Error, .error = "Could not read prepared music audio"};
    for (std::size_t i = 0; i < count; ++i)
      interleaved[offset + i] = static_cast<short>(
          static_cast<std::uint16_t>(bytes[i * 2]) |
          static_cast<std::uint16_t>(bytes[i * 2 + 1]) << 8);
    offset += count;
  }
  if (impl->cancelled.load()) return {.status = ReadStatus::Cancelled};
  return {.status = ReadStatus::Data, .frames = frames};
}
State Source::GetState() const {
  std::lock_guard lock(impl->mutex);
  return impl->state;
}
Statistics Source::GetStatistics() const {
  return {.decoderOpens = impl->decoderOpens.load(),
          .archiveReads = impl->archiveReads.load()};
}
void Source::RequestSeek(std::uint64_t firstFrame) {
  {
    std::lock_guard lock(impl->mutex);
    impl->requestedFrame = std::clamp(firstFrame, impl->firstFrame, kMaximumFrames);
    impl->pendingSeek = impl->requestedFrame;
    ++impl->requestRevision;
  }
  impl->changed.notify_all();
}
void Source::SetRenderingEnabled(bool enabled) {
  { std::lock_guard lock(impl->mutex); impl->enabled = enabled; }
  impl->changed.notify_all();
}
void Source::Cancel() {
  {
    std::lock_guard lock(impl->mutex);
    impl->cancelled.store(true);
    impl->state.cancelled = true;
  }
  impl->worker.request_stop();
  impl->changed.notify_all();
}

} // namespace music_stream
