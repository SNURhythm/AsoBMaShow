#include "audio/PreviewStreamPlayer.h"
#include "audio/AudioBackend.h"
#include "audio/PagedPreviewSource.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <new>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace { thread_local bool inCallback = false; }
void *operator new(std::size_t bytes) {
  assert(!inCallback && "preview callback must not allocate");
  if (auto *memory = std::malloc(bytes ? bytes : 1)) return memory;
  throw std::bad_alloc();
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
namespace {
// Replace only the device and paged-source boundaries. The production player's
// float conversion, callback splitting, state, tail drain and lifecycle are tested.
struct SourceControl {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<float> samples;
  std::uint64_t first = 0;
  std::atomic<std::size_t> published{0};
  std::atomic_bool ready{false}, complete{false}, cancelled{false}, failed{false};
  std::atomic<std::uint64_t> revision{0}, rendered{0};
  std::atomic_int beginRate{0}, cancelCalls{0}, waits{0}, renderCalls{0};
  std::atomic<std::uint32_t> maximumRenderFrames{0};
  std::atomic_bool blockRender{false}, renderEntered{false}, sourceDestroyed{false};
  bool failBegin = false;
  std::string error;
  void changedState() { ++revision; changed.notify_all(); }
  void publish(std::size_t frames, bool final = false) {
    published = frames; complete = final; ready = true; changedState();
  }
  void publishAll() { publish(samples.size() / 2, true); }
  void fail(std::string message) {
    { std::lock_guard lock(mutex); error = std::move(message); failed = true; }
    changedState();
  }
};
std::shared_ptr<SourceControl> nextSource;
struct BackendControl {
  std::mutex callbackMutex;
  audio::RenderCallback render = nullptr;
  audio::RenderTimingCallback timing = nullptr;
  void *timingContext = nullptr;
  void *context = nullptr;
  std::atomic<std::uint32_t> rate{44100};
  audio::StreamRequest request;
  std::int64_t outputDelayMicros = 0;
  bool timingKnown = false;
  bool failStart = false;
  bool failStop = false;
  std::atomic_bool started{false};
  std::atomic_bool stopOnCallbackThread{false};
  std::atomic<std::int64_t> outputTimestampMicros{0};
  std::atomic<std::int64_t> stoppedAtMicros{0};
  std::thread::id callbackThread = std::this_thread::get_id();
  std::atomic_int stops{0};
  std::atomic_int runtimeQueries{0};
  std::vector<short> pump(std::size_t frames, int channels = 2) {
    std::lock_guard lock(callbackMutex);
    assert(render);
    std::vector<short> output(frames * channels, 12345);
    if (timing) {
      const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      outputTimestampMicros = now + outputDelayMicros;
      timing({.outputSteadyMicros = now + outputDelayMicros,
              .outputTimestampKnown = timingKnown}, timingContext);
    }
    inCallback = true;
    render(output.data(), static_cast<std::uint32_t>(frames), channels, context);
    inCallback = false;
    return output;
  }
};
class FakeBackend final : public audio::IBackend {
public:
  explicit FakeBackend(std::shared_ptr<BackendControl> control) : control(std::move(control)) {}
  ~FakeBackend() override {
    std::lock_guard lock(control->callbackMutex);
    control->started = false;
  }
  void setRenderTimingCallback(audio::RenderTimingCallback callback, void *context) override {
    control->timing = callback;
    control->timingContext = context;
  }
  bool start(std::string &error) override {
    if (control->failStart) { error = "injected device start failure"; return false; }
    control->started = true;
    return true;
  }
  bool stop(std::string &error) override {
    std::lock_guard lock(control->callbackMutex);
    control->stoppedAtMicros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    control->stopOnCallbackThread = inCallback;
    ++control->stops;
    if (control->failStop) { error = "injected stop failure"; return false; }
    control->started = false;
    return true;
  }
  bool isStarted() const override { return control->started; }
  audio::RuntimeState runtimeState() const override {
    assert(!inCallback);
    ++control->runtimeQueries;
    return {.effectiveSampleRate = control->rate.load()};
  }
private:
  std::shared_ptr<BackendControl> control;
};
class FakeFactory final : public audio::IBackendFactory {
public:
  explicit FakeFactory(std::shared_ptr<BackendControl> control) : control(std::move(control)) {}
  audio::Capabilities capabilities() const override { return {}; }
  std::unique_ptr<audio::IBackend> open(const audio::StreamRequest &request,
      audio::RenderCallback callback, void *context, std::string &) override {
    control->request = request;
    control->render = callback;
    control->context = context;
    return std::make_unique<FakeBackend>(control);
  }
private:
  std::shared_ptr<BackendControl> control;
};

void waitFor(const auto &predicate) {
  const auto limit = std::chrono::steady_clock::now() + 3s;
  while (!predicate() && std::chrono::steady_clock::now() < limit)
    std::this_thread::sleep_for(1ms);
  assert(predicate());
}
std::shared_ptr<audio::PagedPreviewSource> source(std::shared_ptr<SourceControl> control) {
  nextSource = std::move(control);
  return audio::PagedPreviewSource::Create(bms_parser::Chart{}, {}, nextSource->first);
}
void start(audio::PreviewStreamPlayer &player, const std::shared_ptr<SourceControl> &input,
           const std::shared_ptr<BackendControl> &backend) {
  std::string error;
  assert(player.Start(source(input), error));
  waitFor([&] { return backend->started.load(); });
}
bool all(const std::vector<short> &output, short value) {
  return std::all_of(output.begin(), output.end(), [=](short sample) { return sample == value; });
}

void testStartupUsesActualBackendRateAndDoesNotMixBeforeCallback() {
  for (const auto rate : {44100U, 48000U, 88200U, 22050U}) {
    auto input = std::make_shared<SourceControl>();
    input->first = 441000;
    input->samples.assign(rate * 3 * 2, 0.25f);
    auto backend = std::make_shared<BackendControl>();
    backend->rate = rate;
    audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
    player.SetRequest({.deviceId = "selected-device", .sampleRate = 96000, .bufferFrames = 512});
    std::string error;
    assert(player.Start(source(input), error));
    waitFor([&] { return input->beginRate.load() != 0 && input->waits.load() != 0; });
    assert(input->beginRate == rate && backend->request.deviceId == "selected-device");
    assert(backend->request.sampleRate == 96000 && backend->request.bufferFrames == 512);
    assert(!backend->started && input->renderCalls == 0);
    assert(player.GetState().buffering && player.GetState().positionFrames == input->first);
    input->publish(rate * 2);
    waitFor([&] { return backend->started.load(); });
    assert(input->renderCalls == 0 && "control worker must never mix or refill audio");
    assert(all(backend->pump(rate), 8191));
    assert(player.GetState().positionFrames == input->first + 44100);
    assert(input->maximumRenderFrames <= 4096 && "large native blocks must split into preallocated chunks");
    const auto before = backend->runtimeQueries.load();
    std::this_thread::sleep_for(200ms);
    assert(backend->runtimeQueries.load() - before <= 2 && "ready source must not busy-poll");
    const auto begin = std::chrono::steady_clock::now();
    player.Stop();
    assert(std::chrono::steady_clock::now() - begin < 500ms);
    assert(input->cancelCalls == 1 && !backend->started);
  }
}

void testFloatGainPrecedesClippingAndOutputEnergy() {
  auto input = std::make_shared<SourceControl>();
  input->samples.assign(4096 * 2, 1.5f);
  input->publishAll();
  auto backend = std::make_shared<BackendControl>();
  audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
  start(player, input, backend);
  player.SetGain(0.5f);
  assert(all(backend->pump(128), 24575));
  assert(std::abs(player.GetOutputLevel() - 0.75f) < 0.001f);
  player.SetGain(1);
  assert(all(backend->pump(128), 32767));
  player.SetGain(0);
  assert(all(backend->pump(128), 0) && player.GetOutputLevel() == 0);
  player.Stop();
  assert(player.GetOutputLevel() == 0);

  auto stereo = std::make_shared<SourceControl>();
  stereo->samples.resize(4096 * 2);
  for (std::size_t i = 0; i < stereo->samples.size(); i += 2) {
    stereo->samples[i] = 0.5f; stereo->samples[i + 1] = -0.5f;
  }
  stereo->publishAll();
  player.SetGain(1); // Gain is a player setting and persists across sessions.
  start(player, stereo, backend);
  backend->pump(128);
  assert(std::abs(player.GetOutputLevel() - 0.5f) < 0.001f);
  assert(all(backend->pump(128, 1), 0) && player.GetOutputLevel() == 0);
  const auto surround = backend->pump(128, 6);
  for (std::size_t frame = 0; frame < 128; ++frame) {
    assert(surround[frame * 6] == 16383 && surround[frame * 6 + 1] == -16383);
    for (int channel = 2; channel < 6; ++channel) assert(surround[frame * 6 + channel] == 0);
  }
}

void testSpectrumFollowsAudibleOutputAndResets() {
  constexpr std::size_t block = 4096;
  for (const auto rate : {44100U, 48000U}) {
    auto input = std::make_shared<SourceControl>();
    input->samples.resize(block * 4 * 2);
    for (std::size_t frame = 0; frame < block * 4; ++frame) {
      const double frequency = frame < block ? 100.0 : 4000.0;
      const float sample = 0.45f * std::sin(6.283185307179586 * frequency * frame / rate);
      input->samples[frame * 2] = sample; input->samples[frame * 2 + 1] = -sample;
    }
    input->publishAll();
    auto backend = std::make_shared<BackendControl>(); backend->rate = rate;
    audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
    start(player, input, backend);
    backend->pump(block);
    const auto bass = player.GetOutputSpectrum();
    const auto bassPeak = std::max_element(bass.begin(), bass.end());
    assert(*bassPeak > 0.7f);
    backend->pump(block);
    const auto treble = player.GetOutputSpectrum();
    const auto treblePeak = std::max_element(treble.begin(), treble.end());
    assert(*treblePeak > 0.7f);
    assert(treblePeak - treble.begin() > bassPeak - bass.begin() + audio::kSpectrumBandCount / 4);
    assert(treble[bassPeak - bass.begin()] < 0.35f && player.GetOutputSpectrum() == treble);
    player.SetGain(0); backend->pump(block);
    const auto muted = player.GetOutputSpectrum();
    assert(*std::max_element(muted.begin(), muted.end()) == 0);
    player.Stop();
    const auto stopped = player.GetOutputSpectrum();
    assert(*std::max_element(stopped.begin(), stopped.end()) == 0);
  }
}

void testPendingFreezesSourceClockAndResumesDirectly() {
  auto input = std::make_shared<SourceControl>();
  input->first = 441000; input->samples.assign(4096 * 4, 0.25f); input->publish(4096);
  auto backend = std::make_shared<BackendControl>();
  audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
  start(player, input, backend);
  assert(all(backend->pump(4096), 8191));
  assert(all(backend->pump(128), 0));
  assert(player.GetState().buffering && player.GetState().positionFrames == input->first + 4096);
  input->publishAll();
  assert(all(backend->pump(4096), 8191));
  assert(!player.GetState().buffering && player.GetState().positionFrames == input->first + 8192);
  assert(all(backend->pump(128), 0));
  assert(player.GetState().finished && player.GetState().positionFrames == input->first + 8192);
}

void testNaturalEofPreservesPreparationAndPresentationTail() {
  for (const bool knownTiming : {false, true}) {
    auto input = std::make_shared<SourceControl>();
    input->samples.assign(1024 * 2, 0.25f); input->publishAll();
    auto backend = std::make_shared<BackendControl>();
    backend->timingKnown = knownTiming; backend->outputDelayMicros = 1250000;
    audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
    start(player, input, backend);
    assert(backend->timing);
    const auto output = backend->pump(2048); // Includes partial final PCM and EOF.
    assert(std::all_of(output.begin(), output.begin() + 2048, [](short s) { return s == 8191; }));
    assert(std::all_of(output.begin() + 2048, output.end(), [](short s) { return s == 0; }));
    waitFor([&] { return player.GetState().finished; });
    std::this_thread::sleep_for(100ms);
    assert(backend->started && "final PCM must remain queued until its presentation tail drains");
    waitFor([&] { return !backend->started.load(); });
    if (knownTiming) {
      const auto duration = (1024LL * 1000000 + backend->rate - 1) / backend->rate;
      assert(backend->stoppedAtMicros >= backend->outputTimestampMicros + duration);
    }
    assert(!backend->stopOnCallbackThread && input->cancelCalls == 0);
    assert(player.GetState().finished && player.GetState().positionFrames == 1024);
    player.Stop();
    assert(input->cancelCalls == 1);
  }
}

void testStopInterruptsStartupAndPresentationWaitsAndReplacement() {
  for (const int phase : {0, 1, 2}) {
    auto input = std::make_shared<SourceControl>(); input->samples.assign(1024 * 2, 0.25f);
    if (phase) input->publishAll();
    auto backend = std::make_shared<BackendControl>();
    backend->timingKnown = true; backend->outputDelayMicros = 5000000;
    audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
    std::string error; assert(player.Start(source(input), error));
    if (phase) waitFor([&] { return backend->started.load(); });
    else waitFor([&] { return input->waits.load() > 0; });
    if (phase == 2) { backend->pump(2048); std::this_thread::sleep_for(1100ms); }
    const auto begin = std::chrono::steady_clock::now(); player.Stop();
    assert(std::chrono::steady_clock::now() - begin < 500ms);
    assert(input->cancelCalls == 1 && !backend->started);
    auto replacement = std::make_shared<SourceControl>();
    replacement->first = 900; replacement->samples.assign(4, 0.25f); replacement->publishAll();
    start(player, replacement, backend);
    assert(all(backend->pump(2), 8191));
    backend->pump(1);
    assert(player.GetState().finished && player.GetState().positionFrames == 902);
  }
}

void testFailuresAndCancellationStopOutputOffCallback() {
  for (const int failure : {0, 1, 2, 3}) {
    auto input = std::make_shared<SourceControl>(); input->samples.assign(44100 * 6, 0.25f);
    input->publishAll();
    auto backend = std::make_shared<BackendControl>();
    backend->failStart = failure == 0;
    audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
    std::string error; assert(player.Start(source(input), error));
    if (failure) {
      waitFor([&] { return backend->started.load(); });
      if (failure == 1) input->fail("injected failure during playback");
      if (failure == 2) { input->cancelled = true; input->changedState(); }
      if (failure == 3) backend->rate = 48000;
    }
    waitFor([&] { return !player.GetState().playing; });
    waitFor([&] { return !backend->started.load(); });
    assert(!player.GetState().finished && !backend->stopOnCallbackThread);
    if (failure != 2) assert(!player.GetState().error.empty());
    assert(input->cancelCalls == 0 && "control worker must not race Stop's source cancellation");
    player.Stop(); assert(input->cancelCalls == 1);
  }
  auto input = std::make_shared<SourceControl>(); input->failBegin = true;
  auto backend = std::make_shared<BackendControl>();
  audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
  std::string error; assert(!player.Start(source(input), error) && !error.empty());
  assert(input->cancelCalls == 1 && backend->stops > 0);
}

void testStopRetainsSourceUntilInflightCallbackQuiesces() {
  auto input = std::make_shared<SourceControl>();
  input->samples.assign(4096 * 2, 0.25f); input->publishAll();
  auto backend = std::make_shared<BackendControl>();
  audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
  auto paged = source(input);
  std::weak_ptr<audio::PagedPreviewSource> owner = paged;
  std::string error; assert(player.Start(std::move(paged), error));
  waitFor([&] { return backend->started.load(); });
  input->blockRender = true;
  std::thread callback([&] { backend->pump(128); });
  waitFor([&] { return input->renderEntered.load(); });
  std::atomic_bool stopped = false;
  std::thread stopper([&] { player.Stop(); stopped = true; });
  waitFor([&] { return input->cancelled.load(); });
  assert(!stopped && !owner.expired() && !input->sourceDestroyed &&
         "Cancel must retain callback owners until backend output quiesces");
  input->blockRender = false;
  callback.join();
  stopper.join();
  assert(stopped && owner.expired() && input->sourceDestroyed && !backend->started);
  assert(input->cancelCalls == 1 && !backend->stopOnCallbackThread);
}

void testFailedAutomaticStopQuiescesBackend() {
  auto input = std::make_shared<SourceControl>();
  input->samples.assign(1024 * 2, 0.25f); input->publishAll();
  auto backend = std::make_shared<BackendControl>(); backend->timingKnown = true; backend->failStop = true;
  audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
  start(player, input, backend); backend->pump(2048);
  waitFor([&] { return !backend->started.load(); });
  assert(backend->stops == 1 && !backend->stopOnCallbackThread);
  assert(player.GetState().finished && input->cancelCalls == 0);
}
} // namespace

namespace audio {
struct PagedPreviewSource::Impl { std::shared_ptr<SourceControl> control; };
PagedPreviewSource::PagedPreviewSource(std::unique_ptr<Impl> value) : impl(std::move(value)) {}
PagedPreviewSource::~PagedPreviewSource() { impl->control->sourceDestroyed = true; }
std::shared_ptr<PagedPreviewSource> PagedPreviewSource::Create(const bms_parser::Chart &,
    std::shared_ptr<DecodedAssetCache>, std::uint64_t, PagedPreviewGains) {
  auto value = std::make_unique<Impl>(); value->control = nextSource;
  return std::shared_ptr<PagedPreviewSource>(new PagedPreviewSource(std::move(value)));
}
bool PagedPreviewSource::Begin(int rate) {
  assert(!inCallback); impl->control->beginRate = rate;
  if (impl->control->failBegin) { impl->control->fail("injected Begin failure"); return false; }
  return true;
}
PagedPreviewRenderResult PagedPreviewSource::Render(std::span<float> output,
                                                    std::uint32_t count) noexcept {
  assert(inCallback);
  auto &c = *impl->control; ++c.renderCalls;
  c.maximumRenderFrames = std::max(c.maximumRenderFrames.load(), count);
  std::fill(output.begin(), output.end(), 0.0f);
  if (c.cancelled || c.failed || !c.ready) return {};
  if (c.blockRender) {
    c.renderEntered = true;
    while (c.blockRender) std::this_thread::yield();
    assert(!c.sourceDestroyed);
  }
  const auto offset = c.rendered.load(); const auto available = c.published.load();
  if (offset >= available) return {c.complete ? PagedPreviewRenderStatus::End : PagedPreviewRenderStatus::Pending, 0};
  if (offset + count > available && !c.complete) return {};
  const auto frames = static_cast<std::uint32_t>(std::min<std::size_t>(count, available - offset));
  std::copy_n(c.samples.begin() + offset * 2, frames * 2, output.begin());
  c.rendered = offset + frames;
  return {PagedPreviewRenderStatus::Data, frames};
}
PagedPreviewState PagedPreviewSource::GetState() const {
  assert(!inCallback); auto &c = *impl->control; std::lock_guard lock(c.mutex);
  const auto elapsed = c.beginRate ? c.rendered * kPreviewSampleRate / c.beginRate : 0;
  return {.startupReady = c.ready, .preparationComplete = c.complete, .durationKnown = c.complete,
          .cancelled = c.cancelled, .firstFrame44100 = c.first,
          .durationFrames44100 = c.first + (c.beginRate ? c.samples.size() / 2 * kPreviewSampleRate / c.beginRate : 0),
          .positionFrames44100 = c.first + elapsed, .revision = c.revision, .error = c.error};
}
void PagedPreviewSource::WaitForChange(std::uint64_t revision, std::chrono::milliseconds timeout) {
  assert(!inCallback); auto &c = *impl->control; ++c.waits;
  std::unique_lock lock(c.mutex);
  c.changed.wait_for(lock, timeout, [&] { return c.revision != revision; });
}
void PagedPreviewSource::Cancel() {
  assert(!inCallback); ++impl->control->cancelCalls;
  impl->control->cancelled = true; impl->control->changedState();
}
std::unique_ptr<IBackendFactory> CreatePlatformBackendFactory() {
  return std::make_unique<FakeFactory>(std::make_shared<BackendControl>());
}
}
int main() {
  testStartupUsesActualBackendRateAndDoesNotMixBeforeCallback();
  testFloatGainPrecedesClippingAndOutputEnergy();
  testSpectrumFollowsAudibleOutputAndResets();
  testPendingFreezesSourceClockAndResumesDirectly();
  testNaturalEofPreservesPreparationAndPresentationTail();
  testStopInterruptsStartupAndPresentationWaitsAndReplacement();
  testFailuresAndCancellationStopOutputOffCallback();
  testStopRetainsSourceUntilInflightCallbackQuiesces();
  testFailedAutomaticStopQuiescesBackend();
}
