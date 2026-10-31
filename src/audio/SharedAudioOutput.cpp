#include "SharedAudioOutput.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace audio {
namespace {
constexpr std::size_t kScratchFrames = 4096;

struct SubmixNode {
  // Admission and the in-flight reader share one atomic, so a stopped node
  // cannot acquire a late reader after the control thread observes quiescence.
  static constexpr unsigned kEnabled = 2;
  static constexpr unsigned kRendering = 1;
  std::atomic<unsigned> gate{0};
  bool occupied = false; // Graph control mutex only.
  RenderCallback render = nullptr;
  void *renderData = nullptr;
  RenderTimingCallback timing = nullptr;
  void *timingData = nullptr;

  bool enter() noexcept {
    unsigned expected = kEnabled;
    return gate.compare_exchange_strong(expected, kEnabled | kRendering,
                                         std::memory_order_acquire);
  }
  void leave() noexcept { gate.fetch_and(~kRendering, std::memory_order_release); }
  void stop() noexcept {
    gate.fetch_and(~kEnabled, std::memory_order_acq_rel);
    while (gate.load(std::memory_order_acquire) & kRendering)
      std::this_thread::yield();
  }
  void start() noexcept { gate.fetch_or(kEnabled, std::memory_order_release); }
  bool enabled() const noexcept {
    return (gate.load(std::memory_order_acquire) & kEnabled) != 0;
  }
};
static_assert(std::atomic<unsigned>::is_always_lock_free);
}

struct SharedAudioOutput::State {
  explicit State(std::unique_ptr<IBackendFactory> source) : factory(std::move(source)) {}
  ~State() {
    // Native backend destruction guarantees callback quiescence even when
    // stop reports failure. Keep nodes and scratch alive until then.
    if (backend) {
      std::string ignored;
      backend->stop(ignored);
      backend.reset();
    }
  }

  mutable std::mutex mutex;
  std::unique_ptr<IBackendFactory> factory;
  std::unique_ptr<IBackend> backend;
  RuntimeState lastState;
  bool deferStart = false;
  std::atomic<std::uint32_t> sampleRate{44100};
  std::array<SubmixNode, kSubmixCount> nodes;
  std::array<float, kScratchFrames * 2> mix{};
  RenderTiming pendingTiming; // Native callback thread only.

  static void timing(RenderTiming value, void *context) {
    static_cast<State *>(context)->pendingTiming = value;
  }

  static void render(void *output, std::uint32_t frames, int channels, void *context) {
    auto &state = *static_cast<State *>(context);
    auto timing = state.pendingTiming;
    state.pendingTiming = {};
    if (!output || !frames || channels <= 0) return;
    auto *pcm = static_cast<short *>(output);
    if (channels != 2) {
      std::fill_n(pcm, static_cast<std::size_t>(frames) * channels, short{0});
      return;
    }
    // One wall-clock origin for the whole untimestamped native buffer. Small
    // callbacks keep AudioWrapper's existing clock read, with no extra syscall.
    if (frames > kScratchFrames &&
        (!timing.outputTimestampKnown || timing.outputSteadyMicros <= 0) &&
        timing.fallbackSteadyMicros <= 0) {
      timing.fallbackSteadyMicros = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::array<bool, kSubmixCount> entered{};
    for (std::size_t index = 0; index < entered.size(); ++index)
      entered[index] = state.nodes[index].enter();
    const auto rate = state.sampleRate.load(std::memory_order_relaxed);
    for (std::uint32_t offset = 0; offset < frames;) {
      const auto count = std::min<std::uint32_t>(frames - offset, kScratchFrames);
      const auto samples = static_cast<std::size_t>(count) * 2;
      if (!entered[0]) std::fill_n(state.mix.data(), samples, 0.0F);
      auto chunkTiming = timing;
      if (rate && (chunkTiming.outputTimestampKnown || chunkTiming.fallbackSteadyMicros > 0)) {
        const auto delta = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(offset) * 1'000'000 / rate);
        const auto advance = [delta](std::int64_t origin) {
          return origin > std::numeric_limits<std::int64_t>::max() - delta
              ? std::numeric_limits<std::int64_t>::max() : origin + delta;
        };
        if (chunkTiming.outputTimestampKnown && chunkTiming.outputSteadyMicros > 0)
          chunkTiming.outputSteadyMicros = advance(chunkTiming.outputSteadyMicros);
        if (chunkTiming.fallbackSteadyMicros > 0)
          chunkTiming.fallbackSteadyMicros = advance(chunkTiming.fallbackSteadyMicros);
      }
      for (std::size_t index = 0; index < entered.size(); ++index) {
        if (!entered[index]) continue;
        auto &node = state.nodes[index];
        if (node.timing) node.timing(chunkTiming, node.timingData);
        node.render(state.mix.data(), count, channels, node.renderData);
      }
      for (std::size_t i = 0; i < samples; ++i)
        pcm[static_cast<std::size_t>(offset) * 2 + i] =
            static_cast<short>(std::clamp(state.mix[i], -1.0F, 1.0F) * 32767.0F);
      offset += count;
    }
    for (std::size_t index = 0; index < entered.size(); ++index)
      if (entered[index]) state.nodes[index].leave();
  }

  bool open(const StreamRequest &request, std::string &error) {
    backend = factory->open(request, render, this, error);
    if (!backend) return false;
    backend->setRenderTimingCallback(timing, this);
    lastState = backend->runtimeState();
    sampleRate.store(lastState.effectiveSampleRate ? lastState.effectiveSampleRate : 44100,
                      std::memory_order_relaxed);
    return true;
  }

  bool ensureStarted(std::string &error) {
    // Called with mutex held. During configuration, software nodes may enable
    // only after their PCM commits, but the native device starts last.
    if (deferStart) return true;
    if (!backend) { error = "Audio output is unavailable"; return false; }
    return backend->observeState().state == playback::BackendRunState::Running ||
           backend->start(error);
  }
};

class SharedAudioOutput::Submix final : public IBackend {
public:
  Submix(std::shared_ptr<State> state, std::size_t index)
      : state_(std::move(state)), index_(index) {}
  ~Submix() override {
    auto &node = state_->nodes[index_];
    node.stop();
    std::lock_guard lock(state_->mutex);
    node.render = nullptr;
    node.renderData = nullptr;
    node.timing = nullptr;
    node.timingData = nullptr;
    node.occupied = false;
  }
  void setRenderTimingCallback(RenderTimingCallback callback, void *data) override {
    // Backend setup, before start; no callback can read these fields yet.
    state_->nodes[index_].timing = callback;
    state_->nodes[index_].timingData = data;
  }
  bool start(std::string &error) override {
    std::lock_guard lock(state_->mutex);
    auto &node = state_->nodes[index_];
    node.start();
    if (state_->ensureStarted(error)) return true;
    node.stop();
    return false;
  }
  bool stop(std::string &) override {
    state_->nodes[index_].stop();
    return true;
  }
  bool isStarted() const override {
    return observeState().state == playback::BackendRunState::Running;
  }
  playback::BackendStateObservation observeState() const override {
    // This is the software node's admission state, not native-device liveness.
    // Even a stopped native device can be restarted by the other node.
    return {.state = state_->nodes[index_].enabled()
                         ? playback::BackendRunState::Running
                         : playback::BackendRunState::Stopped};
  }
  RuntimeState runtimeState() const override {
    std::lock_guard lock(state_->mutex);
    return state_->backend ? state_->backend->runtimeState() : state_->lastState;
  }
  std::optional<bool> outputIsBluetooth(bool refresh) const override {
    std::lock_guard lock(state_->mutex);
    return state_->backend ? state_->backend->outputIsBluetooth(refresh) : std::nullopt;
  }
private:
  std::shared_ptr<State> state_;
  std::size_t index_;
};

class SharedAudioOutput::Factory final : public IBackendFactory {
public:
  Factory(std::shared_ptr<State> state, std::size_t index)
      : state_(std::move(state)), index_(index) {}
  RenderFormat renderFormat() const override {
    return index_ == 0 ? RenderFormat::Float32 : RenderFormat::Float32Additive;
  }
  Capabilities capabilities() const override { return {}; }
  std::unique_ptr<IBackend> open(const StreamRequest &request, RenderCallback render,
                                  void *data, std::string &error) override {
    std::lock_guard lock(state_->mutex);
    auto &node = state_->nodes[index_];
    if (node.occupied || !render || request != state_->lastState.request) {
      error = "Submix requires an unused slot and the current output configuration";
      return {};
    }
    auto result = std::make_unique<Submix>(state_, index_);
    node.render = render;
    node.renderData = data;
    node.occupied = true;
    return result;
  }
private:
  std::shared_ptr<State> state_;
  std::size_t index_;
};

SharedAudioOutput::SharedAudioOutput(std::unique_ptr<IBackendFactory> factory)
    : state_(std::make_shared<State>(std::move(factory))) {
  std::string error;
  if (!state_->factory || state_->factory->renderFormat() != RenderFormat::Signed16 ||
      !state_->open({}, error))
    throw std::runtime_error(error.empty() ? "Failed to initialize shared audio output" : error);
  // Submix startup attempts native start. A transient failure keeps the open
  // device available for later playback or the application's saved settings.
}
SharedAudioOutput::~SharedAudioOutput() = default;

std::unique_ptr<IBackendFactory> SharedAudioOutput::createSubmixFactory(std::size_t index) {
  if (index >= kSubmixCount) throw std::out_of_range("Audio submix index");
  return std::make_unique<Factory>(state_, index);
}
Capabilities SharedAudioOutput::capabilities() const {
  std::lock_guard lock(state_->mutex);
  return state_->factory->capabilities();
}
RuntimeState SharedAudioOutput::runtimeState() const {
  std::lock_guard lock(state_->mutex);
  return state_->backend ? state_->backend->runtimeState() : state_->lastState;
}
std::optional<bool> SharedAudioOutput::outputIsBluetooth(bool refresh) const {
  std::lock_guard lock(state_->mutex);
  return state_->backend ? state_->backend->outputIsBluetooth(refresh) : std::nullopt;
}
bool SharedAudioOutput::reopen(const StreamRequest &request, std::string &error) {
  std::lock_guard lock(state_->mutex);
  for (const auto &node : state_->nodes) {
    if (node.gate.load(std::memory_order_acquire) != 0) {
      error = "All submixes must be suspended before changing output";
      return false;
    }
  }
  if (state_->backend && !state_->backend->stop(error)) return false;
  state_->backend.reset();
  state_->pendingTiming = {};
  state_->deferStart = true;
  return state_->open(request, error);
}
bool SharedAudioOutput::start(std::string &error) {
  std::lock_guard lock(state_->mutex);
  state_->deferStart = false;
  return state_->ensureStarted(error);
}
}
