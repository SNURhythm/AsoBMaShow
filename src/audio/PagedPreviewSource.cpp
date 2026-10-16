#include "PagedPreviewSource.h"

#include "../ArchiveFile.h"
#include "../ChartPlaybackDuration.h"
#include "../path.h"
#include "../utils/ParallelWorkBatch.h"
#include "AudioMix.h"
#include "ArchiveAssetBatchLoader.h"
#include "ChartAssetExtensions.h"
#include "ChartAudioRenderer.h"
#include "DecodedAssetCache.h"
#include "PcmResampler.h"

#include <SDL3/SDL_log.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace audio {
namespace {
template <typename... Args>
void logPreparation(const char *format, Args... args) noexcept {
  try {
    char message[512];
    std::snprintf(message, sizeof(message), format, args...);
    SDL_Log("%s", message);
    archive_file::appendDebugLogLine(message);
  } catch (...) {
    // Diagnostics must not turn a successful preparation into a failure.
  }
}

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
} // namespace

struct PagedPreviewSource::Impl {
  struct Asset {
    std::filesystem::path path;
    std::unique_ptr<SoundData> sound;
  };
  struct Event {
    long long timeMicros;
    std::size_t asset;
    Bus bus;
    std::uint64_t outputStart = 0;
  };
  std::vector<Asset> assets;
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
  void fail(std::string error) {
    { std::lock_guard lock(mutex);
      state.error = std::move(error);
      failed.store(true, std::memory_order_release);
      ++state.revision; }
    changed.notify_all();
  }
  void publishAsset(std::size_t index, const std::vector<unsigned char> *bytes = nullptr) {
    checkpoint();
    auto &asset = assets[index];
    DecodedAssetCache::FailureKind failure{};
    std::string error;
    const auto native = bytes ? cache->LoadBytes(asset.path, *bytes, cancelled, {}, &error, &failure)
                              : cache->Load(asset.path, cancelled, {}, &error, &failure);
    if (!native) {
      checkpoint();
      if (failure != DecodedAssetCache::FailureKind::InvalidAsset)
        throw std::runtime_error(error.empty() ? "Could not load preview audio." : error);
      asset.sound->pagedOutput->publishMetadata(0, 0);
    } else {
      const pcm_resampling::Ratio ratio(native->info.samplerate, outputRate);
      if (!ratio.outputFrames(static_cast<std::size_t>(native->info.frames), UINT32_MAX))
        throw std::runtime_error("Preview audio length is not representable.");
      auto output = ResamplePcm(native->pcm, native->info.channels,
                                native->info.samplerate, outputRate);
      checkpoint();
      if ((!native->pcm.empty() && output.empty()) ||
          !asset.sound->pagedOutput->publishFull(native->info.channels, std::move(output)))
        throw std::runtime_error("Could not publish preview audio.");
    }
    publishState();
  }

  void loadAll() {
    // One continuous whole-asset queue: event order first, unused variants last.
    std::vector<std::size_t> ordered;
    std::vector<bool> seen(assets.size());
    for (const auto &event : events)
      if (!seen[event.asset]) { seen[event.asset] = true; ordered.push_back(event.asset); }
    for (std::size_t index = 0; index < assets.size(); ++index)
      if (!seen[index]) ordered.push_back(index);
    static const std::vector<std::string_view> extensions(
        asobmshow::chart_assets::kAudioExtensions.begin(),
        asobmshow::chart_assets::kAudioExtensions.end());
    for (const auto index : ordered) {
      checkpoint();
      if (auto resolved = archive_file::findFileWithExtensions(assets[index].path, extensions))
        assets[index].path = std::move(*resolved);
    }
    // A chart normally has one source. Contiguous source groups also support
    // mixed loose/archive assets without creating nested worker pools.
    for (std::size_t first = 0; first < ordered.size();) {
      checkpoint();
      std::filesystem::path archive, inner;
      const bool archived = archive_file::splitVirtualPath(assets[ordered[first]].path, archive, inner);
      auto end = first + 1;
      while (end < ordered.size()) {
        std::filesystem::path nextArchive, nextInner;
        const bool nextArchived = archive_file::splitVirtualPath(assets[ordered[end]].path,
                                                                nextArchive, nextInner);
        if (nextArchived != archived || (archived && nextArchive != archive)) break;
        ++end;
      }
      if (!archived) {
        ParallelWorkBatch workers(end - first);
        workers.run(end - first, [&](std::size_t item) { publishAsset(ordered[first + item]); });
      } else {
        std::vector<std::filesystem::path> paths;
        std::map<std::filesystem::path, std::vector<std::size_t>> indices;
        std::map<std::filesystem::path, std::size_t> priorities;
        for (auto item = first; item < end; ++item) {
          const auto index = ordered[item];
          if (cache->Contains(assets[index].path) || !archive_file::exists(assets[index].path)) {
            publishAsset(index);
            continue;
          }
          archive_file::splitVirtualPath(assets[index].path, archive, inner);
          if (!indices.contains(inner)) { paths.push_back(inner); priorities.emplace(inner, item); }
          indices[inner].push_back(index);
        }
        std::string error;
        const bool loaded = ConsumeArchiveAssetBatch(archive, paths,
            parallel_worker_count(paths.size()), 64U * 1024U * 1024U,
            [&](archive_file::FileData &&file) {
              for (const auto index : indices.at(file.path)) publishAsset(index, &file.bytes);
              return !cancelled.load(std::memory_order_relaxed);
            }, &error, cancelled, archive_file::readArchiveEntriesConcurrently,
            archive_file::readArchiveEntriesStreaming,
            [&](const std::filesystem::path &path) { return priorities.at(path); });
        if (!loaded) {
          checkpoint();
          throw std::runtime_error(error.empty() ? "Could not read preview audio archive." : error);
        }
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
    const auto started = std::chrono::steady_clock::now();
    const auto milliseconds = [](auto duration) {
      return std::chrono::duration<double, std::milli>(duration).count();
    };
    double startupMs = -1.0;
    bool complete = false;
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
          finalOutput = std::max(finalOutput, event.outputStart +
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
      for (;;) {
        const auto windowEnd = windowStart + static_cast<std::uint64_t>(outputRate) * 2;
        auto due = reached;
        while (due < events.size() && events[due].outputStart < windowEnd) ++due;
        waitUntil([&] {
          for (auto index = reached; index < due; ++index)
            if (!assets[events[index].asset].sound->pagedOutput->metadata()) return false;
          return true;
        });
        while (reached < due) {
          const auto &event = events[reached++];
          finalOutput = std::max(finalOutput, event.outputStart +
              assets[event.asset].sound->pagedOutput->metadata()->frameCount);
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
          startupMs = milliseconds(std::chrono::steady_clock::now() - started);
          logPreparation("[preview-prepare] startup: %.2f ms assets=%zu events=%zu",
                          startupMs, assets.size(), events.size());
          break;
        }
        windowStart = windowEnd;
      }
      if (!durationKnown.load(std::memory_order_relaxed)) {
        waitUntil([&] {
          for (const auto &event : events)
            if (!assets[event.asset].sound->pagedOutput->metadata()) return false;
          return true;
        });
        publishDuration();
      }
      waitUntil([&] { return loadingDone.load(std::memory_order_acquire); });
      loader.join();
      checkpoint();
      { std::lock_guard lock(mutex); state.preparationComplete = true; ++state.revision; }
      complete = true;
      changed.notify_all();
    } catch (const std::exception &error) {
      if (!cancelled.load(std::memory_order_relaxed)) fail(error.what());
    } catch (...) {
      if (!cancelled.load(std::memory_order_relaxed)) fail("Unknown preview preparation error.");
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto statistics = cache->GetStatistics();
    logPreparation("[preview-prepare] %s: totalMs=%.2f startupMs=%.2f assets=%zu decoderOpens=%llu individualArchiveReads=%llu "
            "decodedFrames=%llu cacheHitFrames=%llu",
            complete ? "complete" : cancelled.load(std::memory_order_relaxed) ? "cancelled" : "failed",
            milliseconds(elapsed), startupMs, assets.size(),
            static_cast<unsigned long long>(statistics.decoderOpens),
            static_cast<unsigned long long>(statistics.archiveReads),
            static_cast<unsigned long long>(statistics.decodedFrames),
            static_cast<unsigned long long>(statistics.cacheHitFrames));
  }
};

PagedPreviewSource::PagedPreviewSource(std::unique_ptr<Impl> value) : impl(std::move(value)) {}
PagedPreviewSource::~PagedPreviewSource() { Cancel(); }
std::shared_ptr<PagedPreviewSource> PagedPreviewSource::Create(
    const bms_parser::Chart &chart, std::shared_ptr<DecodedAssetCache> decodedAssets,
    std::uint64_t firstFrame44100, PagedPreviewGains gains) {
  auto value = std::make_unique<Impl>();
  value->cache = decodedAssets ? std::move(decodedAssets) :
      std::make_shared<DecodedAssetCache>(std::filesystem::path{}, DecodedAssetCache::kUnlimitedRetainedBytes);
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
  for (const auto &[wav, name] : chart.WavTable) assetsByWav.emplace(wav, snapshotAsset(name));
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
    if (asset == assetsByWav.end()) continue;
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
    impl->callback = std::make_unique<AudioCallbackState>();
    if (!playback::PrepareScheduledSoundCapacity(*impl->callback, impl->events.size()))
      throw std::runtime_error("Could not allocate preview schedule.");
    for (auto &asset : impl->assets) {
      asset.sound = std::make_unique<SoundData>();
      asset.sound->pagedOutput = std::make_shared<PagedPcmSound>();
    }
    for (auto &event : impl->events)
      event.outputStart = nearestFrames(static_cast<std::uint64_t>(std::max(0LL, event.timeMicros)),
                                        actualOutputRate, 1000000);
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
