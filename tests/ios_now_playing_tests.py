#!/usr/bin/env python3
"""Exercise iOS native transport lifecycle, metadata, and remote seek routing."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from support.fixture_compiler import FixtureCompiler, build_foundation_fixture

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


@unittest.skipUnless(sys.platform == "darwin", "requires host Foundation")
class IOSNowPlayingTests(unittest.TestCase):
    def test_stream_completion_and_failure_pause_the_audio_engine(self):
        stream = (ROOT / "src/audio/IOSMusicStream.mm").read_text()
        run = method(stream, "void Run()")
        ended = method(run, "if (ended)")
        terminal = method(run, "if (result.status == music_stream::ReadStatus::Cancelled ||")
        fixture = r'''
#import <Foundation/Foundation.h>
#include "audio/IOSMusicStreamTimeline.h"
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
@interface FakeEngine : NSObject
@property BOOL running;
- (void)pause;
@end
@implementation FakeEngine
- (void)pause { self.running = NO; }
@end
@interface FakePlayer : NSObject
@property BOOL playing;
- (void)pause;
@end
@implementation FakePlayer
- (void)pause { self.playing = NO; }
@end
namespace music_stream {
enum class ReadStatus { Cancelled, Error };
struct ReadResult { ReadStatus status; std::string error; };
}
enum class EventKind { Finished, Error, StateChanged };
struct Source {
  bool rendering = true;
  void SetRenderingEnabled(bool enabled) { rendering = enabled; }
};
struct Wake { void Wait(std::unique_lock<std::mutex> &) {} };
struct Feeder {
  FakeEngine *engine = [FakeEngine new];
  FakePlayer *player = [FakePlayer new];
  std::shared_ptr<Source> source = std::make_shared<Source>();
  std::shared_ptr<Wake> wake = std::make_shared<Wake>();
  struct Completion { std::atomic<std::uint64_t> played{0}; };
  std::shared_ptr<Completion> completion = std::make_shared<Completion>();
  ios_music_stream::Timeline timeline;
  bool playing = true, ended = true, notified = false;
  std::mutex mutex;
  std::string failure;
  std::vector<EventKind> events;
  Feeder() { engine.running = YES; player.playing = YES; }
  void Notify(EventKind kind, const std::string & = {}) { events.push_back(kind); }
  void Finish() {
    std::unique_lock lock(mutex);
    for (int step = 0; step < 1; ++step) { ENDED }
  }
  void Accept(music_stream::ReadResult result) {
    std::unique_lock lock(mutex);
    for (int step = 0; step < 1; ++step) { TERMINAL }
  }
};
int main() {
  @autoreleasepool {
    Feeder complete;
    complete.timeline.Queue(4096);
    complete.completion->played = 4095;
    complete.Finish();
    assert(complete.engine.running && complete.player.playing && complete.events.empty());
    complete.completion->played = 4096;
    complete.Finish();
    assert(!complete.engine.running && "completed native music must stop engine rendering");
    assert(!complete.player.playing && !complete.playing && !complete.source->rendering);
    assert(complete.events == std::vector<EventKind>{EventKind::Finished});
    for (const auto status : {music_stream::ReadStatus::Cancelled, music_stream::ReadStatus::Error}) {
      Feeder failed;
      failed.Accept({status, "fixture failure"});
      assert(!failed.engine.running && "terminal source failure must stop engine rendering");
      assert(!failed.player.playing && !failed.playing && !failed.source->rendering);
      assert(failed.events == std::vector<EventKind>{status == music_stream::ReadStatus::Error
          ? EventKind::Error : EventKind::StateChanged});
    }
  }
}
'''.replace("ENDED", ended).replace("TERMINAL", terminal)
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            mm, executable = folder / "terminal.mm", folder / "terminal"
            mm.write_text(fixture)
            build_foundation_fixture(mm, executable, includes=[ROOT / "src"])
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_pause_restores_foreground_session_and_resume_keeps_queue(self):
        native = (ROOT / "src/iOSNatives.mm").read_text()
        stream = (ROOT / "src/audio/IOSMusicStream.mm").read_text()
        pause = method(stream, "void IOSMusicStream::Pause()")
        play = method(stream, "bool IOSMusicStream::Play(std::string &error)")
        native_pause = method(native, "bool PauseIOSNativeMusic(std::string &errorMessage)")
        native_play = method(native, "bool PlayIOSNativeMusic(std::string &errorMessage)")
        activate = method(native, "bool ActivateIOSNativeMusicAudioSession(std::string &errorMessage)")
        restore = method(native, "bool RestoreIOSForegroundAudioSession(std::string &errorMessage)")
        fixture = r'''
#import <Foundation/Foundation.h>
#include "audio/IOSMusicStreamWake.h"
#include <cassert>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
std::vector<std::string> calls;
bool active = true, activationSucceeds = true, deactivationSucceeds = true;
bool engineStartSucceeds = true, deactivationBusy = false;
NSString *category;
unsigned gIOSNativeMusicTransportGeneration = 0;
@interface FakeEngine : NSObject
@property BOOL running;
- (void)pause;
@end
@implementation FakeEngine
- (void)pause { calls.push_back("engine pause"); self.running = NO; }
@end
@interface FakePlayer : NSObject
@property BOOL playing;
- (void)pause;
- (void)play;
- (void)playImmediatelyAtRate:(float)rate;
@end
@implementation FakePlayer
- (void)pause { calls.push_back("player pause"); self.playing = NO; }
- (void)play { assert(active); calls.push_back("player play"); self.playing = YES; }
- (void)playImmediatelyAtRate:(float)rate { [self play]; }
@end
FakeEngine *engine;
#define AVAudioSession FakeAudioSession
using AVAudioSessionSetActiveOptions = NSUInteger;
constexpr auto AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation = 1;
constexpr auto AVAudioSessionErrorCodeIsBusy = 560030580;
NSString *const AVAudioSessionCategoryPlayback = @"playback";
NSString *const AVAudioSessionCategoryAmbient = @"ambient";
constexpr auto AVAudioSessionCategoryOptionMixWithOthers = 1;
@interface AVAudioSession : NSObject
+ (instancetype)sharedInstance;
- (BOOL)setCategory:(NSString *)value error:(NSError **)error;
- (BOOL)setCategory:(NSString *)value withOptions:(NSUInteger)options error:(NSError **)error;
- (BOOL)setActive:(BOOL)value error:(NSError **)error;
- (BOOL)setActive:(BOOL)value withOptions:(AVAudioSessionSetActiveOptions)options error:(NSError **)error;
@end
@implementation AVAudioSession
+ (instancetype)sharedInstance { static auto session = [AVAudioSession new]; return session; }
- (BOOL)setCategory:(NSString *)value error:(NSError **)error { category = value; return YES; }
- (BOOL)setCategory:(NSString *)value withOptions:(NSUInteger)options error:(NSError **)error {
  assert(options == AVAudioSessionCategoryOptionMixWithOthers);
  category = value; return YES;
}
- (BOOL)setActive:(BOOL)value error:(NSError **)error {
  if (!activationSucceeds) return NO;
  active = value; calls.push_back("activate"); return YES;
}
- (BOOL)setActive:(BOOL)value withOptions:(AVAudioSessionSetActiveOptions)options error:(NSError **)error {
  assert(!engine.running && "pause hardware before deactivating the session");
  assert(options == AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation);
  assert(!value);
  if (!deactivationSucceeds) return NO;
  if (deactivationBusy) {
    // iOS 8–25 may deactivate the session despite returning IsBusy when a
    // retained foreground output is still running after the music pause.
    active = false;
    *error = [NSError errorWithDomain:NSOSStatusErrorDomain
                                code:AVAudioSessionErrorCodeIsBusy userInfo:nil];
    return NO;
  }
  active = value;
  calls.push_back("deactivate");
  return YES;
}
@end
std::string NSErrorMessage(NSError *, const char *fallback) { return fallback; }
struct Source {
  bool rendering = true;
  void SetRenderingEnabled(bool enabled) { rendering = enabled; }
  void RequestSeek(int) { assert(false && "resume must preserve queued PCM"); }
};
class IOSMusicStream {
public:
  struct Impl {
    std::mutex mutex;
    std::shared_ptr<ios_music_stream::FeederWake> wake =
        std::make_shared<ios_music_stream::FeederWake>();
    bool playing = true, buffering = false, ended = false;
    std::string failure;
    FakeEngine *engine;
    FakePlayer *player = [FakePlayer new];
    std::shared_ptr<Source> source = std::make_shared<Source>();
    struct { unsigned load() { return 100; } } played;
    struct Completion { decltype(played) played; };
    std::shared_ptr<Completion> completion = std::make_shared<Completion>();
    struct Timeline {
      unsigned Queued() { return 200; }
      bool Drained(unsigned played) { return played >= Queued(); }
    } timeline;
    bool StartEngine(std::string &error) {
      assert(active);
      if (!engineStartSucceeds) { error = "engine startup failed"; return false; }
      calls.push_back("engine start");
      engine.running = YES;
      return true;
    }
    void Reset(int) { assert(false && "pause/resume must preserve timeline"); }
  };
  std::unique_ptr<Impl> impl = std::make_unique<Impl>();
  void Pause();
  bool Play(std::string &);
};
PAUSE
PLAY
std::shared_ptr<IOSMusicStream> gIOSNativeMusicStream;
FakePlayer *gIOSNativeMusicPlayer = nil;
bool gIOSNativeMusicResumeAfterInterruption = true;
id IOSNativeMusicLock() { static auto lock = [NSObject new]; return lock; }
void UpdateIOSNativeMusicNowPlayingInfoLocked() {
  calls.push_back("metadata");
}
float gIOSNativeMusicPlaybackRate = 1.0f;
ACTIVATE
RESTORE
NATIVE_PAUSE
NATIVE_PLAY
int main() {
  @autoreleasepool {
    engine = [FakeEngine new]; engine.running = YES;
    gIOSNativeMusicStream = std::make_shared<IOSMusicStream>();
    auto &impl = *gIOSNativeMusicStream->impl;
    impl.engine = engine;
    impl.player.playing = YES;
    std::string error;
    assert(PauseIOSNativeMusic(error));
    assert(!impl.playing && !impl.player.playing && !engine.running);
    assert(!impl.source->rendering && active);
    assert(category == AVAudioSessionCategoryAmbient);
    assert(!gIOSNativeMusicResumeAfterInterruption);
    assert(gIOSNativeMusicTransportGeneration == 1);
    assert((calls == std::vector<std::string>{"player pause", "engine pause", "metadata", "deactivate", "activate"}));
    assert(PlayIOSNativeMusic(error));
    assert(impl.playing && impl.player.playing && engine.running && impl.source->rendering);
    assert(!impl.buffering && impl.timeline.Queued() == 200);
    assert(gIOSNativeMusicTransportGeneration == 2);
    assert((calls == std::vector<std::string>{"player pause", "engine pause", "metadata", "deactivate", "activate",
                                            "activate", "engine start", "player play", "metadata"}));
    deactivationSucceeds = false;
    assert(!PauseIOSNativeMusic(error) && !error.empty());
    assert(!impl.playing && !engine.running && !impl.source->rendering);
    deactivationSucceeds = true;
    assert(PauseIOSNativeMusic(error) && error.empty());
    activationSucceeds = false;
    assert(!PlayIOSNativeMusic(error) && !error.empty());
    assert(!impl.playing && !engine.running);
    activationSucceeds = true;
    assert(PlayIOSNativeMusic(error) && error.empty());
    assert(gIOSNativeMusicTransportGeneration == 6);
    assert(PauseIOSNativeMusic(error));
    engineStartSucceeds = false;
    assert(!PlayIOSNativeMusic(error) && error == "engine startup failed");
    assert(!impl.playing && !engine.running && !impl.source->rendering);
    assert(active && category == AVAudioSessionCategoryAmbient);
    engineStartSucceeds = true;
    assert(PlayIOSNativeMusic(error));
    deactivationBusy = true;
    assert(PauseIOSNativeMusic(error) && error.empty());
    assert(active && category == AVAudioSessionCategoryAmbient);
    assert(!impl.playing && !impl.player.playing && !engine.running && !impl.source->rendering);
    activationSucceeds = false;
    assert(!PauseIOSNativeMusic(error) && !error.empty());
    assert(!active && category == AVAudioSessionCategoryAmbient);
  }
}
'''.replace("NATIVE_PAUSE", native_pause).replace("NATIVE_PLAY", native_play).replace("ACTIVATE", activate).replace("RESTORE", restore).replace("PAUSE", pause).replace("PLAY", play)
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            mm, executable = folder / "pause.mm", folder / "pause"
            mm.write_text(fixture)
            build_foundation_fixture(mm, executable, includes=[ROOT / "src"])
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_failed_stream_is_unloaded_and_stale_callbacks_are_ignored(self):
        native = (ROOT / "src/iOSNatives.mm").read_text()
        load = method(native, "bool LoadIOSNativeMusicStream(")
        callback = method(load, "[identity](std::uint64_t epoch,")
        fixture = r'''
#import <Foundation/Foundation.h>
#include <cassert>
#include <memory>
#include <string>
#include <cstdint>
unsigned destroyed = 0, restored = 0, published = 0, errors = 0, finished = 0;
struct IOSMusicStream {
  enum class EventKind { Finished, Error, StateChanged };
  ~IOSMusicStream() { ++destroyed; }
  bool IsCurrentEvent(std::uint64_t epoch) { return epoch == 7; }
};
std::shared_ptr<IOSMusicStream> gIOSNativeMusicStream;
unsigned gIOSNativeMusicTransportGeneration = 0;
bool gIOSNativeMusicResumeAfterInterruption = true;
struct Metadata { std::string title; } gIOSNativeMusicMetadata;
struct Queue { int count; } gIOSNativeMusicQueue;
id IOSNativeMusicLock() { static auto lock = [NSObject new]; return lock; }
void UpdateIOSNativeMusicNowPlayingInfoLocked() { ++published; }
void PublishIOSNativeMusicNowPlayingInfoLocked(id info, bool playing) {
  assert(!info && !playing && !gIOSNativeMusicStream); ++published;
}
bool RestoreIOSForegroundAudioSession(std::string &) {
  assert(!gIOSNativeMusicStream && destroyed == 1); ++restored; return true;
}
namespace native_music_player {
enum class ControlEvent { Finished };
void NotifyControlEvent(ControlEvent) { ++finished; }
void NotifyPlaybackError(const std::string &error) {
  assert(error == "output failed"); assert(!gIOSNativeMusicStream);
  assert(restored == 1); ++errors;
}
}
int main() {
  @autoreleasepool {
    auto identity = std::make_shared<std::weak_ptr<IOSMusicStream>>();
    auto callback = CALLBACK;
    gIOSNativeMusicStream = std::make_shared<IOSMusicStream>();
    *identity = gIOSNativeMusicStream;
    gIOSNativeMusicMetadata.title = "failed track";
    gIOSNativeMusicQueue.count = 3;
    callback(6, IOSMusicStream::EventKind::Error, "output failed");
    assert(gIOSNativeMusicStream && errors == 0 && published == 0);
    callback(7, IOSMusicStream::EventKind::Error, "output failed");
    assert(!gIOSNativeMusicStream && destroyed == 1 && errors == 1);
    assert(published == 1 && !gIOSNativeMusicResumeAfterInterruption);
    assert(gIOSNativeMusicTransportGeneration == 1);
    assert(gIOSNativeMusicMetadata.title.empty() && gIOSNativeMusicQueue.count == 0);
    gIOSNativeMusicStream = std::make_shared<IOSMusicStream>();
    callback(7, IOSMusicStream::EventKind::Error, "output failed");
    assert(gIOSNativeMusicStream && errors == 1 && published == 1);
    *identity = gIOSNativeMusicStream;
    callback(7, IOSMusicStream::EventKind::Finished, "");
    assert(gIOSNativeMusicStream && finished == 1 && restored == 1);
  }
}
'''.replace("CALLBACK", callback)
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            mm, executable = folder / "error.mm", folder / "error"
            mm.write_text(fixture)
            build_foundation_fixture(mm, executable)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_worker_publication_updates_controls_on_main_and_discards_stale_state(self):
        source = (ROOT / "src/iOSNatives.mm").read_text()
        publish = method(source, "void PublishIOSNativeMusicNowPlayingInfoLocked(")
        fixture = r'''
#import <Foundation/Foundation.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#define MPRemoteCommandCenter FakeCommandCenter
#define MPNowPlayingInfoCenter FakeInfoCenter
NSString *const MPMediaItemPropertyPlaybackDuration = @"duration";
NSString *const MPNowPlayingInfoPropertyElapsedPlaybackTime = @"elapsed";
NSString *const MPNowPlayingInfoPropertyPlaybackRate = @"rate";
constexpr int MPNowPlayingPlaybackStateStopped = 0;
constexpr int MPNowPlayingPlaybackStatePlaying = 1;
constexpr int MPNowPlayingPlaybackStatePaused = 2;
@interface FakeCommand : NSObject
@property BOOL enabled;
@end
@implementation FakeCommand
@end
@interface FakeCommandCenter : NSObject
@property(strong) FakeCommand *playCommand;
@property(strong) FakeCommand *pauseCommand;
@property(strong) FakeCommand *togglePlayPauseCommand;
@property(strong) FakeCommand *stopCommand;
@property(strong) FakeCommand *nextTrackCommand;
@property(strong) FakeCommand *previousTrackCommand;
@property(strong) FakeCommand *changePlaybackPositionCommand;
+ (instancetype)sharedCommandCenter;
@end
@implementation FakeCommandCenter
+ (instancetype)sharedCommandCenter {
  assert([NSThread isMainThread]);
  static FakeCommandCenter *center;
  if (!center) {
    center = [FakeCommandCenter new];
    center.playCommand = [FakeCommand new];
    center.pauseCommand = [FakeCommand new];
    center.togglePlayPauseCommand = [FakeCommand new];
    center.stopCommand = [FakeCommand new];
    center.nextTrackCommand = [FakeCommand new];
    center.previousTrackCommand = [FakeCommand new];
    center.changePlaybackPositionCommand = [FakeCommand new];
  }
  return center;
}
@end
@interface FakeInfoCenter : NSObject
@property(copy) NSDictionary *nowPlayingInfo;
@property int playbackState;
+ (instancetype)defaultCenter;
@end
@implementation FakeInfoCenter
+ (instancetype)defaultCenter {
  assert([NSThread isMainThread] && "MediaPlayer publication must run on main");
  static FakeInfoCenter *center = [FakeInfoCenter new];
  return center;
}
@end
std::atomic<std::uint64_t> gIOSNativeMusicNowPlayingRevision{0};
unsigned registrations = 0;
void ConfigureIOSNativeMusicRemoteCommands() {
  assert([NSThread isMainThread]);
  if (!registrations) ++registrations;
}
NSMutableArray *pending;
void QueueOnMain(int, dispatch_block_t block) { [pending addObject:[block copy]]; }
#define dispatch_async QueueOnMain
#define dispatch_get_main_queue() 0
PUBLISH
void drain() {
  NSArray *blocks = [pending copy];
  [pending removeAllObjects];
  for (dispatch_block_t block in blocks) block();
}
void publishFromWorker(NSDictionary *info, bool playing) {
  std::thread worker([info, playing] {
    @autoreleasepool { PublishIOSNativeMusicNowPlayingInfoLocked(info, playing); }
  });
  worker.join();
}
int main() {
  @autoreleasepool {
    pending = [NSMutableArray new];
    auto center = [MPNowPlayingInfoCenter defaultCenter];
    auto commands = [MPRemoteCommandCenter sharedCommandCenter];
    NSMutableDictionary *info = [@{@"duration": @180.0, @"elapsed": @10.0, @"rate": @1.25} mutableCopy];
    publishFromWorker(info, true);
    info[@"elapsed"] = @99.0;
    assert(center.nowPlayingInfo == nil && "worker must enqueue, never publish synchronously");
    drain();
    assert(registrations == 1 && commands.changePlaybackPositionCommand.enabled);
    assert([center.nowPlayingInfo[@"elapsed"] doubleValue] == 10.0 && "publish a value snapshot");
    assert(commands.playCommand.enabled && commands.pauseCommand.enabled);

    publishFromWorker(@{@"duration": @180.0, @"elapsed": @50.0, @"rate": @1.0}, true);
    PublishIOSNativeMusicNowPlayingInfoLocked(nil, false);
    // An already queued worker update must not republish a stopped track.
    auto old = (dispatch_block_t)pending.firstObject;
    old();
    assert([center.nowPlayingInfo[@"elapsed"] doubleValue] == 10.0);
    drain();
    assert(center.nowPlayingInfo == nil && !commands.changePlaybackPositionCommand.enabled);
    assert(!commands.playCommand.enabled && !commands.pauseCommand.enabled);

    // A finite item stays seekable while paused or waiting for generated PCM.
    for (bool playing : {false, true}) {
      publishFromWorker(@{@"duration": @180.0, @"elapsed": @70.0, @"rate": @0.0}, playing);
      drain();
      assert(commands.changePlaybackPositionCommand.enabled);
      assert([center.nowPlayingInfo[@"rate"] doubleValue] == 0.0);
    }
    publishFromWorker(@{@"duration": @0.0, @"elapsed": @0.0, @"rate": @0.0}, false);
    drain();
    assert(!commands.changePlaybackPositionCommand.enabled);
    assert(registrations == 1);
  }
}
'''.replace("PUBLISH", publish)
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            mm, executable = folder / "publication.mm", folder / "publication"
            mm.write_text(fixture)
            build_foundation_fixture(mm, executable)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_engine_notification_never_waits_for_main_while_holding_native_lock(self):
        source = (ROOT / "src/iOSNatives.mm").read_text()
        configure = method(source, "void ConfigureIOSNativeMusicRemoteCommands() {")
        start = configure.index("  [[NSNotificationCenter defaultCenter]\n      addObserverForName:AVAudioEngineConfigurationChangeNotification")
        observer = configure[start:configure.index("\n\n  MPRemoteCommandCenter", start)]
        route_start = configure.index("  [[NSNotificationCenter defaultCenter]\n      addObserverForName:AVAudioSessionRouteChangeNotification")
        observer = configure[route_start:start] + observer
        fixture = r'''
#import <Foundation/Foundation.h>
#include <cassert>
#include <atomic>
#include <cstdint>
#include <string>
#include <memory>
#include <thread>
NSString *const AVAudioEngineConfigurationChangeNotification = @"TestEngineChange";
NSString *const AVAudioSessionRouteChangeNotification = @"TestRouteChange";
NSString *const AVAudioSessionRouteChangeReasonKey = @"reason";
constexpr NSUInteger AVAudioSessionRouteChangeReasonOldDeviceUnavailable = 1;
#define AVAudioSession FakeAudioSession
@interface FakeAudioSession : NSObject
+ (instancetype)sharedInstance;
@end
@implementation FakeAudioSession
+ (instancetype)sharedInstance { static auto session = [FakeAudioSession new]; return session; }
@end
std::atomic<std::uint64_t> gIOSNativeMusicTransportGeneration{0};
unsigned pauses = 0;
bool playing = true;
bool IOSNativeMusicIsPlayingLocked() { return playing; }
bool PauseIOSNativeMusic(std::string &) { ++pauses; playing = false; return true; }
NSMutableArray *pending;
void QueueOnMain(int, dispatch_block_t block) { [pending addObject:[block copy]]; }
#define dispatch_async QueueOnMain
#define dispatch_get_main_queue() 0
id IOSNativeMusicLock() { static auto lock = [NSObject new]; return lock; }
struct Stream {
  void *changedEngine = nullptr;
  void ConfigurationChanged(void *engine) {
    assert([NSThread isMainThread]);
    changedEngine = engine;
  }
};
std::shared_ptr<Stream> gIOSNativeMusicStream = std::make_shared<Stream>();
void configure() {
OBSERVER
}
int main() {
  @autoreleasepool {
    pending = [NSMutableArray new];
    configure();
    id engine = [NSObject new];
    auto posted = dispatch_semaphore_create(0);
    std::thread worker([engine, posted] {
      @autoreleasepool {
        @synchronized(IOSNativeMusicLock()) {
          [[NSNotificationCenter defaultCenter]
              postNotificationName:AVAudioEngineConfigurationChangeNotification object:engine];
        }
        dispatch_semaphore_signal(posted);
      }
    });
    // Main deliberately does not service notifications until the worker returns.
    assert(dispatch_semaphore_wait(posted, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)) == 0 &&
           "posting an engine notification under the native lock must not wait for main");
    worker.join();
    assert(gIOSNativeMusicStream->changedEngine == nullptr);
    for (dispatch_block_t block in pending) block();
    assert(gIOSNativeMusicStream->changedEngine == (__bridge void *)engine);
    [pending removeAllObjects];
    auto routeChange = [] {
      [[NSNotificationCenter defaultCenter]
          postNotificationName:AVAudioSessionRouteChangeNotification
                        object:[AVAudioSession sharedInstance]
                      userInfo:@{AVAudioSessionRouteChangeReasonKey: @1}];
    };
    routeChange();
    ++gIOSNativeMusicTransportGeneration; // Explicitly start another track before delivery.
    for (dispatch_block_t block in pending) block();
    [pending removeAllObjects];
    assert(pauses == 0 && "old route notification must not pause a new playback request");
    routeChange();
    for (dispatch_block_t block in pending) block();
    [pending removeAllObjects];
    assert(pauses == 1 && !playing);
    routeChange();
    for (dispatch_block_t block in pending) block();
    assert(pauses == 1 && "a paused/stopped stream must not deactivate another audio session");
  }
}
'''.replace("OBSERVER", observer)
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            mm, executable = folder / "notification.mm", folder / "notification"
            mm.write_text(fixture)
            build_foundation_fixture(mm, executable)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_finite_stream_metadata_and_remote_seek_while_buffering(self):
        source = (ROOT / "src/iOSNatives.mm").read_text()
        duration = method(source, "long long IOSNativeMusicDurationMicrosLocked()")
        update = method(source, "void UpdateIOSNativeMusicNowPlayingInfoLocked()")
        metadata = update[update.index("    const long long durationMicros ="):
                          update.index("    const NSUInteger queueCount =")]
        configure = method(source, "void ConfigureIOSNativeMusicRemoteCommands() {")
        seek = configure[configure.index("  center.changePlaybackPositionCommand.enabled"):-1]
        fixture = r'''
#import <Foundation/Foundation.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <string>

using CMTime = double;
double CMTimeGetSeconds(CMTime value) { return value; }
@interface FakeItem : NSObject
@property CMTime duration;
@end
@implementation FakeItem
@end
@interface FakePlayer : NSObject
@property(strong) FakeItem *currentItem;
@end
@implementation FakePlayer
@end
struct StreamState { long long durationMicros = 120000000; };
struct FakeStream {
  StreamState state;
  bool buffering = false;
  StreamState State() { return state; }
  bool IsBuffering() { return buffering; }
};
std::shared_ptr<FakeStream> gIOSNativeMusicStream;
FakePlayer *gIOSNativeMusicPlayer;
struct { long long durationMicros = 0; } gIOSNativeMusicMetadata;
float gIOSNativeMusicPlaybackRate = 1.25;
bool playing = true;
double position = 12.0;
bool IOSNativeMusicIsPlayingLocked() { return playing; }
double IOSNativeMusicCurrentTimeLocked() { return position; }
NSString *const MPMediaItemPropertyPlaybackDuration = @"duration";
NSString *const MPNowPlayingInfoPropertyElapsedPlaybackTime = @"elapsed";
NSString *const MPNowPlayingInfoPropertyPlaybackRate = @"rate";
NSString *const MPNowPlayingInfoPropertyDefaultPlaybackRate = @"defaultRate";
NSString *const MPNowPlayingInfoPropertyIsLiveStream = @"live";
NSString *const MPNowPlayingInfoPropertyMediaType = @"mediaType";
constexpr int MPNowPlayingInfoMediaTypeAudio = 1;
DURATION
NSDictionary *metadata() {
  NSMutableDictionary *info = [NSMutableDictionary dictionary];
METADATA
  return info;
}
using MPRemoteCommandHandlerStatus = int;
constexpr int MPRemoteCommandHandlerStatusSuccess = 0;
constexpr int MPRemoteCommandHandlerStatusCommandFailed = 1;
@interface MPRemoteCommandEvent : NSObject
@end
@implementation MPRemoteCommandEvent
@end
@interface MPChangePlaybackPositionCommandEvent : MPRemoteCommandEvent
@property NSTimeInterval positionTime;
@end
@implementation MPChangePlaybackPositionCommandEvent
@end
using Handler = MPRemoteCommandHandlerStatus (^)(MPRemoteCommandEvent *);
@interface FakeCommand : NSObject
@property BOOL enabled;
@property(copy) Handler handler;
- (id)addTargetWithHandler:(Handler)handler;
@end
@implementation FakeCommand
- (id)addTargetWithHandler:(Handler)handler { self.handler = handler; return self; }
@end
@interface FakeCenter : NSObject
@property(strong) FakeCommand *changePlaybackPositionCommand;
@end
@implementation FakeCenter
@end
long long seekTarget = -1;
bool seekSucceeds = true;
bool SeekIOSNativeMusic(long long micros, std::string &) {
  seekTarget = micros;
  return seekSucceeds;
}
void configure(FakeCenter *center) {
SEEK
}
int main() {
  @autoreleasepool {
    gIOSNativeMusicStream = std::make_shared<FakeStream>();
    for (const bool active : {true, false}) {
      playing = active;
      for (const bool buffering : {true, false}) {
        gIOSNativeMusicStream->buffering = buffering;
        NSDictionary *info = metadata();
        assert(info[@"live"] != nil && ![info[@"live"] boolValue]);
        assert([info[@"mediaType"] intValue] == MPNowPlayingInfoMediaTypeAudio);
        assert([info[@"duration"] doubleValue] == 120.0);
        assert([info[@"elapsed"] doubleValue] == 12.0);
        assert([info[@"defaultRate"] doubleValue] == 1.25);
        assert([info[@"rate"] doubleValue] == (active && !buffering ? 1.25 : 0.0));
      }
    }
    // Initial source state must not discard an already known finite duration.
    gIOSNativeMusicStream->state.durationMicros = 0;
    gIOSNativeMusicMetadata.durationMicros = 90000000;
    assert([metadata()[@"duration"] doubleValue] == 90.0);
    gIOSNativeMusicStream->state.durationMicros = 130000000;
    assert([metadata()[@"duration"] doubleValue] == 130.0);

    FakeCenter *center = [FakeCenter new];
    center.changePlaybackPositionCommand = [FakeCommand new];
    configure(center);
    assert(!center.changePlaybackPositionCommand.enabled); // Enabled when an item is published.
    auto handler = center.changePlaybackPositionCommand.handler;
    MPChangePlaybackPositionCommandEvent *event = [MPChangePlaybackPositionCommandEvent new];
    event.positionTime = 42.5;
    assert(handler(event) == MPRemoteCommandHandlerStatusSuccess);
    assert(seekTarget == 42500000);
    event.positionTime = -1;
    assert(handler(event) == MPRemoteCommandHandlerStatusSuccess && seekTarget == 0);
    seekSucceeds = false;
    assert(handler(event) == MPRemoteCommandHandlerStatusCommandFailed);
    assert(handler([MPRemoteCommandEvent new]) == MPRemoteCommandHandlerStatusCommandFailed);
  }
}
'''.replace("DURATION", duration).replace("METADATA", metadata).replace("SEEK", seek)
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            mm, executable = folder / "now_playing.mm", folder / "now_playing"
            mm.write_text(fixture)
            build_foundation_fixture(mm, executable)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


class IOSStreamSeekTests(unittest.TestCase):
    def test_native_seek_notifies_source_without_waiting_for_feeder(self):
        source = (ROOT / "src/audio/IOSMusicStream.mm").read_text()
        seek = method(source, "void IOSMusicStream::Seek(long long positionMicros)")
        stop = method(source, "void IOSMusicStream::Stop()")
        stop = stop.replace("[impl->engine pause];", "")
        fixture = r'''
#include "audio/IOSMusicStreamWake.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
namespace music_stream { constexpr std::uint64_t kSampleRate = 44100; }
struct SourceState { bool complete = false; std::uint64_t durationFrames = 441000; };
struct FakeSource {
  SourceState state;
  unsigned requests = 0;
  std::uint64_t requestedFrame = 0;
  SourceState GetState() { return state; }
  void SetRenderingEnabled(bool) {}
  void RequestSeek(std::uint64_t frame) { ++requests; requestedFrame = frame; }
};
class IOSMusicStream {
public:
  struct Impl {
    std::mutex mutex;
    std::shared_ptr<ios_music_stream::FeederWake> wake =
        std::make_shared<ios_music_stream::FeederWake>();
    std::shared_ptr<FakeSource> source = std::make_shared<FakeSource>();
    bool playing = false, buffering = true;
    std::uint64_t resetFrame = 0;
    void Reset(std::uint64_t frame) { resetFrame = frame; }
  };
  std::unique_ptr<Impl> impl = std::make_unique<Impl>();
  void Seek(long long positionMicros);
  void Stop();
};
SEEK
STOP
int main() {
  IOSMusicStream stream;
  auto source = stream.impl->source;
  // No feeder thread runs in this fixture: the source must receive the request
  // before Seek returns, even when playback is paused or still buffering.
  for (bool playing : {false, true}) {
    for (bool buffering : {false, true}) {
      stream.impl->playing = playing;
      stream.impl->buffering = buffering;
      source->state.complete = false;
      auto before = source->requests;
      stream.Seek(20500000);
      assert(source->requests == before + 1 && "seek must notify source immediately");
      assert(source->requestedFrame == 904050);
      assert(stream.impl->resetFrame == source->requestedFrame);
      assert(stream.impl->playing == playing && stream.impl->buffering);

      source->state.complete = true;
      stream.Seek(20500000);
      assert(source->requests == before + 2);
      assert(source->requestedFrame == source->state.durationFrames);
      assert(stream.impl->resetFrame == source->requestedFrame);
      stream.Seek(-1);
      assert(source->requests == before + 3 && source->requestedFrame == 0);
      assert(stream.impl->resetFrame == 0);
      stream.Seek(5000000);
      stream.Stop();
      assert(source->requestedFrame == 0 && "stop must replace any pending seek");
      assert(stream.impl->resetFrame == 0 && !stream.impl->playing);
    }
  }
}
'''.replace("SEEK", seek).replace("STOP", stop)
        compiler = FixtureCompiler.from_environment()
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            cpp = folder / "seek.cpp"
            executable = folder / ("seek" + compiler.executable_suffix)
            cpp.write_text(fixture)
            compiler.build([cpp], executable, folder, includes=[ROOT / "src"])
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
