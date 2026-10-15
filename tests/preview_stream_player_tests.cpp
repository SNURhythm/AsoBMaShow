#include "audio/PreviewStreamPlayer.h"
#include "audio/AudioBackend.h"
#include "audio/ProgressiveMusicSource.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace {
// A source boundary double controls publication and bounded waits. The player,
// its resampler, ring buffer, lifecycle, and callback all remain production code.
struct SourceControl {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<short> samples;
  std::uint64_t first = 0;
  std::size_t published = 0;
  std::size_t maximumRead = 4096;
  bool complete = false;
  std::string error;
  std::atomic_bool cancelled{false};
  std::atomic_bool enabled{false};
  std::atomic_bool readOnCallbackThread{false};
  std::atomic<std::uint64_t> lastRead{0};
  std::thread::id callbackThread = std::this_thread::get_id();
  void publishAll() {
    { std::lock_guard lock(mutex); published = samples.size() / 2; complete = true; }
    changed.notify_all();
  }
};
std::shared_ptr<SourceControl> nextSource;

struct BackendControl {
  audio::RenderCallback render = nullptr;
  audio::RenderTimingCallback timing = nullptr;
  void *timingContext = nullptr;
  void *context = nullptr;
  std::uint32_t rate = 44100;
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
    assert(render);
    std::vector<short> output(frames * channels, 12345);
    if (timing) {
      const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      outputTimestampMicros = now + outputDelayMicros;
      timing({.outputSteadyMicros = now + outputDelayMicros,
              .outputTimestampKnown = timingKnown}, timingContext);
    }
    render(output.data(), static_cast<std::uint32_t>(frames), channels, context);
    return output;
  }
};
class FakeBackend final : public audio::IBackend {
public:
  explicit FakeBackend(std::shared_ptr<BackendControl> control) : control(std::move(control)) {}
  ~FakeBackend() override { control->started = false; }
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
    control->stoppedAtMicros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    control->stopOnCallbackThread = std::this_thread::get_id() == control->callbackThread;
    ++control->stops;
    if (control->failStop) { error = "injected stop failure"; return false; }
    control->started = false;
    return true;
  }
  bool isStarted() const override { return control->started; }
  audio::RuntimeState runtimeState() const override {
    ++control->runtimeQueries;
    return {.effectiveSampleRate = control->rate};
  }
private:
  std::shared_ptr<BackendControl> control;
};
class FakeFactory final : public audio::IBackendFactory {
public:
  explicit FakeFactory(std::shared_ptr<BackendControl> control) : control(std::move(control)) {}
  audio::Capabilities capabilities() const override { return {}; }
  std::unique_ptr<audio::IBackend> open(const audio::StreamRequest &,
      audio::RenderCallback callback, void *context, std::string &) override {
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
std::shared_ptr<music_stream::Source> source(std::shared_ptr<SourceControl> control) {
  nextSource = std::move(control);
  return music_stream::Source::Create(nullptr, {});
}
std::vector<short> drain(audio::PreviewStreamPlayer &player,
                         const std::shared_ptr<BackendControl> &backend,
                         std::size_t frames) {
  waitFor([&] { return backend->started.load(); });
  std::vector<short> result;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    waitFor([&] { return !player.GetState().buffering; });
    const auto output = backend->pump(1);
    result.insert(result.end(), output.begin(), output.end());
  }
  return result;
}

void testSpectrumFollowsCurrentFrequencyNotRecentLevels() {
  constexpr std::size_t block = 4096;
  for (const auto rate : {44100U, 48000U}) {
    auto input = std::make_shared<SourceControl>();
    input->samples.resize(block * 4 * 2);
    for (std::size_t frame = 0; frame < block * 4; ++frame) {
      const double frequency = frame < block ? 100.0 : 4000.0;
      const short sample = static_cast<short>(14000 * std::sin(6.283185307179586 * frequency * frame / 44100));
      input->samples[frame * 2] = sample;
      input->samples[frame * 2 + 1] = -sample;
    }
    input->publishAll();
    auto backend = std::make_shared<BackendControl>();
    backend->rate = rate;
    audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
    std::string error;
    assert(player.Start(source(input), 0, error));
    waitFor([&] { return backend->started.load(); });
    backend->pump(block * rate / 44100);
    const auto bass = player.GetOutputSpectrum();
    const auto bassPeak = std::max_element(bass.begin(), bass.end());
    assert(*bassPeak > 0.7f && "actual output tone must produce a visible spectrum peak");
    backend->pump(block * rate / 44100);
    const auto treble = player.GetOutputSpectrum();
    const auto treblePeak = std::max_element(treble.begin(), treble.end());
    assert(*treblePeak > 0.7f);
    assert(treblePeak - treble.begin() > bassPeak - bass.begin() + audio::kSpectrumBandCount / 4);
    // Linear output resampling can leave quiet spectral images; the former
    // fundamental must fall below -45 dB instead of persisting as a peak.
    assert(treble[bassPeak - bass.begin()] < 0.35f);
    assert(player.GetOutputSpectrum() == treble); // No new output: no cycling or drift.
    player.SetGain(0);
    backend->pump(block);
    const auto muted = player.GetOutputSpectrum();
    assert(*std::max_element(muted.begin(), muted.end()) == 0);
    player.SetGain(1);
    backend->pump(block / 2);
    player.Stop();
    const auto stopped = player.GetOutputSpectrum();
    assert(*std::max_element(stopped.begin(), stopped.end()) == 0);
  }
}

void testOutputLevelTracksAudibleSamples() {
  auto input = std::make_shared<SourceControl>();
  input->samples.resize(1024 * 2);
  for (std::size_t i = 0; i < input->samples.size(); i += 2) {
    input->samples[i] = 16384;
    input->samples[i + 1] = -16384; // Stereo energy must not cancel out.
  }
  input->publishAll();
  auto backend = std::make_shared<BackendControl>();
  audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
  std::string error;
  assert(player.GetOutputLevel() == 0);
  assert(player.Start(source(input), 0, error));
  waitFor([&] { return backend->started.load(); });
  assert(player.GetOutputLevel() == 0);
  backend->pump(128);
  assert(std::abs(player.GetOutputLevel() - 0.5f) < 0.001f);
  player.SetGain(0.5f);
  backend->pump(128);
  assert(std::abs(player.GetOutputLevel() - 0.25f) < 0.001f);
  player.SetGain(0);
  backend->pump(128);
  assert(player.GetOutputLevel() == 0);
  player.SetGain(1);
  backend->pump(128, 1);
  assert(player.GetOutputLevel() == 0); // Opposite-phase stereo is silent in mono.
  backend->pump(512);
  backend->pump(128);
  assert(player.GetOutputLevel() == 0); // EOF callback emits silence.
  player.Stop();
  assert(player.GetOutputLevel() == 0);
  auto replacement = std::make_shared<SourceControl>();
  replacement->samples.assign(1024 * 2, 16384);
  replacement->publishAll();
  assert(player.Start(source(replacement), 0, error));
  waitFor([&] { return backend->started.load(); });
  backend->pump(128);
  assert(player.GetOutputLevel() > 0);
  player.Stop();
  assert(player.GetOutputLevel() == 0); // Stop directly from a non-silent callback.
}

void testStartupReserveBridgesPreparationStall() {
  for (const auto rate : {44100U, 48000U, 88200U, 22050U}) {
    auto input = std::make_shared<SourceControl>();
    input->first = 441000;
    input->samples.assign(44100 * 5 * 2, 2345);
    input->published = 33075; // Three quarters of a second is not enough reserve.
    auto backend = std::make_shared<BackendControl>();
    backend->rate = rate;
    audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
    std::string error;
    assert(player.Start(source(input), input->first, error));
    waitFor([&] { return input->lastRead.load() >= input->first + 33075; });
    assert(!backend->started && "preview must build a reserve before starting output");
    assert(player.GetState().buffering && player.GetState().positionFrames == input->first);
    {
      std::lock_guard lock(input->mutex);
      input->published = 44100 * 4 + 1;
    }
    input->changed.notify_all();
    waitFor([&] { return backend->started.load(); });
    // Even with no further producer progress, one second must already be queued.
    const auto first = backend->pump(rate);
    assert(std::all_of(first.begin(), first.end(), [](short value) { return value == 2345; }));
    // Let the feeder fill its ring, then consume without allowing more decoding.
    waitFor([&] { return input->lastRead.load() >= input->first + 44100 * 3; });
    const auto next = backend->pump(rate);
    assert(std::all_of(next.begin(), next.end(), [](short value) { return value == 2345; }));
    assert(player.GetState().positionFrames == input->first + 88200);
    waitFor([&] { return input->lastRead.load() >= input->first + 44100 * 4; });
    const auto throughBurst = backend->pump(rate * 3 / 2);
    assert(std::all_of(throughBurst.begin(), throughBurst.end(), [](short value) { return value == 2345; }));
    assert(player.GetState().positionFrames == input->first + 154350);
    assert(!input->readOnCallbackThread);
    player.Stop();
  }
}

void testFullReserveDoesNotWakeEveryFiveMilliseconds() {
  auto input = std::make_shared<SourceControl>();
  input->samples.assign(44100 * 8 * 2, 2345);
  input->publishAll();
  auto backend = std::make_shared<BackendControl>();
  audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
  std::string error;
  assert(player.Start(source(input), 0, error));
  waitFor([&] { return input->lastRead.load() >= 44100 * 3 - 4096; });
  const auto before = backend->runtimeQueries.load();
  std::this_thread::sleep_for(200ms);
  assert(backend->runtimeQueries.load() - before < 10 &&
         "a full preview reserve must not poll the device 200 times per second");
  const auto output = backend->pump(44100);
  assert(std::all_of(output.begin(), output.end(), [](short sample) { return sample == 2345; }));
  waitFor([&] { return input->lastRead.load() >= 44100 * 4 - 4096; });
  const auto begin = std::chrono::steady_clock::now();
  player.Stop();
  assert(std::chrono::steady_clock::now() - begin < 500ms);
}

void testFinishedPreviewStopsItsDeviceWithoutCancellingPreparedAudio() {
  for (const bool knownTiming : {false, true}) {
    auto input = std::make_shared<SourceControl>();
    input->samples.assign(1024 * 2, 2345);
    input->publishAll();
    auto backend = std::make_shared<BackendControl>();
    backend->timingKnown = knownTiming;
    backend->outputDelayMicros = 250000;
    audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
    std::string error;
    assert(player.Start(source(input), 0, error));
    assert(backend->timing && "preview must observe native presentation timing for tail drain");
    waitFor([&] { return backend->started.load(); });
    const auto output = backend->pump(1024);
    assert(std::all_of(output.begin(), output.end(), [](short sample) { return sample == 2345; }));
    waitFor([&] { return player.GetState().finished; });
    std::this_thread::sleep_for(100ms);
    assert(backend->started && "submitted final PCM must not stop before presentation");
    waitFor([&] { return !backend->started.load(); });
    if (knownTiming) {
      const auto finalFrameMicros = (1024LL * 1000000 + backend->rate - 1) / backend->rate;
      assert(backend->stoppedAtMicros >= backend->outputTimestampMicros + finalFrameMicros &&
             "device stop must follow the final sample's native presentation time");
    }
    assert(!backend->stopOnCallbackThread && "native stop must run outside the render callback");
    assert(!input->cancelled && "finishing device output must preserve gameplay cache warming");
    assert(player.GetState().finished && player.GetState().positionFrames == 1024);
  }
}

void testStopInterruptsFinalQueueAndPresentationWaits() {
  for (const bool consume : {false, true}) {
    auto input = std::make_shared<SourceControl>();
    input->samples.assign(1024 * 2, 2345);
    input->publishAll();
    auto backend = std::make_shared<BackendControl>();
    backend->timingKnown = true;
    backend->outputDelayMicros = 5000000;
    audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
    std::string error;
    assert(player.Start(source(input), 0, error));
    waitFor([&] { return backend->started.load(); });
    if (consume) {
      backend->pump(1024);
      waitFor([&] { return player.GetState().finished; });
      std::this_thread::sleep_for(150ms); // Let the feeder enter the presentation wait.
    }
    const auto begin = std::chrono::steady_clock::now();
    player.Stop();
    assert(std::chrono::steady_clock::now() - begin < 500ms);
    assert(input->cancelled && !backend->started);
  }
}

void testFailedAutomaticStopQuiescesTheBackend() {
  auto input = std::make_shared<SourceControl>();
  input->samples.assign(1024 * 2, 2345);
  input->publishAll();
  auto backend = std::make_shared<BackendControl>();
  backend->timingKnown = true;
  backend->failStop = true;
  audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
  std::string error;
  assert(player.Start(source(input), 0, error));
  waitFor([&] { return backend->started.load(); });
  backend->pump(1024);
  waitFor([&] { return !backend->started.load(); });
  assert(backend->stops == 1 && !backend->stopOnCallbackThread);
  assert(player.GetState().finished && !input->cancelled);
}

void testFutureOriginUnderrunAndStop() {
  auto input = std::make_shared<SourceControl>();
  input->first = 441000;
  input->samples.assign(44100 * 3 * 2, 2345);
  input->published = 88200;
  auto backend = std::make_shared<BackendControl>();
  audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
  std::string error;
  assert(player.Start(source(input), input->first, error));
  const auto initial = drain(player, backend, 88200);
  assert(std::all_of(initial.begin(), initial.end(), [](short value) { return value == 2345; }));
  const auto silence = backend->pump(128);
  assert(std::all_of(silence.begin(), silence.end(), [](short value) { return value == 0; }));
  assert(player.GetState().positionFrames == 529200);
  assert(player.GetState().buffering && !player.GetState().finished);
  input->publishAll();
  const auto rest = drain(player, backend, 44100);
  assert(std::all_of(rest.begin(), rest.end(), [](short value) { return value == 2345; }));
  waitFor([&] { return player.GetState().finished; });
  assert(player.GetState().positionFrames == 573300);
  assert(!input->readOnCallbackThread);
  player.Stop();
  assert(input->cancelled && !input->enabled && !backend->started);
}

void testRationalResamplingAndShortReads() {
  for (const auto rate : {48000U, 88200U, 22050U}) {
    auto input = std::make_shared<SourceControl>();
    input->first = 100000;
    input->samples = {0, 0, 1000, -1000, 2000, -2000, 3000, -3000};
    input->maximumRead = 1;
    input->publishAll();
    auto backend = std::make_shared<BackendControl>();
    backend->rate = rate;
    audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
    std::string error;
    assert(player.Start(source(input), input->first, error));
    const std::vector<short> expected = rate == 48000
        ? std::vector<short>{0, 0, 919, -919, 1838, -1838, 2756, -2756, 3000, -3000}
        : rate == 88200
        ? std::vector<short>{0, 0, 500, -500, 1000, -1000, 1500, -1500,
                             2000, -2000, 2500, -2500, 3000, -3000, 3000, -3000}
        : std::vector<short>{0, 0, 2000, -2000};
    assert(drain(player, backend, expected.size() / 2) == expected);
    waitFor([&] { return player.GetState().finished; });
    assert(player.GetState().positionFrames == 100004);
    assert(!input->readOnCallbackThread);
  }
}

void testDownsampledFinalPositionDoesNotPassEnd() {
  auto input = std::make_shared<SourceControl>();
  input->first = 12345;
  input->samples = {0, 0, 1000, 1000, 2000, 2000, 3000, 3000, 4000, 4000};
  input->publishAll();
  auto backend = std::make_shared<BackendControl>();
  backend->rate = 22050;
  audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
  std::string error;
  assert(player.Start(source(input), input->first, error));
  assert((drain(player, backend, 3) == std::vector<short>{0, 0, 2000, 2000, 4000, 4000}));
  waitFor([&] { return player.GetState().finished; });
  assert(player.GetState().positionFrames == 12350);
}

void testStopWakesPendingReadAndReplacement() {
  auto stalled = std::make_shared<SourceControl>();
  stalled->samples.assign(100, 77);
  auto backend = std::make_shared<BackendControl>();
  audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
  std::string error;
  assert(player.Start(source(stalled), 0, error));
  waitFor([&] { return stalled->enabled.load(); });
  const auto begin = std::chrono::steady_clock::now();
  player.Stop();
  assert(std::chrono::steady_clock::now() - begin < 500ms);
  assert(stalled->cancelled && !backend->started);
  auto replacement = std::make_shared<SourceControl>();
  replacement->first = 900;
  replacement->samples = {987, -987};
  replacement->publishAll();
  assert(player.Start(source(replacement), 900, error));
  assert((drain(player, backend, 1) == std::vector<short>{987, -987}));
  waitFor([&] { return player.GetState().finished; });
  assert(player.GetState().positionFrames == 901);
}

void testDeviceAndSourceErrorsAreNotCompletion() {
  for (const bool sourceError : {false, true}) {
    auto input = std::make_shared<SourceControl>();
    input->samples = {100, 100};
    input->publishAll();
    if (sourceError) input->error = "injected source failure";
    auto backend = std::make_shared<BackendControl>();
    backend->failStart = !sourceError;
    audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
    std::string error;
    assert(player.Start(source(input), 0, error));
    waitFor([&] { return !player.GetState().error.empty(); });
    assert(!player.GetState().playing && !player.GetState().finished);
    assert(!input->enabled);
  }
}

void testTerminalSourceStatesStopAnAlreadyPlayingDevice() {
  for (const bool cancelled : {false, true}) {
    auto input = std::make_shared<SourceControl>();
    input->samples.assign(44100 * 4 * 2, 2345);
    input->published = 88200;
    auto backend = std::make_shared<BackendControl>();
    audio::PreviewStreamPlayer player(std::make_unique<FakeFactory>(backend));
    std::string error;
    assert(player.Start(source(input), 0, error));
    waitFor([&] { return backend->started.load(); });
    {
      std::lock_guard lock(input->mutex);
      if (cancelled) input->cancelled = true;
      else input->error = "injected failure during playback";
    }
    input->changed.notify_all();
    waitFor([&] { return !player.GetState().playing; });
    waitFor([&] { return !backend->started.load(); });
    assert(!backend->stopOnCallbackThread && !player.GetState().finished);
    assert(!input->enabled);
  }
}
} // namespace

namespace music_stream {
struct Source::Impl { std::shared_ptr<SourceControl> control; };
Source::Source(std::unique_ptr<Impl> value) : impl(std::move(value)) {}
Source::~Source() { Cancel(); }
std::shared_ptr<Source> Source::Create(std::unique_ptr<bms_parser::Chart>, std::filesystem::path, bool) {
  auto impl = std::make_unique<Impl>();
  impl->control = nextSource;
  return std::shared_ptr<Source>(new Source(std::move(impl)));
}
ReadResult Source::ReadFrames(std::uint64_t frame, std::span<short> output,
                             std::chrono::milliseconds wait) {
  auto &control = *impl->control;
  if (std::this_thread::get_id() == control.callbackThread) control.readOnCallbackThread = true;
  std::unique_lock lock(control.mutex);
  assert(frame >= control.first);
  const auto offset = frame - control.first;
  control.lastRead.store(frame);
  control.changed.wait_for(lock, wait, [&] {
    return control.cancelled || !control.error.empty() || control.complete || offset < control.published;
  });
  if (control.cancelled) return {.status = ReadStatus::Cancelled};
  if (!control.error.empty()) return {.status = ReadStatus::Error, .error = control.error};
  if (offset >= control.published)
    return {.status = control.complete ? ReadStatus::End : ReadStatus::Pending};
  const auto frames = std::min({output.size() / 2, control.published - static_cast<std::size_t>(offset), control.maximumRead});
  std::copy_n(control.samples.begin() + offset * 2, frames * 2, output.begin());
  return {.status = ReadStatus::Data, .frames = frames};
}
State Source::GetState() const {
  auto &control = *impl->control;
  std::lock_guard lock(control.mutex);
  return {.firstFrame = control.first,
          .availableFrames = control.first + control.published,
          .durationFrames = control.first + control.samples.size() / 2,
          .complete = control.complete, .cancelled = control.cancelled};
}
void Source::SetRenderingEnabled(bool enabled) { impl->control->enabled = enabled; }
void Source::Cancel() { impl->control->cancelled = true; impl->control->changed.notify_all(); }
}
namespace audio {
std::unique_ptr<IBackendFactory> CreatePlatformBackendFactory() {
  return std::make_unique<FakeFactory>(std::make_shared<BackendControl>());
}
}
int main() {
  testTerminalSourceStatesStopAnAlreadyPlayingDevice();
  testFinishedPreviewStopsItsDeviceWithoutCancellingPreparedAudio();
  testStopInterruptsFinalQueueAndPresentationWaits();
  testFailedAutomaticStopQuiescesTheBackend();
  testSpectrumFollowsCurrentFrequencyNotRecentLevels();
  testOutputLevelTracksAudibleSamples();
  testStartupReserveBridgesPreparationStall();
  testFullReserveDoesNotWakeEveryFiveMilliseconds();
  testFutureOriginUnderrunAndStop();
  testRationalResamplingAndShortReads();
  testDownsampledFinalPositionDoesNotPassEnd();
  testStopWakesPendingReadAndReplacement();
  testDeviceAndSourceErrorsAreNotCompletion();
}
