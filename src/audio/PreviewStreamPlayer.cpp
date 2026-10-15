#include "PreviewStreamPlayer.h"
#include "ProgressiveMusicSource.h"
#include "PreviewSpectrumAnalyzer.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace audio {
namespace {
constexpr std::size_t kSourceWindowFrames = 4096;
constexpr std::size_t kOutputBatchFrames = 4096;

static_assert(std::atomic<float>::is_always_lock_free);

struct PreviewSession {
  explicit PreviewSession(std::atomic<float> &level, PreviewSpectrumAnalyzer &analyzer)
      : outputLevel(level), spectrum(analyzer) {}
  std::atomic<float> &outputLevel;
  PreviewSpectrumAnalyzer &spectrum;
  std::shared_ptr<music_stream::Source> source;
  std::unique_ptr<IBackend> backend;
  std::uint64_t firstFrame = 0;
  std::uint32_t outputRate = 0;
  std::vector<short> ring;
  std::size_t capacity = 0;
  std::size_t startupFrames = 0;
  // Exactly one producer (feeder) and one consumer (backend callback).
  std::atomic<std::uint64_t> written{0};
  std::atomic<std::uint64_t> consumed{0};
  std::atomic_bool active{true};
  std::atomic_bool sourceEnded{false};
  std::atomic_bool deviceStarted{false};
  RenderTiming pendingTiming; // Backend callback thread only.
  std::atomic<std::int64_t> lastOutputEndMicros{0};
  std::atomic<std::uint32_t> lastOutputBufferFrames{0};
  std::atomic<std::uint64_t> endFrame{std::numeric_limits<std::uint64_t>::max()};
  std::atomic<float> gain{1.0f};
  std::mutex waitMutex;
  std::condition_variable wake;
  mutable std::mutex errorMutex;
  std::string error;
  std::thread feeder;

  // Feeder-owned native-source window and exact rational resampling phase.
  std::array<short, (kSourceWindowFrames + 1) * music_stream::kChannels> window{};
  std::uint64_t windowFirst = 0;
  std::size_t windowFrames = 0;
  std::uint64_t cursor = 0;
  std::uint64_t fraction = 0;
  bool inputEnded = false;

  ~PreviewSession() { Stop(); }

  void Stop() {
    {
      std::lock_guard lock(waitMutex);
      active.store(false, std::memory_order_release);
    }
    if (source) {
      source->SetRenderingEnabled(false);
      source->Cancel();
    }
    wake.notify_all();
    if (feeder.joinable()) feeder.join();
    if (backend) {
      std::string ignored;
      (void)backend->stop(ignored);
      // Destruction also quiesces native callbacks if stop() reported failure.
      backend.reset();
    }
    deviceStarted.store(false, std::memory_order_release);
    outputLevel.store(0.0f, std::memory_order_relaxed);
    spectrum.reset();
  }

  void Fail(std::string message) {
    {
      std::lock_guard lock(errorMutex);
      error = std::move(message);
      SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Preview stream: %s", error.c_str());
    }
    active.store(false, std::memory_order_release);
    source->SetRenderingEnabled(false);
  }

  static void Timing(RenderTiming timing, void *context) {
    static_cast<PreviewSession *>(context)->pendingTiming = timing;
  }

  static void Render(void *output, std::uint32_t frameCount, int channels,
                     void *context) {
    auto &session = *static_cast<PreviewSession *>(context);
    const auto timing = session.pendingTiming;
    session.pendingTiming = {};
    if (!output || channels <= 0) return;
    auto *pcm = static_cast<short *>(output);
    std::fill_n(pcm, static_cast<std::size_t>(frameCount) * channels, short{0});
    if (!session.active.load(std::memory_order_acquire)) {
      session.outputLevel.store(0.0f, std::memory_order_relaxed);
      session.spectrum.push(pcm, frameCount, channels, session.outputRate);
      return;
    }
    const auto first = session.consumed.load(std::memory_order_relaxed);
    const auto end = session.written.load(std::memory_order_acquire);
    const auto frames = std::min<std::uint64_t>(frameCount, end - first);
    const auto gain = session.gain.load(std::memory_order_relaxed);
    double energy = 0.0;
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
      const auto offset = static_cast<std::size_t>((first + frame) % session.capacity) * 2;
      const auto left = session.ring[offset];
      const auto right = session.ring[offset + 1];
      if (channels == 1) {
        pcm[frame] = static_cast<short>((static_cast<int>(left) + right) * 0.5f * gain);
      } else {
        pcm[frame * channels] = static_cast<short>(left * gain);
        pcm[frame * channels + 1] = static_cast<short>(right * gain);
      }
      const double leftOutput = pcm[frame * channels] / 32768.0;
      const double rightOutput = channels == 1 ? leftOutput : pcm[frame * channels + 1] / 32768.0;
      energy += (leftOutput * leftOutput + rightOutput * rightOutput) * 0.5;
    }
    session.outputLevel.store(frameCount ? static_cast<float>(std::sqrt(energy / frameCount)) : 0.0f,
                              std::memory_order_relaxed);
    session.spectrum.push(pcm, frameCount, channels, session.outputRate);
    if (frames) {
      const auto durationMicros = static_cast<std::int64_t>(
          (frames * 1000000ULL + session.outputRate - 1) / session.outputRate);
      session.lastOutputEndMicros.store(
          timing.outputTimestampKnown && timing.outputSteadyMicros > 0
              ? timing.outputSteadyMicros + durationMicros : 0,
          std::memory_order_relaxed);
      session.lastOutputBufferFrames.store(frameCount, std::memory_order_relaxed);
    }
    // Silence inserted during starvation never advances the music position.
    session.consumed.store(first + frames, std::memory_order_release);
  }

  enum class SampleStatus { Ready, Pending, End, Stopped };

  bool WindowHasSample(bool needRight) const {
    if (!windowFrames || cursor < windowFirst || cursor >= windowFirst + windowFrames)
      return false;
    return inputEnded || cursor + (needRight ? 1 : 0) < windowFirst + windowFrames;
  }

  SampleStatus FillWindow(bool needRight) {
    if (inputEnded && cursor >= endFrame.load(std::memory_order_relaxed))
      return SampleStatus::End;
    const auto required = cursor + (needRight ? 1 : 0);
    if (windowFrames && cursor >= windowFirst &&
        required < windowFirst + windowFrames) return SampleStatus::Ready;
    if (inputEnded && windowFrames && cursor >= windowFirst &&
        cursor < windowFirst + windowFrames) return SampleStatus::Ready;

    // Retain the left interpolation endpoint when the next frame has not been
    // published yet. Read at the missing frame so Pending actually waits.
    std::size_t retained = 0;
    if (windowFrames && cursor >= windowFirst && cursor < windowFirst + windowFrames) {
      const auto offset = static_cast<std::size_t>(cursor - windowFirst) * 2;
      window[0] = window[offset];
      window[1] = window[offset + 1];
      retained = 1;
    }
    windowFirst = cursor;
    windowFrames = retained;
    const auto requested = cursor + retained;
    auto destination = std::span<short>(window).subspan(retained * 2, kSourceWindowFrames * 2);
    const auto read = source->ReadFrames(requested, destination, std::chrono::milliseconds(100));
    if (!active.load(std::memory_order_acquire)) return SampleStatus::Stopped;
    switch (read.status) {
    case music_stream::ReadStatus::Pending:
      return SampleStatus::Pending;
    case music_stream::ReadStatus::Cancelled:
      active.store(false, std::memory_order_release);
      source->SetRenderingEnabled(false);
      return SampleStatus::Stopped;
    case music_stream::ReadStatus::Error:
      Fail(read.error.empty() ? "Could not prepare preview audio" : read.error);
      return SampleStatus::Stopped;
    case music_stream::ReadStatus::End:
      inputEnded = true;
      {
        const auto state = source->GetState();
        endFrame.store(state.complete ? state.durationFrames : requested,
                       std::memory_order_relaxed);
      }
      return retained ? SampleStatus::Ready : SampleStatus::End;
    case music_stream::ReadStatus::Data:
      if (!read.frames || read.frames > kSourceWindowFrames) {
        Fail("Invalid preview audio block");
        return SampleStatus::Stopped;
      }
      windowFrames += read.frames;
      if (required >= windowFirst + windowFrames) return SampleStatus::Pending;
      return SampleStatus::Ready;
    }
    return SampleStatus::Stopped;
  }

  short Sample(std::size_t channel) const {
    const auto offset = static_cast<std::size_t>(cursor - windowFirst) * 2;
    const auto left = window[offset + channel];
    if (!fraction) return left;
    const auto next = std::min(static_cast<std::size_t>(cursor - windowFirst) + 1,
                               windowFrames - 1) * 2;
    const auto right = window[next + channel];
    const auto weighted = static_cast<std::int64_t>(left) * static_cast<std::int64_t>(outputRate - fraction) +
                          static_cast<std::int64_t>(right) * static_cast<std::int64_t>(fraction);
    return static_cast<short>((weighted + (weighted >= 0 ? outputRate / 2 :
                                          -static_cast<std::int64_t>(outputRate / 2))) /
                              outputRate);
  }

  void Advance() {
    cursor += music_stream::kSampleRate / outputRate;
    fraction += music_stream::kSampleRate % outputRate;
    if (fraction >= outputRate) {
      fraction -= outputRate;
      ++cursor;
    }
  }

  bool StartOutputIfReady(bool final) {
    if (deviceStarted.load(std::memory_order_relaxed)) return true;
    const auto queued = written.load(std::memory_order_relaxed);
    if (!final && queued < startupFrames) return true;
    if (!active.load(std::memory_order_acquire)) return false;
    if (!queued && final) return true;
    std::string diagnostic;
    if (!backend->start(diagnostic)) {
      Fail(diagnostic.empty() ? "Could not start preview audio device" : diagnostic);
      return false;
    }
    deviceStarted.store(true, std::memory_order_release);
    return true;
  }

  void FinishOutput() {
    if (!deviceStarted.load(std::memory_order_acquire)) return;
    std::unique_lock lock(waitMutex);
    while (active.load(std::memory_order_acquire) &&
           consumed.load(std::memory_order_acquire) < written.load(std::memory_order_acquire)) {
      wake.wait_for(lock, std::chrono::milliseconds(100), [&] {
        return !active.load(std::memory_order_acquire);
      });
    }
    if (!active.load(std::memory_order_acquire)) return;
    lock.unlock();
    // Consumed means copied into a native buffer, not yet heard. Preserve its
    // presentation tail before stopping CoreAudio, whose stop need not drain.
    const auto runtime = backend->runtimeState();
    const double nativePeriod = runtime.effectiveSampleRate
        ? static_cast<double>(runtime.effectiveBufferFrames) / runtime.effectiveSampleRate : 0;
    const auto reserve = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(2 * std::max(nativePeriod,
            static_cast<double>(lastOutputBufferFrames.load(std::memory_order_relaxed)) / outputRate)));
    const auto outputEnd = lastOutputEndMicros.load(std::memory_order_relaxed);
    auto deadline = std::chrono::steady_clock::now();
    if (outputEnd > 0) {
      deadline = std::chrono::steady_clock::time_point(std::chrono::microseconds(outputEnd)) + reserve;
    } else {
      // No fabricated timestamp: an unknown route gets a conservative, bounded
      // grace period plus any reported latency and native buffering reserve.
      const double latency = std::isfinite(runtime.effectiveLatencyMs)
          ? std::max(0.0, runtime.effectiveLatencyMs) / 1000.0 : 0;
      deadline += std::chrono::seconds(1) + reserve +
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(latency));
    }
    lock.lock();
    if (wake.wait_until(lock, deadline, [&] {
          return !active.load(std::memory_order_acquire);
        })) return;
  }

  void StopOutput() {
    std::string error;
    if (!backend->stop(error)) {
      SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Preview output stop failed: %s", error.c_str());
      // Destruction quiesces callbacks even if explicit stop could not confirm
      // it. Stop() joins this worker before touching backend ownership.
      backend.reset();
    }
    deviceStarted.store(false, std::memory_order_release);
    outputLevel.store(0.0f, std::memory_order_relaxed);
  }

  void RunFeeder() noexcept {
    try {
      cursor = firstFrame;
      windowFirst = firstFrame;
      while (active.load(std::memory_order_acquire)) {
        const auto runtime = backend->runtimeState();
        const auto currentRate = runtime.effectiveCallbackSampleRate
            ? runtime.effectiveCallbackSampleRate : runtime.effectiveSampleRate;
        if (currentRate && currentRate != outputRate) {
          Fail("Preview audio output sample rate changed");
          return;
        }
        auto write = written.load(std::memory_order_relaxed);
        const auto read = consumed.load(std::memory_order_acquire);
        const auto free = capacity - static_cast<std::size_t>(write - read);
        if (!free) {
          // Three seconds are already queued. Sleep until roughly one useful
          // batch has drained instead of waking every 5 ms to top it up.
          const auto refillWait = std::chrono::duration<double>(
              std::min(0.1, static_cast<double>(kOutputBatchFrames) / outputRate));
          std::unique_lock lock(waitMutex);
          wake.wait_for(lock, refillWait, [&] {
            return !active.load(std::memory_order_acquire);
          });
          continue;
        }
        const auto count = std::min(free, kOutputBatchFrames);
        std::size_t produced = 0;
        SampleStatus status = SampleStatus::Ready;
        while (produced < count && active.load(std::memory_order_acquire)) {
          // Publish ready output before a source read can wait for more data.
          if (produced && !WindowHasSample(fraction != 0)) break;
          status = FillWindow(fraction != 0);
          if (status != SampleStatus::Ready) break;
          const auto target = static_cast<std::size_t>((write + produced) % capacity) * 2;
          ring[target] = Sample(0);
          ring[target + 1] = Sample(1);
          ++produced;
          Advance();
        }
        written.store(write + produced, std::memory_order_release);
        if (!active.load(std::memory_order_acquire)) return;
        const bool final = status == SampleStatus::End;
        if (!StartOutputIfReady(final)) return;
        if (final) {
          source->SetRenderingEnabled(false);
          sourceEnded.store(true, std::memory_order_release);
          FinishOutput();
          return;
        }
      }
    } catch (const std::exception &exception) {
      Fail(exception.what());
    } catch (...) {
      Fail("Unexpected preview playback failure");
    }
  }

  void Run() noexcept {
    RunFeeder();
    // Includes source cancellation, device/rate errors and exceptions after
    // startup. EOF first waits for presentation; explicit Stop interrupts it.
    if (deviceStarted.load(std::memory_order_acquire)) StopOutput();
  }
};
} // namespace

struct PreviewStreamPlayer::Impl {
  explicit Impl(std::unique_ptr<IBackendFactory> value, StreamRequest requested)
      : factory(std::move(value)), request(std::move(requested)) {}
  std::unique_ptr<IBackendFactory> factory;
  StreamRequest request;
  float gain = 1.0f;
  mutable std::mutex mutex;
  std::atomic<float> outputLevel{0.0f};
  PreviewSpectrumAnalyzer spectrum;
  std::unique_ptr<PreviewSession> session;
};

PreviewStreamPlayer::PreviewStreamPlayer()
    : PreviewStreamPlayer(CreatePlatformBackendFactory()) {}
PreviewStreamPlayer::PreviewStreamPlayer(std::unique_ptr<IBackendFactory> factory,
                                         StreamRequest request)
    : impl(std::make_unique<Impl>(std::move(factory), std::move(request))) {}
PreviewStreamPlayer::~PreviewStreamPlayer() { Stop(); }

bool PreviewStreamPlayer::Start(std::shared_ptr<music_stream::Source> source,
                                std::uint64_t firstFrame, std::string &error) {
  Stop();
  error.clear();
  std::lock_guard lock(impl->mutex);
  if (!source || !impl->factory) {
    error = "Preview audio source or backend is unavailable";
    return false;
  }
  auto session = std::make_unique<PreviewSession>(impl->outputLevel, impl->spectrum);
  session->source = std::move(source);
  session->firstFrame = firstFrame;
  session->gain.store(impl->gain, std::memory_order_relaxed);
  session->backend = impl->factory->open(impl->request, PreviewSession::Render,
                                       session.get(), error);
  if (!session->backend) {
    if (error.empty()) error = "Could not open preview audio device";
    return false;
  }
  session->backend->setRenderTimingCallback(PreviewSession::Timing, session.get());
  const auto runtime = session->backend->runtimeState();
  session->outputRate = runtime.effectiveCallbackSampleRate
      ? runtime.effectiveCallbackSampleRate : runtime.effectiveSampleRate;
  if (!session->outputRate || session->outputRate > 768000) {
    error = "Preview audio device returned an invalid sample rate";
    return false;
  }
  session->startupFrames = (static_cast<std::uint64_t>(session->outputRate) *
      music_stream::kInitialBufferFrames + music_stream::kSampleRate - 1) /
      music_stream::kSampleRate;
  // Retain another second beyond the startup reserve so the feeder can absorb
  // decoder/scheduling bursts without immediately starving the callback.
  session->capacity = std::max<std::size_t>(
      session->startupFrames + session->outputRate, kOutputBatchFrames);
  session->ring.resize(session->capacity * music_stream::kChannels);
  session->source->SetRenderingEnabled(true);
  try {
    session->feeder = std::thread([state = session.get()] { state->Run(); });
  } catch (const std::exception &exception) {
    error = exception.what();
    return false;
  }
  impl->session = std::move(session);
  return true;
}

void PreviewStreamPlayer::Stop() {
  std::lock_guard lock(impl->mutex);
  // The feeder and callback never acquire the player ownership mutex.
  impl->session.reset();
}

void PreviewStreamPlayer::SetRequest(StreamRequest request) {
  std::lock_guard lock(impl->mutex);
  impl->request = std::move(request);
}

void PreviewStreamPlayer::SetGain(float gain) {
  std::lock_guard lock(impl->mutex);
  impl->gain = std::isfinite(gain) ? std::clamp(gain, 0.0f, 1.0f) : 1.0f;
  if (impl->session) impl->session->gain.store(impl->gain, std::memory_order_relaxed);
}

float PreviewStreamPlayer::GetOutputLevel() const noexcept {
  return impl->outputLevel.load(std::memory_order_relaxed);
}

Spectrum PreviewStreamPlayer::GetOutputSpectrum() {
  return impl->spectrum.read();
}

PreviewStreamPlayer::State PreviewStreamPlayer::GetState() const {
  std::lock_guard lock(impl->mutex);
  State state;
  if (!impl->session) return state;
  const auto &session = *impl->session;
  // Acquire EOF before the counters so a completed source cannot expose an
  // older write boundary and incorrectly report a still-queued tail as finished.
  const bool ended = session.sourceEnded.load(std::memory_order_acquire);
  const auto write = session.written.load(std::memory_order_acquire);
  const auto read = session.consumed.load(std::memory_order_acquire);
  const auto elapsed = (read / session.outputRate) * music_stream::kSampleRate +
                       (read % session.outputRate) * music_stream::kSampleRate / session.outputRate;
  state.positionFrames = std::min(session.firstFrame + elapsed,
                                  session.endFrame.load(std::memory_order_relaxed));
  {
    std::lock_guard errorLock(session.errorMutex);
    state.error = session.error;
  }
  state.finished = ended && read == write && state.error.empty();
  state.playing = session.active.load(std::memory_order_acquire) && !state.finished;
  state.buffering = state.playing && (!session.deviceStarted.load(std::memory_order_acquire) || read == write);
  return state;
}
} // namespace audio
