#include "PreviewStreamPlayer.h"
#include "PagedPreviewSource.h"
#include "PreviewSpectrumAnalyzer.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace audio {
namespace {
constexpr std::size_t kOutputBatchFrames = 4096;

static_assert(std::atomic<float>::is_always_lock_free);

struct PreviewSession {
  explicit PreviewSession(std::atomic<float> &level, PreviewSpectrumAnalyzer &analyzer)
      : outputLevel(level), spectrum(analyzer) {}
  std::atomic<float> &outputLevel;
  PreviewSpectrumAnalyzer &spectrum;
  std::shared_ptr<PagedPreviewSource> source;
  std::unique_ptr<IBackend> backend;
  std::uint32_t outputRate = 0;
  // Allocated with the session before Begin or any backend callback.
  std::array<float, kOutputBatchFrames * 2> scratch{};
  std::atomic_bool active{true};
  std::atomic_bool sourceEnded{false};
  std::atomic_bool callbackPending{true};
  std::atomic_bool deviceStarted{false};
  RenderTiming pendingTiming; // Backend callback thread only.
  std::atomic<std::int64_t> lastOutputEndMicros{0};
  std::atomic<std::uint32_t> lastOutputBufferFrames{0};
  std::atomic<float> gain{1.0f};
  std::mutex waitMutex;
  std::condition_variable wake;
  mutable std::mutex errorMutex;
  std::string error;
  std::thread control;

  ~PreviewSession() { Stop(); }

  void Stop() {
    {
      std::lock_guard lock(waitMutex);
      active.store(false, std::memory_order_release);
    }
    wake.notify_all();
    // Cancel wakes the control wait and joins preparation. It retains every
    // published page/owner until source destruction, after callback quiescence.
    // The control worker never calls Cancel, so there is only one join owner.
    if (source) source->Cancel();
    if (control.joinable()) control.join();
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
  }

  static void Timing(RenderTiming timing, void *context) {
    static_cast<PreviewSession *>(context)->pendingTiming = timing;
  }

  static short Pcm(float sample) noexcept {
    // Match gameplay's final conversion; preserve float mixer headroom until
    // after playback gain, then clamp once and truncate toward zero.
    return static_cast<short>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f);
  }

  static void Render(void *output, std::uint32_t frameCount, int channels,
                     void *context) {
    auto &session = *static_cast<PreviewSession *>(context);
    const auto timing = session.pendingTiming;
    session.pendingTiming = {};
    if (!output || channels <= 0) return;
    auto *pcm = static_cast<short *>(output);
    std::fill_n(pcm, static_cast<std::size_t>(frameCount) * channels, short{0});
    const auto gain = session.gain.load(std::memory_order_relaxed);
    double energy = 0.0;
    std::uint32_t offset = 0, lastDataEnd = 0;
    bool pending = false, ended = false;
    while (offset < frameCount && session.active.load(std::memory_order_acquire) &&
           !session.sourceEnded.load(std::memory_order_relaxed)) {
      const auto count = std::min<std::uint32_t>(frameCount - offset, kOutputBatchFrames);
      const auto rendered = session.source->Render(
          std::span<float>(session.scratch).first(static_cast<std::size_t>(count) * 2), count);
      if (rendered.status == PagedPreviewRenderStatus::End) { ended = true; break; }
      if (rendered.status == PagedPreviewRenderStatus::Pending || !rendered.frames) {
        pending = true;
        break;
      }
      const auto frames = std::min(count, rendered.frames);
      for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto target = static_cast<std::size_t>(offset + frame) * channels;
        const float left = session.scratch[frame * 2] * gain;
        const float right = session.scratch[frame * 2 + 1] * gain;
        if (channels == 1) {
          pcm[target] = Pcm((left + right) * 0.5f);
        } else {
          pcm[target] = Pcm(left);
          pcm[target + 1] = Pcm(right);
        }
        const double leftOutput = pcm[target] / 32768.0;
        const double rightOutput = channels == 1 ? leftOutput : pcm[target + 1] / 32768.0;
        energy += (leftOutput * leftOutput + rightOutput * rightOutput) * 0.5;
      }
      offset += frames;
      lastDataEnd = offset;
    }
    session.outputLevel.store(frameCount ? static_cast<float>(std::sqrt(energy / frameCount)) : 0.0f,
                              std::memory_order_relaxed);
    session.spectrum.push(pcm, frameCount, channels, session.outputRate);
    if (lastDataEnd) {
      const auto durationMicros = static_cast<std::int64_t>(
          (static_cast<std::uint64_t>(lastDataEnd) * 1000000ULL + session.outputRate - 1) /
          session.outputRate);
      session.lastOutputEndMicros.store(
          timing.outputTimestampKnown && timing.outputSteadyMicros > 0
              ? timing.outputSteadyMicros + durationMicros : 0,
          std::memory_order_relaxed);
      session.lastOutputBufferFrames.store(frameCount, std::memory_order_relaxed);
    }
    session.callbackPending.store(pending, std::memory_order_relaxed);
    // Publish EOF after the presentation boundary. Silence while pending never
    // advances the paged source's clock. No callback locks or notifications.
    if (ended) session.sourceEnded.store(true, std::memory_order_release);
  }

  void FinishOutput() {
    if (!deviceStarted.load(std::memory_order_acquire)) return;
    if (!active.load(std::memory_order_acquire)) return;
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
    while (active.load(std::memory_order_acquire)) {
      // Playback can end while referenced assets are still warming. A later
      // preparation failure must stop output even during a long native tail;
      // retain warming ownership and leave Cancel to explicit Stop.
      const auto state = source->GetState();
      if (!state.error.empty()) { Fail(state.error); return; }
      if (state.cancelled) { active.store(false, std::memory_order_release); return; }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return;
      const auto nextCheck = std::min(deadline, now + std::chrono::seconds(1));
      std::unique_lock lock(waitMutex);
      if (wake.wait_until(lock, nextCheck, [&] {
            return !active.load(std::memory_order_acquire);
          })) return;
    }
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

  void RunControl() noexcept {
    try {
      while (active.load(std::memory_order_acquire)) {
        const auto state = source->GetState();
        if (!state.error.empty()) { Fail(state.error); return; }
        if (state.cancelled) { active.store(false, std::memory_order_release); return; }
        const auto runtime = backend->runtimeState();
        const auto currentRate = runtime.effectiveCallbackSampleRate
            ? runtime.effectiveCallbackSampleRate : runtime.effectiveSampleRate;
        if (currentRate && currentRate != outputRate) {
          Fail("Preview audio output sample rate changed");
          return;
        }
        if (state.startupReady && !deviceStarted.load(std::memory_order_relaxed)) {
          if (!active.load(std::memory_order_acquire)) return;
          std::string diagnostic;
          if (!backend->start(diagnostic)) {
            Fail(diagnostic.empty() ? "Could not start preview audio device" : diagnostic);
            return;
          }
          deviceStarted.store(true, std::memory_order_release);
        }
        if (sourceEnded.load(std::memory_order_acquire)) {
          FinishOutput();
          return;
        }
        // Revision changes wake startup/errors immediately. Callback EOF has no
        // notification; bound its observation without a permanently true wait
        // predicate or continuous polling once preparation is complete.
        source->WaitForChange(state.revision, std::chrono::seconds(1));
      }
    } catch (const std::exception &exception) {
      Fail(exception.what());
    } catch (...) {
      Fail("Unexpected preview playback failure");
    }
  }

  void Run() noexcept {
    RunControl();
    // Includes cancellation, route/source errors and exceptions after startup.
    // EOF first waits for presentation; explicit Stop interrupts that wait.
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

bool PreviewStreamPlayer::Start(std::shared_ptr<PagedPreviewSource> source,
                                std::string &error) {
  Stop();
  error.clear();
  std::lock_guard lock(impl->mutex);
  if (!source || !impl->factory) {
    error = "Preview audio source or backend is unavailable";
    return false;
  }
  auto session = std::make_unique<PreviewSession>(impl->outputLevel, impl->spectrum);
  session->source = std::move(source);
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
  if (!session->source->Begin(static_cast<int>(session->outputRate))) {
    error = session->source->GetState().error;
    if (error.empty()) error = "Could not prepare preview audio";
    return false;
  }
  try {
    session->control = std::thread([state = session.get()] { state->Run(); });
  } catch (const std::exception &exception) {
    error = exception.what();
    return false;
  }
  impl->session = std::move(session);
  return true;
}

void PreviewStreamPlayer::Stop() {
  std::lock_guard lock(impl->mutex);
  // The control worker and callback never acquire the player ownership mutex.
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
  const bool ended = session.sourceEnded.load(std::memory_order_acquire);
  const auto sourceState = session.source->GetState();
  state.positionFrames = sourceState.positionFrames44100;
  {
    std::lock_guard errorLock(session.errorMutex);
    state.error = session.error;
  }
  if (state.error.empty()) state.error = sourceState.error;
  state.finished = ended && state.error.empty() && !sourceState.cancelled;
  state.playing = session.active.load(std::memory_order_acquire) && !state.finished &&
                  state.error.empty() && !sourceState.cancelled;
  state.buffering = state.playing && (!session.deviceStarted.load(std::memory_order_acquire) ||
                                    session.callbackPending.load(std::memory_order_relaxed));
  return state;
}
} // namespace audio
