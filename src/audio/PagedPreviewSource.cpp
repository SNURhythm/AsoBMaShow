#include "PagedPreviewSource.h"

#include "../ArchiveFile.h"
#include "../ChartPlaybackDuration.h"
#include "../path.h"
#include "../RAII.h"
#include "../targets.h"
#include "../utils/ParallelWorkBatch.h"
#include "AudioDurationBound.h"
#include "AudioMix.h"
#include "ArchiveAssetBatchLoader.h"
#include "ChartAssetExtensions.h"
#include "ChartAudioRenderer.h"
#include "DecodedAssetCache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#include <unistd.h>
#endif

namespace audio {
namespace {
// All ratios here use positive rates <= 1,000,000. Splitting the quotient
// keeps the fractional multiplication bounded on platforms without int128.
std::uint64_t scaleFramesBounded(std::uint64_t frames, std::uint64_t numerator,
                                 std::uint64_t denominator, bool roundUp = false) noexcept {
  const auto whole = frames / denominator;
  const auto fractional = (frames % denominator) * numerator;
  const auto tail = fractional / denominator + (roundUp && fractional % denominator != 0);
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (whole > (maximum - tail) / numerator) return maximum;
  return whole * numerator + tail;
}
std::uint64_t scaleFrames(std::uint64_t frames, std::uint64_t numerator,
                          std::uint64_t denominator, bool roundUp = false) {
  const auto result = scaleFramesBounded(frames, numerator, denominator, roundUp);
  if (result == std::numeric_limits<std::uint64_t>::max())
    throw std::runtime_error("Preview frame position is not representable.");
  return result;
}
std::uint64_t nearestFrames(std::uint64_t frames, std::uint64_t numerator,
                             std::uint64_t denominator) noexcept {
  const auto floor = scaleFramesBounded(frames, numerator, denominator);
  const auto remainder = ((frames % denominator) * numerator) % denominator;
  return floor == std::numeric_limits<std::uint64_t>::max() ? floor :
      floor + (remainder >= (denominator + 1) / 2);
}
std::uint64_t framesForMicros(long long micros, int rate, bool roundUp) {
  return scaleFrames(static_cast<std::uint64_t>(std::max(0LL, micros)),
                     rate, 1000000, roundUp);
}
std::optional<duration_bound::Duration> probeFileDuration(
    const std::filesystem::path &path, const std::atomic_bool &cancelled) {
  std::FILE *opened = nullptr;
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(path)) {
    std::string error;
    const auto descriptor = OpenAndroidTreeFileDescriptor(path, error);
    if (!descriptor) return {};
    opened = fdopen(*descriptor, "rb");
    if (!opened) close(*descriptor);
  } else
#endif
  {
#ifdef _WIN32
    opened = _wfopen(path.c_str(), L"rb");
#else
    opened = std::fopen(path.c_str(), "rb");
#endif
  }
  UniqueResource<std::FILE, std::fclose> file(opened);
  if (!file) return {};
  std::setvbuf(file.get(), nullptr, _IONBF, 0);
  if (std::fseek(file.get(), 0, SEEK_END) != 0) return {};
  const auto size = std::ftell(file.get());
  if (size < 0 || static_cast<std::uint64_t>(size) > DecodedAssetCache::kMaximumEncodedAssetBytes)
    return {};
  return duration_bound::InspectAudio(static_cast<std::uint64_t>(size),
      [&](std::uint64_t offset, std::span<unsigned char> bytes) {
        return !cancelled.load(std::memory_order_relaxed) &&
            std::fseek(file.get(), static_cast<long>(offset), SEEK_SET) == 0 &&
            std::fread(bytes.data(), 1, bytes.size(), file.get()) == bytes.size();
      });
}
} // namespace

struct PagedPreviewSource::Impl {
  struct Asset {
    std::filesystem::path path;
    std::unique_ptr<SoundData> sound;
    std::optional<std::uint64_t> latestPastStart, firstFutureStart;
    std::optional<DecodedAssetCache::SourceSnapshot> probedSource;
  };
  struct Event {
    long long timeMicros;
    std::size_t asset;
    Bus bus;
    std::uint64_t outputStart = 0;
  };
  struct DeferredAsset {
    std::size_t index;
    std::shared_ptr<const DecodedAssetCache::DecodedSound> native;
  };
  std::vector<Asset> assets;
  std::unique_ptr<std::atomic<std::uint64_t>[]> pastDurationFrames;
  std::vector<Event> events;
  std::shared_ptr<DecodedAssetCache> cache;
  PagedPreviewGains gains;
  std::unique_ptr<AudioCallbackState> callback;
  std::jthread worker;
  mutable std::mutex mutex;
  std::condition_variable changed;
  PagedPreviewState state;
  std::atomic_bool cancelled{false};
  std::atomic_bool failed{false};
  std::atomic_bool startupReady{false};
  std::atomic_bool durationKnown{false};
  std::atomic<std::uint64_t> durationOutput{0};
  std::atomic<std::uint64_t> position44100{0};
  int outputRate = 0;
  std::uint64_t originOutput = 0;
  std::uint64_t requestedOriginOutput = 0; // Immutable while loader and startup workers run.
  std::uint64_t renderedOutput = 0; // Callback-owned.
  std::uint64_t timelineFrames44100 = 0;
  std::uint64_t finalOutput = 0; // Worker-owned until durationKnown publication.
  bool begun = false;
  bool skipLeadingSilence = false;
  std::atomic_bool loadingDone{false};

  struct RenderPosition { long long micros; std::uint32_t offset; };
  RenderPosition renderPosition(std::uint64_t frame) const noexcept {
    // Integer-second epochs preserve scheduler rounding across scratch chunks.
    return {static_cast<long long>((frame / outputRate) * 1000000),
            static_cast<std::uint32_t>(frame % outputRate)};
  }
  PagedPreviewRenderResult checkRender(std::uint32_t frameCount,
                                       bool includeAllDueSounds) const noexcept {
    if (cancelled.load(std::memory_order_relaxed) || failed.load(std::memory_order_acquire) ||
        !startupReady.load(std::memory_order_acquire)) return {};
    if (durationKnown.load(std::memory_order_acquire)) {
      const auto end = durationOutput.load(std::memory_order_relaxed);
      if (renderedOutput >= end) return {PagedPreviewRenderStatus::End, 0};
      frameCount = static_cast<std::uint32_t>(std::min<std::uint64_t>(frameCount,
                                                                  end - renderedOutput));
    }
    if (!frameCount) return {};
    const auto position = renderPosition(renderedOutput);
    if (!playback::ScheduledMixReady(*callback, position.micros, outputRate,
                                     frameCount, 100, position.offset,
                                     includeAllDueSounds)) return {};
    return {PagedPreviewRenderStatus::Data, frameCount};
  }

  void checkpoint() const {
    if (cancelled.load(std::memory_order_relaxed))
      throw std::runtime_error("Preview preparation cancelled.");
  }
  void publishState() {
    { std::lock_guard lock(mutex); ++state.revision; }
    changed.notify_all();
  }
  bool excluded(std::size_t index) const {
    return assets[index].latestPastStart &&
        pastDurationFrames[index].load(std::memory_order_acquire) <=
            requestedOriginOutput - *assets[index].latestPastStart;
  }
  bool excluded(const Event &event) const {
    return event.outputStart < requestedOriginOutput &&
        pastDurationFrames[event.asset].load(std::memory_order_acquire) <=
            requestedOriginOutput - event.outputStart;
  }
  bool openingRequired(std::size_t index) const {
    return assets[index].firstFutureStart &&
        *assets[index].firstFutureStart < requestedOriginOutput + static_cast<std::uint64_t>(outputRate) * 2;
  }
  bool canDefer(std::size_t index) const {
    const auto &asset = assets[index];
    return requestedOriginOutput && (!asset.latestPastStart || excluded(index)) && !openingRequired(index);
  }
  std::uint64_t futurePriority(std::size_t index) const {
    return assets[index].firstFutureStart.value_or(std::numeric_limits<std::uint64_t>::max());
  }
  void publishPastDuration(std::size_t index, std::optional<duration_bound::Duration> duration) {
    if (!assets[index].latestPastStart) return;
    // Unknown formats have the user-requested 15-second overlap lookback.
    // Known long audio is never truncated to that fallback.
    const auto frames = duration
        ? scaleFramesBounded(duration->frames, outputRate, duration->sampleRate, true)
        : static_cast<std::uint64_t>(outputRate) * 15;
    // Retain the probe decision even when a recent instance requires decoding.
    // Each old onset must independently obey the unknown-duration lookback.
    pastDurationFrames[index].store(frames, std::memory_order_release);
    publishState();
  }
  void publishCachedDuration(std::size_t index,
      const std::shared_ptr<const DecodedAssetCache::DecodedSound> &native) {
    if (native->info.channels > 0 && native->info.samplerate > 0)
      publishPastDuration(index, duration_bound::Duration{
          native->pcm.size() / native->info.channels, native->info.samplerate});
  }
  void fail(std::string error) {
    { std::lock_guard lock(mutex);
      state.error = std::move(error);
      failed.store(true, std::memory_order_release);
      ++state.revision; }
    changed.notify_all();
  }
  void publishDecoded(std::size_t index,
      const std::shared_ptr<const DecodedAssetCache::DecodedSound> &native) {
    checkpoint();
    auto &outputSound = *assets[index].sound->pagedOutput;
    if (!native || (excluded(index) && !assets[index].firstFutureStart)) {
      // Native PCM remains warm for gameplay; expired preview voices need no
      // output-rate conversion, even when the cache already contains them.
      outputSound.publishMetadata(0, 0);
    } else {
      // Resampling can expand a small native asset into gigabytes. Bound the
      // actual output samples (including channels and rounded-up frames) before
      // allocating, independently of the cache's unlimited retention setting.
      const auto outputSamples = ProjectedResampledPcmSampleCount(
          native->pcm.size(), native->info.channels, native->info.samplerate, outputRate);
      if (!outputSamples ||
          *outputSamples > DecodedAssetCache::kMaximumPcmAssetBytes / sizeof(short))
        throw std::runtime_error("Preview resampled audio exceeds the per-asset byte limit.");
      auto output = native->FindPreparedOutput(outputRate);
      if (output.empty() && !native->pcm.empty()) {
        output = ResamplePcm(native->pcm, native->info.channels,
                             native->info.samplerate, outputRate, &cancelled);
      }
      checkpoint();
      native->RememberPreparedOutput(outputRate, output);
      if ((!native->pcm.empty() && output.empty()) ||
          !outputSound.publishFull(native->info.channels, std::move(output)))
        throw std::runtime_error("Could not publish preview audio.");
    }
    publishState();
  }
  void publishAsset(std::size_t index, const std::vector<unsigned char> *bytes = nullptr,
                     const DecodedAssetCache::ArchiveBatch *batch = nullptr) {
    checkpoint();
    if (const auto &source = assets[index].probedSource; source && !cache->ArchiveBatchUnchanged(*source))
      throw std::runtime_error("Audio source changed after duration probing.");
    DecodedAssetCache::FailureKind failure{};
    std::string error;
    const auto native = bytes
        ? cache->LoadArchiveBytes(*batch, assets[index].path, *bytes, cancelled, {}, &error, &failure)
        : cache->Load(assets[index].path, cancelled, {}, &error, &failure);
    if (const auto &source = assets[index].probedSource; source && !cache->ArchiveBatchUnchanged(*source))
      throw std::runtime_error("Audio source changed after duration probing.");
    if (!native && failure != DecodedAssetCache::FailureKind::InvalidAsset) {
      checkpoint();
      throw std::runtime_error(error.empty() ? "Could not load preview audio." : error);
    }
    publishDecoded(index, native);
  }

  void loadAll() {
    // Opening audio first, then past overlap checks, then future/referenced PCM.
    // Keep the original order for a preview starting at the beginning.
    std::vector<std::size_t> ordered;
    std::vector<bool> seen(assets.size());
    if (requestedOriginOutput) {
      const auto openingEnd = requestedOriginOutput + static_cast<std::uint64_t>(outputRate) * 2;
      for (const auto &event : events)
        if (event.outputStart >= requestedOriginOutput && event.outputStart < openingEnd && !seen[event.asset]) {
          seen[event.asset] = true;
          ordered.push_back(event.asset);
        }
      for (const auto &event : events)
        if (event.outputStart < requestedOriginOutput && !seen[event.asset]) {
          seen[event.asset] = true;
          ordered.push_back(event.asset);
        }
    }
    for (const auto &event : events)
      if (!seen[event.asset]) { seen[event.asset] = true; ordered.push_back(event.asset); }
    for (std::size_t index = 0; index < assets.size(); ++index)
      if (!seen[index]) ordered.push_back(index);
    static const std::vector<std::string_view> extensions(
        asobmshow::chart_assets::kAudioExtensions.begin(),
        asobmshow::chart_assets::kAudioExtensions.end());
    // A chart normally has one source. Contiguous source groups also support
    // mixed loose/archive assets without creating nested worker pools.
    for (std::size_t first = 0; first < ordered.size();) {
      checkpoint();
      std::filesystem::path archive, inner;
      const bool archived = archive_file::splitVirtualPath(assets[ordered[first]].path, archive, inner);
      auto end = first + 1;
      while (end < ordered.size()) {
        // The first path has established this archive. Containment is purely
        // lexical; splitVirtualPath would stat that same file for every sound.
        if (archived) {
          const auto relative = assets[ordered[end]].path.lexically_relative(archive);
          if (relative.empty() || relative == "." || relative.is_absolute() ||
              *relative.begin() == "..") break;
          ++end;
          continue;
        }
        std::filesystem::path nextArchive, nextInner;
        const bool nextArchived = archive_file::splitVirtualPath(assets[ordered[end]].path,
                                                                nextArchive, nextInner);
        if (nextArchived) break;
        ++end;
      }
      if (!archived) {
        std::vector<DeferredAsset> deferred;
        std::mutex deferredMutex;
        ParallelWorkBatch workers(end - first);
        workers.run(end - first, [&](std::size_t item) {
          const auto index = ordered[first + item];
          std::shared_ptr<const DecodedAssetCache::DecodedSound> ready;
          if (assets[index].latestPastStart) {
            ready = cache->Find(assets[index].path);
            if (ready) {
              publishCachedDuration(index, ready);
            } else {
              auto &asset = assets[index];
              asset.path = archive_file::findFileWithExtensions(asset.path, extensions).value_or(asset.path);
              asset.probedSource = cache->CaptureSource(asset.path);
              const auto duration = probeFileDuration(asset.path, cancelled);
              checkpoint();
              if (asset.probedSource && !cache->ArchiveBatchUnchanged(*asset.probedSource))
                throw std::runtime_error("Audio source changed during duration probing.");
              publishPastDuration(index, duration);
            }
          }
          if (canDefer(index)) {
            std::lock_guard lock(deferredMutex);
            deferred.push_back({index, std::move(ready)});
            return;
          }
          if (ready) publishDecoded(index, ready);
          else publishAsset(index);
        });
        std::sort(deferred.begin(), deferred.end(), [&](const auto &a, const auto &b) {
          return futurePriority(a.index) < futurePriority(b.index);
        });
        workers.run(deferred.size(), [&](std::size_t item) {
          const auto &asset = deferred[item];
          if (asset.native) publishDecoded(asset.index, asset.native);
          else publishAsset(asset.index);
        });
      } else {
        const auto batch = cache->BeginArchiveBatch(archive);
        if (!batch) throw std::runtime_error("Preview audio archive is unavailable.");
        std::vector<std::filesystem::path> requested;
        for (auto item = first; item < end; ++item) {
          requested.push_back(assets[ordered[item]].path.lexically_relative(archive));
        }
        std::vector<std::optional<std::filesystem::path>> resolved;
        std::string error;
        if (!archive_file::resolveArchiveEntriesWithExtensions(archive, requested, extensions,
                resolved, &error, [&] { return !cancelled.load(std::memory_order_relaxed); })) {
          checkpoint();
          throw std::runtime_error(error.empty() ? "Could not resolve preview archive audio." : error);
        }
        std::vector<std::filesystem::path> paths;
        std::map<std::filesystem::path, std::vector<std::size_t>> indices;
        std::map<std::filesystem::path, std::size_t> priorities;
        std::vector<DeferredAsset> cachedDeferred;
        for (auto item = first; item < end; ++item) {
          checkpoint();
          const auto index = ordered[item];
          if (!resolved[item - first]) { publishDecoded(index, {}); continue; }
          inner = *resolved[item - first];
          assets[index].path = archive_file::makeVirtualPath(archive, inner);
          if (const auto ready = cache->FindArchiveAsset(*batch, assets[index].path)) {
            publishCachedDuration(index, ready);
            if (canDefer(index)) cachedDeferred.push_back({index, ready});
            else publishDecoded(index, ready);
            continue;
          }
          if (!indices.contains(inner)) { paths.push_back(inner); priorities.emplace(inner, item); }
          indices[inner].push_back(index);
        }
        // ZIP can inspect duration headers without inflating expired payloads.
        // Reuse its cached directory once for the whole chart, then extract
        // opening/overlapping voices before the remaining gameplay warm-up.
        std::map<std::filesystem::path, std::optional<duration_bound::Duration>> probed;
        bool zipProbed = false;
        if (requestedOriginOutput) {
          std::vector<std::filesystem::path> pastPaths;
          for (const auto &path : paths)
            if (std::any_of(indices.at(path).begin(), indices.at(path).end(),
                    [&](auto index) { return assets[index].latestPastStart.has_value(); }))
              pastPaths.push_back(path);
          zipProbed = archive_file::probeZipEntries(archive, pastPaths,
                  [&](const auto &path, auto size, const auto &readAt) {
                    probed.emplace(path, duration_bound::InspectAudio(size, readAt));
                  }, &error, [&] { return !cancelled.load(std::memory_order_relaxed); });
          checkpoint();
          if (!zipProbed) probed.clear();
          if (!cache->ArchiveBatchUnchanged(*batch))
            throw std::runtime_error("Preview audio archive changed during duration probing.");
          for (const auto &[path, duration] : probed)
            for (auto index : indices.at(path)) publishPastDuration(index, duration);
        }
        std::vector<archive_file::FileData> deferred;
        std::mutex deferredMutex;
        std::size_t deferredBytes = 0;
        constexpr std::size_t maximumDeferredBytes = 8U * 1024U * 1024U;
        const auto readBatch = [&](const auto &batchPaths) {
          return ConsumeArchiveAssetBatch(archive, batchPaths,
            parallel_worker_count(paths.size()), 64U * 1024U * 1024U,
            [&](archive_file::FileData &&file) {
              checkpoint();
              const auto &aliases = indices.at(file.path);
              const bool hasPast = std::any_of(aliases.begin(), aliases.end(),
                  [&](auto index) { return assets[index].latestPastStart.has_value(); });
              const auto duration = hasPast && !probed.contains(file.path) ? duration_bound::InspectAudio(file.bytes)
                                           : std::optional<duration_bound::Duration>{};
              bool defer = true;
              for (const auto index : aliases) {
                if (!probed.contains(file.path)) publishPastDuration(index, duration);
                defer = canDefer(index) && defer;
              }
              if (defer) {
                std::lock_guard lock(deferredMutex);
                if (deferred.size() < 2048 && file.bytes.size() <= maximumDeferredBytes - deferredBytes) {
                  deferredBytes += file.bytes.size();
                  deferred.push_back(std::move(file));
                  return !cancelled.load(std::memory_order_relaxed);
                }
              }
              for (const auto index : indices.at(file.path)) publishAsset(index, &file.bytes, &*batch);
              return !cancelled.load(std::memory_order_relaxed);
            }, &error, cancelled,
            [](const auto &archivePath, const auto &entries, auto onFile,
               auto workers, auto budget, auto *readError, auto keepGoing, auto) {
              // Keep concurrent extraction within its scheduling reservation.
              // Larger valid entries use the separately bounded fallback below.
              return archive_file::readArchiveEntriesConcurrently(
                  archivePath, entries, std::move(onFile), workers, budget,
                  readError, std::move(keepGoing),
                  archive_file::ConcurrentReadMemoryPolicy::Strict);
            },
            [](const auto &archivePath, const auto &entries, auto onFile,
               auto *readError, auto keepGoing) {
              return archive_file::readArchiveEntriesStreamingBounded(
                  archivePath, entries, std::move(onFile),
                  DecodedAssetCache::kMaximumEncodedAssetBytes, readError,
                  std::move(keepGoing));
            },
            [&](const std::filesystem::path &path) { return priorities.at(path); });
        };
        std::vector<std::filesystem::path> openingPaths, remainingPaths;
        for (const auto &path : paths) {
          const bool required = !zipProbed || std::any_of(indices.at(path).begin(), indices.at(path).end(),
              [&](auto index) { return !canDefer(index); });
          (required ? openingPaths : remainingPaths).push_back(path);
        }
        const bool loaded = readBatch(openingPaths) && readBatch(remainingPaths);
        if (!loaded) {
          checkpoint();
          throw std::runtime_error(error.empty() ? "Could not read preview audio archive." : error);
        }
        if (!cache->ArchiveBatchUnchanged(*batch))
          throw std::runtime_error("Preview audio archive changed during preparation.");
        // Merge cached conversions and retained encoded entries by their next
        // onset, leaving dead/hidden gameplay-only assets at the back.
        struct Work { std::uint64_t priority; std::size_t item; bool cached; };
        std::vector<Work> work;
        for (std::size_t item = 0; item < deferred.size(); ++item) {
          auto priority = std::numeric_limits<std::uint64_t>::max();
          for (auto index : indices.at(deferred[item].path)) priority = std::min(priority, futurePriority(index));
          work.push_back({priority, item, false});
        }
        for (std::size_t item = 0; item < cachedDeferred.size(); ++item)
          work.push_back({futurePriority(cachedDeferred[item].index), item, true});
        std::sort(work.begin(), work.end(), [](const auto &a, const auto &b) { return a.priority < b.priority; });
        ParallelWorkBatch workers(work.size());
        workers.run(work.size(), [&](std::size_t item) {
          const auto &job = work[item];
          if (job.cached) {
            const auto &asset = cachedDeferred[job.item];
            publishDecoded(asset.index, asset.native);
          } else {
            const auto &file = deferred[job.item];
            for (const auto index : indices.at(file.path)) publishAsset(index, &file.bytes, &*batch);
          }
        });
        if (!work.empty() && !cache->ArchiveBatchUnchanged(*batch))
          throw std::runtime_error("Preview audio archive changed during preparation.");
      }
      first = end;
    }
  }

  // Worker-only, before startupReady. Rebuilding after the scan retains each
  // overlapping voice's exact offset and leaves later pauses in the schedule.
  void prepareSchedule() {
    playback::ClearCallbackSounds(*callback);
    for (std::size_t index = 0; index < events.size(); ++index) {
      const auto &event = events[index];
      if (excluded(event)) continue;
      const auto *meta = assets[event.asset].sound->pagedOutput->metadata();
      const auto skipped = event.outputStart < originOutput ? originOutput - event.outputStart : 0;
      if (meta && skipped >= meta->frameCount) continue;
      const auto relative = event.outputStart > originOutput ? event.outputStart - originOutput : 0;
      const auto micros = nearestFrames(relative, 1000000, outputRate);
      if (micros > static_cast<std::uint64_t>(std::numeric_limits<long long>::max()))
        throw std::runtime_error("Preview schedule time is not representable.");
      auto &scheduled = callback->scheduledSounds[callback->scheduledSoundCount++];
      scheduled = {.soundData = assets[event.asset].sound.get(), .bus = event.bus,
                   .startMicros = static_cast<long long>(micros), .sequence = index,
                   .startFrame = static_cast<std::size_t>(skipped)};
    }
    callback->scheduledNonSystemSounds.store(callback->scheduledSoundCount,
                                             std::memory_order_relaxed);
  }

  std::optional<std::uint64_t> firstAudibleFrame(std::uint64_t first,
                                               std::uint64_t end) {
    std::array<float, 1024 * 2> mixed{};
    for (auto frame = first; frame < end;) {
      checkpoint();
      const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(1024, end - frame));
      const auto position = renderPosition(frame - originOutput);
      // The mixer admits due voices before retiring finished ones. A scan block
      // may be larger than a device callback; never let that difference drop a
      // sound and mistake the whole preview for silence.
      auto voices = callback->playingSoundCount;
      for (std::size_t i = 0; i < callback->scheduledSoundCount; ++i) {
        const auto start = nearestFrames(
            static_cast<std::uint64_t>(callback->scheduledSounds[i].startMicros),
            outputRate, 1000000);
        if (start >= frame - originOutput + count) break;
        if (++voices > kMaxActiveSounds) return originOutput;
      }
      if (!playback::ScheduledMixReady(*callback, position.micros, outputRate,
                                       count, 100, position.offset, true))
        throw std::runtime_error("Preview silence scan reached unprepared audio.");
      auto samples = std::span<float>(mixed).first(count * 2);
      std::fill(samples.begin(), samples.end(), 0.0f);
      playback::ActivateScheduledSounds(*callback, position.micros, outputRate,
                                         count, 100, position.offset);
      playback::MixActiveSounds(*callback, samples, count, 2,
                                gains.bgm, gains.keysound, 100);
      const auto audible = std::find_if(samples.begin(), samples.end(),
          // Match the player's int16 conversion; cancellation between voices
          // can leave floating-point residue that produces a zero PCM sample.
          [](float sample) { return std::abs(sample * 32767.0f) >= 1.0f; });
      if (audible != samples.end()) return frame + (audible - samples.begin()) / 2;
      frame += count;
    }
    return std::nullopt;
  }

  void run() noexcept {
    try {
      finalOutput = scaleFrames(timelineFrames44100, outputRate, kPreviewSampleRate, true);
      prepareSchedule();
      std::jthread loader([&] {
        try { loadAll(); }
        catch (const std::exception &error) {
          if (!cancelled.load(std::memory_order_relaxed)) fail(error.what());
        } catch (...) {
          if (!cancelled.load(std::memory_order_relaxed)) fail("Unknown preview loading error.");
        }
        loadingDone.store(true, std::memory_order_release);
        publishState();
      });
      auto waitUntil = [&](auto ready) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
          return cancelled.load(std::memory_order_relaxed) || failed.load(std::memory_order_acquire) || ready();
        });
        checkpoint();
        if (failed.load(std::memory_order_acquire)) throw std::runtime_error(state.error);
      };
      auto publishDuration = [&] {
        for (const auto &event : events)
          if (!excluded(event)) finalOutput = std::max(finalOutput, event.outputStart +
              assets[event.asset].sound->pagedOutput->metadata()->frameCount);
        durationOutput.store(finalOutput > originOutput ? finalOutput - originOutput : 0,
                              std::memory_order_relaxed);
        {
          std::lock_guard lock(mutex);
          state.durationKnown = true;
          state.durationFrames44100 = std::max(timelineFrames44100,
              scaleFrames(finalOutput, kPreviewSampleRate, outputRate, true));
        }
        durationKnown.store(true, std::memory_order_release);
        publishState();
      };
      std::size_t reached = 0;
      auto windowStart = originOutput;
      bool firstWindow = true;
      for (;;) {
        // With no active voice, only the next event can end leading silence.
        if (skipLeadingSilence && callback->playingSoundCount == 0)
          windowStart = std::max(windowStart, reached < events.size()
              ? events[reached].outputStart : finalOutput);
        const auto windowEnd = windowStart + static_cast<std::uint64_t>(outputRate) * 2;
        auto due = reached;
        while (due < events.size() && events[due].outputStart < windowEnd) ++due;
        waitUntil([&] {
          for (auto index = reached; index < due; ++index)
            if (!excluded(events[index]) &&
                !assets[events[index].asset].sound->pagedOutput->metadata()) return false;
          return true;
        });
        while (reached < due) {
          const auto &event = events[reached++];
          if (!excluded(event)) finalOutput = std::max(finalOutput, event.outputStart +
              assets[event.asset].sound->pagedOutput->metadata()->frameCount);
        }
        if (firstWindow) {
          // All past overlap decisions are now published. Rebuild before the
          // first scan/callback so excluded sounds cannot block the mixer.
          prepareSchedule();
          firstWindow = false;
        }
        const auto audible = skipLeadingSilence
            ? firstAudibleFrame(windowStart, std::min(windowEnd, finalOutput))
            : std::optional<std::uint64_t>{originOutput};
        const bool ended = reached == events.size() && windowEnd >= finalOutput;
        if (audible || ended) {
          const auto newOrigin = audible.value_or(std::max(originOutput, finalOutput));
          const auto skipped = newOrigin - originOutput;
          originOutput = newOrigin;
          prepareSchedule();
          if (reached == events.size()) publishDuration();
          {
            std::lock_guard lock(mutex);
            state.firstFrame44100 += scaleFrames(skipped, kPreviewSampleRate, outputRate);
            position44100.store(state.firstFrame44100, std::memory_order_relaxed);
            state.startupReady = true;
            ++state.revision;
          }
          startupReady.store(true, std::memory_order_release);
          changed.notify_all();
          break;
        }
        windowStart = windowEnd;
      }
      if (!durationKnown.load(std::memory_order_relaxed)) {
        waitUntil([&] {
          for (const auto &event : events)
            if (!excluded(event) && !assets[event.asset].sound->pagedOutput->metadata()) return false;
          return true;
        });
        publishDuration();
      }
      waitUntil([&] { return loadingDone.load(std::memory_order_acquire); });
      loader.join();
      checkpoint();
      { std::lock_guard lock(mutex); state.preparationComplete = true; ++state.revision; }
      changed.notify_all();
    } catch (const std::exception &error) {
      if (!cancelled.load(std::memory_order_relaxed)) fail(error.what());
    } catch (...) {
      if (!cancelled.load(std::memory_order_relaxed)) fail("Unknown preview preparation error.");
    }
  }
};

PagedPreviewSource::PagedPreviewSource(std::unique_ptr<Impl> value) : impl(std::move(value)) {}
PagedPreviewSource::~PagedPreviewSource() { Cancel(); }
std::shared_ptr<PagedPreviewSource> PagedPreviewSource::Create(
    const bms_parser::Chart &chart, std::shared_ptr<DecodedAssetCache> decodedAssets,
    std::uint64_t firstFrame44100, PagedPreviewGains gains) {
  auto value = std::make_unique<Impl>();
  value->cache = decodedAssets ? std::move(decodedAssets) :
      std::make_shared<DecodedAssetCache>(DecodedAssetCache::kUnlimitedRetainedBytes);
  value->gains = gains;
  value->state.firstFrame44100 = firstFrame44100;
  value->state.positionFrames44100 = firstFrame44100;
  value->position44100.store(firstFrame44100, std::memory_order_relaxed);
  value->timelineFrames44100 = framesForMicros(
      chart_playback_duration::ChartTimelineEndMicros(chart), kPreviewSampleRate, true);
  value->state.durationFrames44100 = value->timelineFrames44100;
  std::map<std::filesystem::path, std::size_t> assetsByPath;
  std::map<int, std::size_t> assetsByWav;
  auto snapshotAsset = [&](const std::string &name) {
    auto path = (chart.Meta.Folder / utf8_to_path_t(name)).lexically_normal();
    auto [it, added] = assetsByPath.emplace(path, value->assets.size());
    if (added) value->assets.push_back({.path = std::move(path), .sound = {}});
    return it->second;
  };
  // Definitions alone do not require decoding. Preserve non-audible gameplay
  // references (including hidden notes), as well as every scheduled event.
  for (const auto &[wav, name] : chart.ReferencedWavTable) {
    (void)wav;
    snapshotAsset(name);
  }
  const auto background = chart_audio::CollectBackgroundAudioEvents(chart);
  auto bgm = background.begin();
  for (const auto &event : chart_audio::CollectChartTimedAudioEvents(chart)) {
    const bool isBackground = bgm != background.end() &&
        bgm->timeMicros == event.timeMicros && bgm->wav == event.wav;
    if (isBackground) ++bgm;
    auto asset = assetsByWav.find(event.wav);
    if (asset == assetsByWav.end()) {
      const auto defined = chart.WavTable.find(event.wav);
      if (defined == chart.WavTable.end()) continue;
      asset = assetsByWav.emplace(event.wav, snapshotAsset(defined->second)).first;
    }
    value->events.push_back({event.timeMicros, asset->second, isBackground ? Bus::Bgm : Bus::Keysound});
  }
  return std::shared_ptr<PagedPreviewSource>(new PagedPreviewSource(std::move(value)));
}
bool PagedPreviewSource::Begin(int actualOutputRate, bool skipLeadingSilence) {
  if (impl->begun || impl->cancelled.load(std::memory_order_relaxed)) return false;
  impl->begun = true;
  try {
    if (actualOutputRate <= 0 || actualOutputRate > 768000)
      throw std::runtime_error("Preview backend sample rate is invalid.");
    impl->outputRate = actualOutputRate;
    impl->skipLeadingSilence = skipLeadingSilence;
    impl->originOutput = nearestFrames(impl->state.firstFrame44100,
                                       actualOutputRate, kPreviewSampleRate);
    if (impl->originOutput > std::numeric_limits<std::uint64_t>::max() -
                              static_cast<std::uint64_t>(actualOutputRate) * 2)
      throw std::runtime_error("Preview origin is not representable.");
    impl->requestedOriginOutput = impl->originOutput;
    impl->callback = std::make_unique<AudioCallbackState>();
    if (!playback::PrepareScheduledSoundCapacity(*impl->callback, impl->events.size()))
      throw std::runtime_error("Could not allocate preview schedule.");
    for (auto &asset : impl->assets) {
      asset.sound = std::make_unique<SoundData>();
      asset.sound->pagedOutput = std::make_shared<PagedPcmSound>();
    }
    impl->pastDurationFrames = std::make_unique<std::atomic<std::uint64_t>[]>(impl->assets.size());
    for (std::size_t index = 0; index < impl->assets.size(); ++index)
      impl->pastDurationFrames[index].store(std::numeric_limits<std::uint64_t>::max(), std::memory_order_relaxed);
    for (auto &event : impl->events) {
      event.outputStart = nearestFrames(static_cast<std::uint64_t>(std::max(0LL, event.timeMicros)),
                                        actualOutputRate, 1000000);
      auto &asset = impl->assets[event.asset];
      if (event.outputStart < impl->requestedOriginOutput)
        asset.latestPastStart = std::max(asset.latestPastStart.value_or(0), event.outputStart);
      else
        asset.firstFutureStart = std::min(asset.firstFutureStart.value_or(event.outputStart), event.outputStart);
    }
    impl->worker = std::jthread([state = impl.get()] { state->run(); });
    return true;
  } catch (const std::exception &error) {
    impl->fail(error.what());
  } catch (...) {
    impl->fail("Could not start preview preparation.");
  }
  return false;
}
PagedPreviewRenderResult PagedPreviewSource::CheckRender(std::uint32_t frameCount) const noexcept {
  return impl->checkRender(frameCount, true);
}
PagedPreviewRenderResult PagedPreviewSource::Render(std::span<float> stereo,
                                                   std::uint32_t frameCount) noexcept {
  if (stereo.size() / 2 < frameCount) return {};
  stereo = stereo.first(static_cast<std::size_t>(frameCount) * 2);
  std::fill(stereo.begin(), stereo.end(), 0.0f);
  const auto ready = impl->checkRender(frameCount, false);
  if (ready.status != PagedPreviewRenderStatus::Data) return ready;
  frameCount = ready.frames;
  const auto position = impl->renderPosition(impl->renderedOutput);
  playback::ActivateScheduledSounds(*impl->callback, position.micros, impl->outputRate,
                                    frameCount, 100, position.offset);
  playback::MixActiveSounds(*impl->callback, stereo, frameCount, 2,
                            impl->gains.bgm, impl->gains.keysound, 100);
  impl->renderedOutput += frameCount;
  const auto chartFrames = scaleFramesBounded(impl->renderedOutput,
                                               kPreviewSampleRate, impl->outputRate);
  impl->position44100.store(impl->state.firstFrame44100 + chartFrames, std::memory_order_relaxed);
  return {PagedPreviewRenderStatus::Data, frameCount};
}
PagedPreviewState PagedPreviewSource::GetState() const {
  std::lock_guard lock(impl->mutex);
  auto result = impl->state;
  result.positionFrames44100 = impl->position44100.load(std::memory_order_relaxed);
  return result;
}
void PagedPreviewSource::WaitForChange(std::uint64_t revision, std::chrono::milliseconds timeout) {
  std::unique_lock lock(impl->mutex);
  impl->changed.wait_for(lock, timeout, [&] { return impl->state.revision != revision; });
}
void PagedPreviewSource::Cancel() {
  impl->cancelled.store(true, std::memory_order_relaxed);
  { std::lock_guard lock(impl->mutex);
    if (!impl->state.cancelled) { impl->state.cancelled = true; ++impl->state.revision; } }
  impl->changed.notify_all();
  if (impl->worker.joinable()) impl->worker.join();
}
} // namespace audio
