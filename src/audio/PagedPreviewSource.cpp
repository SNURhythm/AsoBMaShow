#include "PagedPreviewSource.h"

#include "../ChartPlaybackDuration.h"
#include "../path.h"
#include "../utils/ParallelWorkBatch.h"
#include "AudioMix.h"
#include "ChartAudioRenderer.h"
#include "DecodedAssetCache.h"
#include "PcmResampler.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

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
} // namespace

struct PagedPreviewSource::Impl {
  struct Asset {
    std::filesystem::path path;
    SF_INFO info{};
    std::unique_ptr<SoundData> sound;
    bool staged = false;
    std::size_t preparedPages = 0;
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

  struct RenderPosition { long long micros; std::uint32_t offset; };
  RenderPosition renderPosition() const noexcept {
    // Integer-second epochs preserve scheduler rounding across scratch chunks.
    return {static_cast<long long>((renderedOutput / outputRate) * 1000000),
            static_cast<std::uint32_t>(renderedOutput % outputRate)};
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
    const auto position = renderPosition();
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
  void stageAssets(std::vector<std::size_t> indices) {
    std::erase_if(indices, [&](std::size_t index) { return assets[index].staged; });
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (indices.empty()) return;
    std::vector<std::filesystem::path> paths;
    paths.reserve(indices.size());
    for (const auto index : indices) paths.push_back(assets[index].path);
    std::string error;
    if (!cache->StageArchiveAssets(paths, cancelled, {}, &error)) {
      checkpoint();
      throw std::runtime_error(error.empty() ? "Could not stage preview audio." : error);
    }
    for (const auto index : indices) assets[index].staged = true;
  }
  void inspect(Asset &asset) {
    checkpoint();
    if (asset.sound->pagedOutput->metadata()) return;
    DecodedAssetCache::FailureKind failure{};
    std::string error;
    if (!cache->Inspect(asset.path, asset.info, cancelled, {}, &error, &failure)) {
      checkpoint();
      if (failure != DecodedAssetCache::FailureKind::InvalidAsset)
        throw std::runtime_error(error.empty() ? "Could not inspect preview audio." : error);
      if (!asset.sound->pagedOutput->publishMetadata(0, 0))
        throw std::runtime_error("Could not publish empty preview audio.");
      return;
    }
    if (asset.info.frames <= 0 || asset.info.channels <= 0 || asset.info.samplerate <= 0) {
      if (!asset.sound->pagedOutput->publishMetadata(0, 0))
        throw std::runtime_error("Could not publish empty preview audio.");
      return;
    }
    const pcm_resampling::Ratio ratio(asset.info.samplerate, outputRate);
    const auto count = ratio.outputFrames(static_cast<std::size_t>(asset.info.frames),
                                          UINT32_MAX);
    if (!count || !asset.sound->pagedOutput->publishMetadata(asset.info.channels, *count))
      throw std::runtime_error("Preview audio length is not representable.");
  }

  DecodedAssetCache::DecoderPin pinDecoder(Asset &asset) {
    checkpoint();
    DecodedAssetCache::FailureKind failure{};
    std::string error;
    auto pin = cache->PinDecoder(asset.path, cancelled, {}, &error, &failure);
    if (!pin && failure != DecodedAssetCache::FailureKind::InvalidAsset) {
      checkpoint();
      throw std::runtime_error(error.empty() ? "Could not retain preview decoder." : error);
    }
    return pin;
  }

  // Each contiguous missing output-page run leases one decoder/cache ReadRange,
  // including the native page holding the right interpolation endpoint.
  void prepare(Asset &asset, std::uint64_t first, std::uint64_t end) {
    inspect(asset);
    const auto *meta = asset.sound->pagedOutput->metadata();
    end = std::min<std::uint64_t>(end, meta->frameCount);
    if (first >= end) return;
    constexpr auto pageFrames = PagedPcmSound::kPageFrames;
    auto page = static_cast<std::size_t>(first / pageFrames);
    const auto endPage = static_cast<std::size_t>((end - 1) / pageFrames + 1);
    const pcm_resampling::Ratio ratio(asset.info.samplerate, outputRate);
    while (page < endPage) {
      checkpoint();
      if (meta->pageSamples(page)) { ++page; continue; }
      const auto firstPage = page++;
      while (page < endPage && !meta->pageSamples(page)) ++page;
      const auto firstOutput = firstPage * pageFrames;
      const auto endOutput = std::min(page * pageFrames, meta->frameCount);
      pcm_resampling::Cursor cursor(ratio, static_cast<std::size_t>(asset.info.frames));
      cursor.seek(firstOutput);
      const auto firstNative = cursor.left();
      cursor.seek(endOutput - 1);
      const auto endNative = cursor.right() + 1;
      std::vector<short> native;
      SF_INFO info{};
      std::string error;
      if (!cache->ReadRange(asset.path, firstNative, endNative - firstNative,
                            native, info, cancelled, {}, &error)) {
        checkpoint();
        // Metadata already succeeded: a later read failure is operational,
        // never retroactively replace published sound metadata with empty PCM.
        throw std::runtime_error(error.empty() ? "Could not prepare preview audio." : error);
      }
      if (info.channels != asset.info.channels || info.frames != asset.info.frames ||
          info.samplerate != asset.info.samplerate ||
          native.size() != (endNative - firstNative) * static_cast<std::size_t>(info.channels))
        throw std::runtime_error("Preview audio changed during preparation.");
      const auto channels = static_cast<std::size_t>(info.channels);
      std::vector<short> output(pageFrames * channels);
      cursor.seek(firstOutput);
      for (auto publish = firstPage; publish < page; ++publish) {
        checkpoint();
        const auto frames = std::min(pageFrames, meta->frameCount - publish * pageFrames);
        for (std::size_t frame = 0; frame < frames; ++frame) {
          const auto left = (cursor.left() - firstNative) * channels;
          const auto right = (cursor.right() - firstNative) * channels;
          for (std::size_t channel = 0; channel < channels; ++channel)
            output[frame * channels + channel] = ratio.direct() ? native[left + channel] :
                cursor.interpolate(native[left + channel], native[right + channel]);
          cursor.advance();
        }
        if (!asset.sound->pagedOutput->publishPage(publish,
                std::span<const short>(output.data(), frames * channels)))
          throw std::runtime_error("Could not publish preview PCM page.");
        ++asset.preparedPages;
      }
    }
  }

  void run() noexcept {
    try {
      struct ActiveAsset {
        DecodedAssetCache::DecoderPin pin;
        std::size_t voices = 0;
      };
      // These pins follow preparation windows, never playback/session lifetime.
      // Stack ownership releases every pin on completion, cancellation or error.
      std::vector<ActiveAsset> activeAssets(assets.size());
      ParallelWorkBatch preparation(assets.size());
      const auto outputComplete = [](const Asset &asset) {
        const auto *metadata = asset.sound->pagedOutput->metadata();
        return metadata && asset.preparedPages == metadata->pageCount;
      };
      finalOutput = scaleFrames(timelineFrames44100, outputRate, kPreviewSampleRate, true);
      // Before callbacks start, classify all earlier events so expired voices
      // are omitted and every overlapping tail is scheduled at its offset.
      std::size_t nextEvent = 0;
      std::vector<std::size_t> active;
      active.reserve(events.size());
      auto reach = [&](std::size_t index) {
        auto &event = events[index];
        auto &asset = assets[event.asset];
        auto &retained = activeAssets[event.asset];
        if (!retained.pin && !outputComplete(asset)) retained.pin = pinDecoder(asset);
        inspect(asset);
        const auto length = asset.sound->pagedOutput->metadata()->frameCount;
        const auto tail = event.outputStart + length;
        finalOutput = std::max(finalOutput, tail);
        if (tail > originOutput && length != 0) {
          active.push_back(index);
          ++retained.voices;
        } else if (!retained.voices) {
          retained.pin = {};
        }
      };
      // Prepare two seconds at arbitrary origin, then short forward windows.
      std::uint64_t windowStart = originOutput;
      auto windowEnd = originOutput + static_cast<std::uint64_t>(outputRate) * 2;
      auto stagedThrough = windowEnd;
      std::size_t nextStagedEvent = 0;
      auto stageThrough = [&](std::uint64_t end) {
        std::vector<std::size_t> indices;
        while (nextStagedEvent < events.size() && events[nextStagedEvent].outputStart < end)
          indices.push_back(events[nextStagedEvent++].asset);
        stageAssets(std::move(indices));
      };
      // Keep archive extraction bounded by the next two seconds of event
      // requests. Stage opening/highlight tails first; later windows never wait
      // for the chart's unused or distant assets to be extracted in archive order.
      stageThrough(stagedThrough);
      while (nextEvent < events.size() && events[nextEvent].outputStart <= originOutput)
        reach(nextEvent++);
      struct Range { std::uint64_t first, end; };
      std::vector<std::vector<Range>> ranges(assets.size());
      std::vector<std::size_t> work;
      work.reserve(assets.size());
      bool initial = true;
      for (;;) {
        checkpoint();
        if (windowEnd > stagedThrough) {
          stagedThrough = windowStart + static_cast<std::uint64_t>(outputRate) * 2;
          stageThrough(stagedThrough);
        }
        while (nextEvent < events.size() && events[nextEvent].outputStart < windowEnd)
          reach(nextEvent++);
        // Voices can share an asset. Give each asset one task so page writes
        // and decoder cursors stay exclusive while independent sounds overlap.
        work.clear();
        for (const auto index : active) {
          const auto &event = events[index];
          if (outputComplete(assets[event.asset])) continue;
          if (ranges[event.asset].empty()) work.push_back(event.asset);
          const auto first = windowStart > event.outputStart ? windowStart - event.outputStart : 0;
          const auto end = windowEnd - event.outputStart + 1;
          ranges[event.asset].push_back({first, end});
        }
        preparation.run(work.size(), [&](std::size_t index) {
          const auto asset = work[index];
          for (const auto &range : ranges[asset])
            prepare(assets[asset], range.first, range.end);
        });
        for (const auto asset : work) {
          ranges[asset].clear();
          // Repeated voices consume immutable output pages after preparation.
          if (outputComplete(assets[asset])) activeAssets[asset].pin = {};
        }
        std::erase_if(active, [&](std::size_t index) {
          const auto &event = events[index];
          if (event.outputStart + assets[event.asset].sound->pagedOutput->metadata()->frameCount > windowEnd)
            return false;
          auto &retained = activeAssets[event.asset];
          if (--retained.voices == 0) retained.pin = {};
          return true;
        });
        if (nextEvent == events.size() && !durationKnown.load(std::memory_order_relaxed)) {
          durationOutput.store(finalOutput > originOutput ? finalOutput - originOutput : 0,
                                std::memory_order_relaxed);
          { std::lock_guard lock(mutex);
            state.durationKnown = true;
            state.durationFrames44100 = std::max(timelineFrames44100,
                scaleFrames(finalOutput, kPreviewSampleRate, outputRate, true)); }
          durationKnown.store(true, std::memory_order_release);
          publishState();
        }
        if (initial) {
          // Schedule writes end before startupReady's release publication. Later
          // preparation only touches immutable owners' atomic page slots.
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
          { std::lock_guard lock(mutex); state.startupReady = true; ++state.revision; }
          startupReady.store(true, std::memory_order_release);
          changed.notify_all();
          initial = false;
        }
        if (nextEvent == events.size() && active.empty() && windowEnd >= finalOutput) break;
        windowStart = windowEnd;
        windowEnd += std::max(1, outputRate / 4);
      }
      // Playback consumption has no effect on warming: prepare the prefixes,
      // unused selected WAVs and referenced WAV variants as well.
      std::vector<std::size_t> remaining;
      for (std::size_t index = 0; index < assets.size(); ++index)
        if (!assets[index].staged) remaining.push_back(index);
      stageAssets(std::move(remaining));
      preparation.run(assets.size(), [&](std::size_t index) {
        auto &asset = assets[index];
        auto pin = pinDecoder(asset);
        inspect(asset);
        const auto count = asset.sound->pagedOutput->metadata()->frameCount;
        const auto step = static_cast<std::uint64_t>(std::max(1, outputRate / 4));
        for (std::uint64_t first = 0; first < count; first += step)
          prepare(asset, first, std::min<std::uint64_t>(count, first + step));
        // Downsampling can skip a final native page (or entire pages at very
        // large ratios). Complete means gameplay can reuse every native frame,
        // including samples no output interpolation endpoint ever requested.
        if (count != 0) {
          std::string error;
          if (!cache->Preload(asset.path, cancelled, {}, &error)) {
            checkpoint();
            // This asset already published valid metadata; disappearance or a
            // later decode failure cannot turn it retroactively into empty PCM.
            throw std::runtime_error(error.empty() ? "Could not retain preview native audio." : error);
          }
        }
      });
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
    if (added) value->assets.push_back({.path = std::move(path), .info = {}, .sound = {}});
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
bool PagedPreviewSource::Begin(int actualOutputRate) {
  if (impl->begun || impl->cancelled.load(std::memory_order_relaxed)) return false;
  impl->begun = true;
  try {
    if (actualOutputRate <= 0 || actualOutputRate > 768000)
      throw std::runtime_error("Preview backend sample rate is invalid.");
    impl->outputRate = actualOutputRate;
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
  const auto position = impl->renderPosition();
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
