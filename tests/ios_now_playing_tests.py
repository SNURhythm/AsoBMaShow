#!/usr/bin/env python3
"""Exercise extracted iOS Now Playing metadata and remote seek registration."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from support.fixture_compiler import FixtureCompiler

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
    def test_pause_releases_audio_session_and_resume_keeps_queue(self):
        native = (ROOT / "src/iOSNatives.mm").read_text()
        stream = (ROOT / "src/audio/IOSMusicStream.mm").read_text()
        pause = method(stream, "void IOSMusicStream::Pause()")
        play = method(stream, "bool IOSMusicStream::Play(std::string &error)")
        native_pause = method(native, "bool PauseIOSNativeMusic(std::string &errorMessage)")
        native_play = method(native, "bool PlayIOSNativeMusic(std::string &errorMessage)")
        activate = method(native, "bool ActivateIOSNativeMusicAudioSession(std::string &errorMessage)")
        fixture = r'''
#import <Foundation/Foundation.h>
#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
std::vector<std::string> calls;
bool active = true, activationSucceeds = true, deactivationSucceeds = true;
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
NSString *const AVAudioSessionCategoryPlayback = @"playback";
@interface AVAudioSession : NSObject
+ (instancetype)sharedInstance;
- (BOOL)setCategory:(NSString *)category error:(NSError **)error;
- (BOOL)setActive:(BOOL)value error:(NSError **)error;
- (BOOL)setActive:(BOOL)value withOptions:(AVAudioSessionSetActiveOptions)options error:(NSError **)error;
@end
@implementation AVAudioSession
+ (instancetype)sharedInstance { static auto session = [AVAudioSession new]; return session; }
- (BOOL)setCategory:(NSString *)category error:(NSError **)error { return YES; }
- (BOOL)setActive:(BOOL)value error:(NSError **)error {
  if (!activationSucceeds) return NO;
  active = value; calls.push_back("activate"); return YES;
}
- (BOOL)setActive:(BOOL)value withOptions:(AVAudioSessionSetActiveOptions)options error:(NSError **)error {
  assert(!engine.running && "pause hardware before deactivating the session");
  assert(!value);
  if (!deactivationSucceeds) return NO;
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
    std::condition_variable wake;
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
    bool StartEngine(std::string &) {
      assert(active);
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
    assert(!impl.source->rendering && !active);
    assert(!gIOSNativeMusicResumeAfterInterruption);
    assert((calls == std::vector<std::string>{"player pause", "engine pause", "metadata", "deactivate"}));
    assert(PlayIOSNativeMusic(error));
    assert(impl.playing && impl.player.playing && engine.running && impl.source->rendering);
    assert(!impl.buffering && impl.timeline.Queued() == 200);
    assert((calls == std::vector<std::string>{"player pause", "engine pause", "metadata", "deactivate",
                                            "activate", "engine start", "player play", "metadata"}));
    deactivationSucceeds = false;
    assert(!PauseIOSNativeMusic(error) && !error.empty());
    assert(!impl.playing && !engine.running && !impl.source->rendering);
    deactivationSucceeds = true;
    assert(PauseIOSNativeMusic(error) && error.empty());
    activationSucceeds = false;
    assert(!PlayIOSNativeMusic(error) && !error.empty());
    assert(!impl.playing && !engine.running && !active);
    activationSucceeds = true;
    assert(PlayIOSNativeMusic(error) && error.empty());
  }
}
'''.replace("NATIVE_PAUSE", native_pause).replace("NATIVE_PLAY", native_play).replace("ACTIVATE", activate).replace("PAUSE", pause).replace("PLAY", play)
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            mm, executable = folder / "pause.mm", folder / "pause"
            mm.write_text(fixture)
            subprocess.run(["xcrun", "--sdk", "macosx", "clang++", "-std=c++20",
                            "-fobjc-arc", "-fblocks", "-framework", "Foundation",
                            str(mm), "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_finite_stream_metadata_and_remote_seek_while_buffering(self):
        source = (ROOT / "src/iOSNatives.mm").read_text()
        duration = method(source, "long long IOSNativeMusicDurationMicrosLocked()")
        update = method(source, "void UpdateIOSNativeMusicNowPlayingInfoLocked()")
        metadata = update[update.index("    const long long durationMicros ="):
                          update.index("    const NSUInteger queueCount =")]
        configure = method(source, "void ConfigureIOSNativeMusicRemoteCommands()")
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
    assert(center.changePlaybackPositionCommand.enabled);
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
            subprocess.run(["xcrun", "--sdk", "macosx", "clang++", "-std=c++20",
                            "-fobjc-arc", "-fblocks", "-framework", "Foundation",
                            str(mm), "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


class IOSStreamSeekTests(unittest.TestCase):
    def test_native_seek_notifies_source_without_waiting_for_feeder(self):
        source = (ROOT / "src/audio/IOSMusicStream.mm").read_text()
        seek = method(source, "void IOSMusicStream::Seek(long long positionMicros)")
        stop = method(source, "void IOSMusicStream::Stop()")
        stop = stop.replace("[impl->engine pause];", "")
        fixture = r'''
#include <algorithm>
#include <cassert>
#include <condition_variable>
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
    std::condition_variable wake;
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
            cpp, obj = folder / "seek.cpp", folder / "seek.o"
            executable = folder / ("seek" + compiler.executable_suffix)
            cpp.write_text(fixture)
            subprocess.run(compiler.compile_command(cpp, obj), check=True)
            subprocess.run(compiler.link_command([obj], executable), check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
