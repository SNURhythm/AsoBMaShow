#include "../targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "IOSMusicStream.h"
#include "IOSMusicStreamTimeline.h"
#include "ProgressiveMusicSource.h"
#include "../iOSNatives.hpp"
#import <AVFoundation/AVFoundation.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

struct IOSMusicStream::Impl {
  struct Completion { std::atomic<std::uint64_t> played{0}; };
  std::shared_ptr<music_stream::Source> source;
  Event event;
  AVAudioEngine *engine = nil;
  AVAudioPlayerNode *player = nil;
  AVAudioUnitVarispeed *varispeed = nil;
  AVAudioUnitTimePitch *timePitch = nil;
  AVAudioFormat *format = nil;
  std::mutex mutex;
  std::condition_variable wake;
  std::thread feeder;
  bool quitting = false;
  bool playing = false;
  bool buffering = true;
  std::uint64_t lastDuration = 0;
  bool ended = false;
  bool notified = false;
  std::string failure;
  std::uint64_t generation = 0;
  std::shared_ptr<std::atomic<std::uint64_t>> notifications =
      std::make_shared<std::atomic<std::uint64_t>>(0);
  ios_music_stream::Timeline timeline;
  std::shared_ptr<Completion> completion = std::make_shared<Completion>();

  ~Impl() {
    { std::lock_guard lock(mutex); quitting = true; playing = false; ++*notifications; }
    source->Cancel();
    wake.notify_all();
    if (feeder.joinable()) feeder.join();
    [player stop];
    [engine stop];
  }

  bool StartEngine(std::string &error) {
    if (engine.running) return true;
    NSError *nativeError = nil;
    if ([engine startAndReturnError:&nativeError]) return true;
    const char *message = nativeError.localizedDescription.UTF8String;
    error = message ? message : "Could not start music audio engine";
    return false;
  }

  std::int64_t NodeFrame() {
    AVAudioTime *render = player.lastRenderTime;
    AVAudioTime *time = render ? [player playerTimeForNodeTime:render] : nil;
    return time && time.sampleTimeValid ? time.sampleTime : -1;
  }

  std::uint64_t Position() {
    return timeline.Position(NodeFrame(), completion->played.load());
  }

  void Reset(std::uint64_t frame) {
    // stop invokes outstanding completion handlers. They retain their own epoch
    // and never access this adapter, its mutex, the source, or native UI locks.
    [player stop];
    completion = std::make_shared<Completion>();
    timeline.Reset(frame);
    ++generation;
    ++*notifications;
    ended = false;
    notified = false;
    buffering = true;
  }

  void Notify(EventKind kind, const std::string &error = {}) {
    auto callback = event;
    auto token = notifications;
    const auto epoch = token->load();
    const std::string message = error;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (token->load() == epoch) callback(epoch, kind, message);
    });
  }

  void Run() {
    std::vector<short> samples(4096 * music_stream::kChannels);
    for (;;) {
      @autoreleasepool {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return quitting || playing; });
        if (quitting) return;
        if (ended) {
          if (timeline.Drained(completion->played.load())) {
            playing = false;
            source->SetRenderingEnabled(false);
            [player pause];
            if (!notified) { notified = true; Notify(EventKind::Finished); }
          } else {
            wake.wait_for(lock, std::chrono::milliseconds(5));
          }
          continue;
        }
        // Keep roughly half a second in native buffers; source owns read-ahead.
        if (timeline.Queued() - completion->played.load() >= 22050) {
          wake.wait_for(lock, std::chrono::milliseconds(5));
          continue;
        }
        const auto first = timeline.End();
        const auto epoch = generation;
        lock.unlock();
        const auto result = source->ReadFrames(first, samples,
                                               std::chrono::milliseconds(100));
        lock.lock();
        if (quitting) return;
        if (epoch != generation || !playing) continue;
        if (result.status == music_stream::ReadStatus::Pending) {
          if (!buffering && timeline.Drained(completion->played.load())) {
            buffering = true;
            Notify(EventKind::StateChanged);
          }
          continue;
        }
        if (result.status == music_stream::ReadStatus::End) {
          ended = true;
          continue;
        }
        if (result.status == music_stream::ReadStatus::Cancelled ||
            result.status == music_stream::ReadStatus::Error) {
          playing = false;
          source->SetRenderingEnabled(false);
          [player pause];
          if (result.status == music_stream::ReadStatus::Error) {
            failure = result.error.empty() ? "Could not prepare music audio" : result.error;
            if (!notified) { notified = true; Notify(EventKind::Error, failure); }
          } else {
            Notify(EventKind::StateChanged);
          }
          continue;
        }
        if (!result.frames) continue;
        AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc]
            initWithPCMFormat:format frameCapacity:static_cast<AVAudioFrameCount>(result.frames)];
        buffer.frameLength = static_cast<AVAudioFrameCount>(result.frames);
        for (std::size_t frame = 0; frame < result.frames; ++frame) {
          for (int channel = 0; channel < music_stream::kChannels; ++channel) {
            buffer.floatChannelData[channel][frame] =
                samples[frame * music_stream::kChannels + channel] / 32768.0f;
          }
        }
        if (timeline.NeedsRestart(NodeFrame(), completion->played.load())) {
          // A time-pitch unit can still hold output after the source node has
          // exhausted its queue. Let that audio play before resetting its clock;
          // scheduling into a starved running timeline would count the silent gap.
          while (!timeline.Drained(completion->played.load()) &&
                 !quitting && playing && epoch == generation) {
            wake.wait_for(lock, std::chrono::milliseconds(5));
          }
          if (quitting) return;
          if (!playing || epoch != generation) continue;
          Reset(first);
        }
        timeline.Queue(result.frames);
        const auto end = timeline.Queued();
        auto acknowledgement = completion;
        [player scheduleBuffer:buffer completionCallbackType:AVAudioPlayerNodeCompletionDataPlayedBack
             completionHandler:^(AVAudioPlayerNodeCompletionCallbackType) {
               // Late callbacks from a flushed epoch cannot advance a new seek.
               auto previous = acknowledgement->played.load();
               while (previous < end && !acknowledgement->played.compare_exchange_weak(previous, end)) {}
             }];
        if (!player.playing) [player play];
        const auto duration = source->GetState().durationFrames;
        if (buffering || duration != lastDuration) {
          buffering = false;
          lastDuration = duration;
          Notify(EventKind::StateChanged);
        }
      }
    }
  }
};

IOSMusicStream::IOSMusicStream(std::unique_ptr<Impl> value) : impl(std::move(value)) {}
IOSMusicStream::~IOSMusicStream() = default;

std::shared_ptr<IOSMusicStream> IOSMusicStream::Create(
    std::shared_ptr<music_stream::Source> source, Event event, std::string &error) {
  if (!source) { error = "Music stream is empty"; return {}; }
  auto state = std::make_unique<Impl>();
  state->source = std::move(source);
  state->event = std::move(event);
  state->engine = [[AVAudioEngine alloc] init];
  state->player = [[AVAudioPlayerNode alloc] init];
  state->varispeed = [[AVAudioUnitVarispeed alloc] init];
  state->timePitch = [[AVAudioUnitTimePitch alloc] init];
  state->format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:music_stream::kSampleRate
                                                              channels:music_stream::kChannels];
  [state->engine attachNode:state->player];
  [state->engine attachNode:state->varispeed];
  [state->engine attachNode:state->timePitch];
  [state->engine connect:state->player to:state->varispeed format:state->format];
  [state->engine connect:state->varispeed to:state->timePitch format:state->format];
  [state->engine connect:state->timePitch to:state->engine.mainMixerNode format:state->format];
  [state->engine prepare];
  // Loading prepares the graph; only Play may start hardware and claim playback.
  state->source->SetRenderingEnabled(false);
  auto stream = std::shared_ptr<IOSMusicStream>(new IOSMusicStream(std::move(state)));
  stream->impl->feeder = std::thread([ptr = stream->impl.get()] { ptr->Run(); });
  return stream;
}

bool IOSMusicStream::Play(std::string &error) {
  std::lock_guard lock(impl->mutex);
  if (!impl->failure.empty()) { error = impl->failure; return false; }
  if (!impl->StartEngine(error)) return false;
  if (impl->ended && impl->timeline.Drained(impl->completion->played.load())) {
    impl->Reset(0);
    impl->source->RequestSeek(0);
  }
  impl->playing = true;
  impl->buffering = impl->timeline.Drained(impl->completion->played.load());
  impl->source->SetRenderingEnabled(true);
  if (impl->timeline.Queued() && !impl->timeline.Drained(impl->completion->played.load()))
    [impl->player play];
  impl->wake.notify_all();
  return true;
}

void IOSMusicStream::Pause() {
  std::lock_guard lock(impl->mutex);
  impl->playing = false;
  [impl->player pause];
  // iOS derives native play/pause state from the audio hardware/session.
  // Pausing only the node leaves the system sending pause instead of play.
  [impl->engine pause];
  impl->source->SetRenderingEnabled(false);
  impl->wake.notify_all();
}

void IOSMusicStream::Stop() {
  std::lock_guard lock(impl->mutex);
  impl->playing = false;
  impl->source->SetRenderingEnabled(false);
  impl->Reset(0);
  impl->source->RequestSeek(0);
  [impl->engine pause];
  impl->wake.notify_all();
}

void IOSMusicStream::Seek(long long positionMicros) {
  std::lock_guard lock(impl->mutex);
  const auto sourceState = impl->source->GetState();
  const auto micros = static_cast<std::uint64_t>(std::max(0LL, positionMicros));
  const auto frame = (micros / 1000000) * music_stream::kSampleRate +
                     (micros % 1000000) * music_stream::kSampleRate / 1000000;
  const auto target = sourceState.complete ? std::min(frame, sourceState.durationFrames) : frame;
  impl->Reset(target);
  impl->source->RequestSeek(target);
  impl->buffering = true;
  impl->wake.notify_all();
}

void IOSMusicStream::SetRate(float rate, bool timeStretch) {
  std::lock_guard lock(impl->mutex);
  rate = std::clamp(rate, 0.5f, 2.0f);
  impl->varispeed.rate = timeStretch ? 1.0f : rate;
  impl->timePitch.rate = timeStretch ? rate : 1.0f;
  impl->timePitch.pitch = 0;
  impl->timePitch.bypass = !timeStretch;
}

bool IOSMusicStream::IsBuffering() const {
  std::lock_guard lock(impl->mutex);
  return impl->buffering;
}

bool IOSMusicStream::IsCurrentEvent(std::uint64_t epoch) const {
  return impl->notifications->load() == epoch;
}

void IOSMusicStream::ConfigurationChanged(void *changedEngine) {
  std::lock_guard lock(impl->mutex);
  if ((__bridge void *)impl->engine != changedEngine) return;
  const auto position = impl->Position();
  impl->Reset(position);
  impl->source->RequestSeek(position);
  if (impl->playing) {
    std::string error;
    if (!impl->StartEngine(error)) {
      impl->playing = false;
      impl->failure = error;
      impl->source->SetRenderingEnabled(false);
      impl->Notify(EventKind::Error, error);
    }
  }
  impl->wake.notify_all();
}

IOSNativeMusicState IOSMusicStream::State() {
  std::lock_guard lock(impl->mutex);
  IOSNativeMusicState state;
  state.loaded = true;
  state.playing = impl->playing;
  state.positionMicros = impl->Position() * 1000000 / music_stream::kSampleRate;
  state.durationMicros = impl->source->GetState().durationFrames * 1000000 / music_stream::kSampleRate;
  return state;
}
#endif
