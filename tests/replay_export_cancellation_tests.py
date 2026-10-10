#!/usr/bin/env python3
"""Execute the actual export/audio cancellation boundary without a GPU."""
from pathlib import Path
import subprocess
import tempfile

from support.fixture_compiler import FixtureCompiler

compiler = FixtureCompiler.from_environment()
root = Path(__file__).resolve().parents[1]
source = (root / 'src/ReplayVideoExporter.cpp').read_text()
start = source.index('ReplayAudioTrackResult\nwriteReplayAudioTrack(')
end = source.index('\nsf_count_t replayAudioFramesForMicros', start)
helper = source[start:end]
fixture = r'''
#include <atomic>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>
#include <stdexcept>
#define TARGET_OS_IPHONE 0
namespace bms_parser { struct Chart {}; }
struct ReplayData { struct { bool clubMode = false; } provenance; };
namespace preparation {
struct Metronome { bool enabled = false; };
struct Plan { int playback = 100; long long playbackStartTimeMicros = 0; Metronome metronome; };
}
struct ReplayVideoExportLog { void write(const std::string &) {} };
struct ReplayAudioTrackResult {
  bool success = false; std::filesystem::path outputPath; std::string message;
  long long durationMicros = 0;
};
std::stop_source nativeStop;
bool cancelDuringRender = false;
namespace chart_audio {
enum class KeySoundMode { ChartTiming, ReplayTiming };
struct RenderOptions {
  KeySoundMode keySoundMode; const ReplayData *replay; int playback; bool clubMode;
  long long keySoundOffsetMicros, timelineStartMicros, playbackEventDeadlineMicros;
  const preparation::Metronome *prepMetronomePlan; std::atomic_bool *isCancelled;
  std::function<void(const std::string &)> log;
};
ReplayAudioTrackResult RenderChartAudioToWav(bms_parser::Chart &, const std::filesystem::path &, const RenderOptions &options) {
  if (cancelDuringRender) {
    if (options.isCancelled->load()) throw std::runtime_error("fresh audio was cancelled");
    std::thread native([] { nativeStop.request_stop(); });
    native.join();
  }
  if (!options.isCancelled->load())
    throw std::runtime_error("native cancellation did not reach audio preparation");
  return {false, {}, "cancelled", 0};
}
}
'''
fixture += helper
fixture += r'''
int main() {
  bms_parser::Chart chart;
  nativeStop.request_stop();
  auto result = writeReplayAudioTrack(chart, {}, {}, 0, 0, {}, nullptr, false, nativeStop.get_token());
  if (result.message != "cancelled") return 1;
  nativeStop = std::stop_source{};
  cancelDuringRender = true;
  result = writeReplayAudioTrack(chart, {}, {}, 0, 0, {}, nullptr, true, nativeStop.get_token());
  return result.message == "cancelled" ? 0 : 1;
}
'''
with tempfile.TemporaryDirectory(prefix='replay-export-cancellation-') as directory:
    cpp = Path(directory) / 'boundary.cpp'
    binary = Path(directory) / ('boundary' + compiler.executable_suffix)
    cpp.write_text(fixture)
    compiler.build([cpp], binary, directory)
    subprocess.run([str(binary)], check=True)
print('Replay export cancellation boundary passed')

# Exercise the real copy/pad loops with a deterministic native cancellation at
# the sndfile I/O boundary. No wall-clock race or large WAV fixture is needed.
audio_start = source.index('sf_count_t replayAudioFramesForMicros(')
audio_end = source.index('\nColor resultGaugeLineColor', audio_start)
audio_fixture = r'''
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
using sf_count_t = long long;
constexpr int kExportChannels = 2, kExportSampleRate = 48000;
constexpr int SFM_READ = 1, SFM_WRITE = 2, SF_FORMAT_WAV = 4, SF_FORMAT_PCM_16 = 8;
struct SF_INFO { int channels = 0, samplerate = 0, format = 0; };
struct SNDFILE {};
std::stop_source nativeStop;
int writes = 0, reads = 0, opens = 0, cancelAtWrite = 0;
namespace asobmashow::audio {
std::unique_ptr<SNDFILE> openSoundFileHandle(const std::filesystem::path &, int, SF_INFO &info) {
  ++opens; info.channels = 2; info.samplerate = 48000;
  return std::make_unique<SNDFILE>();
}
}
const char *sf_strerror(SNDFILE *) { return "I/O failure"; }
sf_count_t sf_readf_short(SNDFILE *, short *, sf_count_t frames) { ++reads; return frames; }
sf_count_t sf_writef_short(SNDFILE *, const short *, sf_count_t frames) {
  if (++writes == cancelAtWrite) {
    std::thread native([] { nativeStop.request_stop(); });
    native.join();
  }
  return frames;
}
struct ReplayVideoExportLog { void write(const std::string &) {} };
struct ReplayAudioTrackResult {
  bool success = false; std::filesystem::path outputPath; std::string message;
  long long durationMicros = 0;
};
'''
audio_fixture += source[audio_start:audio_end]
audio_fixture += r'''
// Accept the old boundary as well so the regression fails on behavior before
// the cancellation parameter is introduced, rather than failing to compile.
template<class Function, class... Args>
auto invokeWithStop(Function function, Args&&... args) {
  if constexpr (std::is_invocable_v<Function, Args..., std::stop_token>)
    return function(std::forward<Args>(args)..., nativeStop.get_token());
  else
    return function(std::forward<Args>(args)...);
}
void reset(int cancelWrite) {
  nativeStop = std::stop_source{}; writes = reads = opens = 0; cancelAtWrite = cancelWrite;
}
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char **argv) {
  const std::string scenario = argc > 1 ? argv[1] : "copy";
  reset(3);
  const std::filesystem::path output = "course.wav";
  if (scenario == "pre-cancel") nativeStop.request_stop();
  if (scenario == "success") cancelAtWrite = 0;
  if (scenario == "aligned") {
    std::string error;
    const bool success = invokeWithStop(writeReplayAudioFileAtDuration,
        std::filesystem::path("input.wav"), output, 600000000LL, 0LL, error);
    require(!success && error.find("cancel") != std::string::npos,
            "aligned replay padding ignored native cancellation");
  } else {
    const bool padding = scenario == "pad";
    std::vector<CourseReplayAudioSegment> segments{{"input.wav", 600000000LL,
                                                   padding ? 0LL : 600000000LL}};
    const auto result = invokeWithStop(writeCourseReplayAudioTrack, segments,
                                       output, static_cast<ReplayVideoExportLog *>(nullptr));
    if (scenario == "success") {
      require(result.success && result.durationMicros == 600000000LL,
              "uncancelled course audio duration changed");
      return 0;
    }
    require(!result.success && result.message.find("cancel") != std::string::npos,
            "course audio ignored native cancellation");
  }
  if (scenario == "pre-cancel") require(opens == 0, "cancelled export opened WAV files");
  else require(writes == 3 && reads <= 3, "audio continued copying after cancellation");
}
'''
with tempfile.TemporaryDirectory(prefix='replay-course-cancellation-') as directory:
    cpp = Path(directory) / 'audio.cpp'
    binary = Path(directory) / ('audio' + compiler.executable_suffix)
    cpp.write_text(audio_fixture)
    compiler.build([cpp], binary, directory)
    results = [subprocess.run([str(binary), scenario], capture_output=True, text=True)
               for scenario in ('copy', 'pad', 'aligned', 'pre-cancel', 'success')]
    for scenario, result in zip(('copy', 'pad', 'aligned', 'pre-cancel', 'success'), results):
        if result.returncode:
            print(f'{scenario}: {result.stderr.strip()}')
    assert all(result.returncode == 0 for result in results), 'audio cancellation regression'
print('Course copy/pad and aligned audio cancellation passed')

encode_start = source.index('bool encodeFrame(AVCodecContext')
encode_end = source.index('\nbool fillAudioFrame(', encode_start)
encode_fixture = r'''
#include <chrono>
#include <cstdint>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#define AVERROR(value) (-(value))
#define AVERROR_EOF -2
#define AV_NOPTS_VALUE -1
struct AVCodecContext { int time_base = 1; };
struct AVFormatContext {};
struct AVStream { int time_base = 1, index = 0; };
struct AVFrame {};
struct AVPacket { int64_t pts = 0, dts = 0, duration = 0; int stream_index = 0; };
struct ReplayFfmpegEncodeProfile {
  long long receiveMicros = 0, sendMicros = 0, writeMicros = 0, stallMicros = 0, packetCount = 0;
  int stalledRetries = 0;
};
std::stop_source nativeStop;
int delays = 0, sends = 0, receives = 0;
long long elapsedMicros(std::chrono::steady_clock::time_point) { return 0; }
std::string ffmpegError(int) { return "ffmpeg error"; }
void av_packet_unref(AVPacket *) {}
void av_packet_rescale_ts(AVPacket *, int, int) {}
int avcodec_send_frame(AVCodecContext *, AVFrame *) { ++sends; return AVERROR(EAGAIN); }
int avcodec_receive_packet(AVCodecContext *, AVPacket *) { ++receives; return AVERROR(EAGAIN); }
int av_interleaved_write_frame(AVFormatContext *, AVPacket *) { return 0; }
void SDL_Delay(int) { ++delays; nativeStop.request_stop(); }
'''
encode_fixture += source[encode_start:encode_end]
encode_fixture += r'''
template<class Function, class... Args>
auto invokeWithStop(Function function, Args&&... args) {
  if constexpr (std::is_invocable_v<Function, Args..., std::stop_token>)
    return function(std::forward<Args>(args)..., nativeStop.get_token());
  else
    return function(std::forward<Args>(args)...);
}
int main() {
  AVCodecContext codec; AVFormatContext format; AVStream stream; AVPacket packet;
  std::string error;
  bool success = invokeWithStop(encodeFrame, &codec, &format, &stream,
      static_cast<AVFrame *>(nullptr), &packet, error, int64_t(0),
      static_cast<ReplayFfmpegEncodeProfile *>(nullptr));
  if (success || error.find("cancel") == std::string::npos || delays != 1 || sends != 1)
    throw std::runtime_error("encoder retries continued after native cancellation");
}
'''
with tempfile.TemporaryDirectory(prefix='replay-encode-cancellation-') as directory:
    cpp = Path(directory) / 'encode.cpp'
    binary = Path(directory) / ('encode' + compiler.executable_suffix)
    cpp.write_text(encode_fixture)
    compiler.build([cpp], binary, directory)
    subprocess.run([str(binary)], check=True)
print('Codec stall cancellation passed')

async_start = source.index('class ReplayAsyncFrameEncoder {')
async_end = source.index('\nReplayVideoExportResult\nrenderReplayVideoToMp4', async_start)
async_fixture = r'''
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
struct ReplayVideoExportLog {};
struct ReplayVideoExportResult {
  bool success = false; std::filesystem::path outputPath; std::string message;
};
std::stop_source nativeStop;
std::atomic_int encoded{0};
long long elapsedMicros(std::chrono::steady_clock::time_point) { return 0; }
void SDL_Delay(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
class ReplayPlatformMp4StreamWriter {
  std::stop_token stop;
public:
  bool open(const std::filesystem::path &, const std::filesystem::path &, int, int, int,
            ReplayVideoExportLog *, std::string &, std::stop_token token = {}) {
    stop = token; return true;
  }
  bool encodeVideoFrame(const uint8_t *, size_t, long long, std::string &) {
    ++encoded; nativeStop.request_stop(); return true;
  }
  ReplayVideoExportResult finish() {
    return {!stop.stop_requested(), {}, stop.stop_requested() ? "cancelled" : "finished"};
  }
#define PROFILE(name) long long name() const { return 0; }
  PROFILE(audioEncodeMicros) PROFILE(framePrepareMicros) PROFILE(pixelConvertMicros)
  PROFILE(videoEncodeMicros) PROFILE(videoSendMicros) PROFILE(videoReceiveMicros)
  PROFILE(videoWriteMicros) PROFILE(videoStallMicros) PROFILE(videoFlushMicros)
  PROFILE(videoPackets) PROFILE(videoStalls) PROFILE(videoThreadCount)
  PROFILE(maxVideoFrameRefCount) PROFILE(videoFrameCowCopies)
#undef PROFILE
};
'''
async_fixture += source[async_start:async_end]
async_fixture += r'''
template<class Encoder>
void startEncoder(Encoder &encoder, std::string &error) {
  if constexpr (std::is_invocable_v<decltype(&Encoder::start), Encoder *,
      std::filesystem::path, std::filesystem::path, int, int, int, size_t, size_t,
      ReplayVideoExportLog *, std::string &, std::stop_token>)
    encoder.start({}, {}, 1, 1, 1, 4, 2, nullptr, error, nativeStop.get_token());
  else
    encoder.start({}, {}, 1, 1, 1, 4, 2, nullptr, error);
}
int main() {
  ReplayAsyncFrameEncoder encoder;
  std::string error;
  startEncoder(encoder, error);
  int buffer = encoder.acquireFrameBuffer(error);
  encoder.submitFrame(buffer, 0, 0, error);
  auto result = encoder.finish();
  if (result.success || result.message.find("cancel") == std::string::npos)
    throw std::runtime_error("finalization ignored native cancellation");
}
'''
with tempfile.TemporaryDirectory(prefix='replay-async-cancellation-') as directory:
    cpp = Path(directory) / 'async.cpp'
    binary = Path(directory) / ('async' + compiler.executable_suffix)
    cpp.write_text(async_fixture)
    compiler.build([cpp], binary, directory)
    subprocess.run([str(binary)], check=True, timeout=10)
print('Async encoder finalization cancellation passed')

native_fixture = r'''
#include <chrono>
#include <future>
#include <stop_token>
#include <stdexcept>
#include <string>
#if __has_include("replay/ReplayNativeExportOperation.h")
#include "replay/ReplayNativeExportOperation.h"
#else
namespace replay_video_export {
template<class Task>
bool runNativeExportOperation(std::stop_token, Task task, std::string &error) {
  return task(error);
}
}
#endif
int main() {
  using namespace std::chrono_literals;
  std::stop_source lifecycle;
  std::promise<void> entered, release, cleaned;
  auto gate = release.get_future().share();
  auto completion = cleaned.get_future();
  std::string error;
  auto owner = std::async(std::launch::async, [&] {
    return replay_video_export::runNativeExportOperation(lifecycle.get_token(),
      [gate, &entered, &cleaned](std::string &) {
        entered.set_value(); gate.wait(); cleaned.set_value(); return true;
      }, error);
  });
  entered.get_future().wait();
  lifecycle.request_stop();
  const bool returnedBeforeNativeCompletion = owner.wait_for(1s) == std::future_status::ready;
  release.set_value();
  const bool result = owner.get();
  completion.wait();
  if (!returnedBeforeNativeCompletion || result || error.find("cancel") == std::string::npos)
    throw std::runtime_error("native export wait blocked owner after cancellation");
  bool started = false;
  if (replay_video_export::runNativeExportOperation(lifecycle.get_token(),
      [&](std::string &) { started = true; return true; }, error) || started)
    throw std::runtime_error("pre-cancelled native work started");
  lifecycle = std::stop_source{};
  if (!replay_video_export::runNativeExportOperation(lifecycle.get_token(),
      [](std::string &) { return true; }, error))
    throw std::runtime_error("successful native export lost result");
  if (replay_video_export::runNativeExportOperation(lifecycle.get_token(),
      [](std::string &message) { message = "native failure"; return false; }, error)
      || error != "native failure")
    throw std::runtime_error("native export lost error");
}
'''
with tempfile.TemporaryDirectory(prefix='replay-native-cancellation-') as directory:
    cpp = Path(directory) / 'native.cpp'
    binary = Path(directory) / ('native' + compiler.executable_suffix)
    cpp.write_text(native_fixture)
    compiler.build([cpp], binary, directory, includes=[root / 'src'])
    subprocess.run([str(binary)], check=True, timeout=10)
print('Native export owner cancellation and completion lifetime passed')

wait_marker = 'bool waitForReplayExportForeground('
wait_fixture = r'''
#include <stop_token>
#include <stdexcept>
#define TARGET_OS_IPHONE 1
#define TARGET_IPHONE_SIMULATOR 0
bool active = false, resume = false;
int waits = 0;
std::stop_source nativeStop;
bool IOSApplicationActive() { return active; }
void SDL_Delay(int) {
  ++waits;
  if (resume) active = true;
  else nativeStop.request_stop();
}
'''
if wait_marker in source:
    wait_start = source.index(wait_marker)
    wait_end = source.index('\nclass ScopedReplayVideoBgfxAccess', wait_start)
    wait_fixture += source[wait_start:wait_end]
else:
    wait_fixture += 'bool waitForReplayExportForeground(std::stop_token) { return true; }\n'
wait_fixture += r'''
int main() {
  if (waitForReplayExportForeground(nativeStop.get_token()) || waits != 1)
    throw std::runtime_error("inactive export did not wait cancellably");
  nativeStop = std::stop_source{}; waits = 0; resume = true;
  if (!waitForReplayExportForeground(nativeStop.get_token()) || waits != 1)
    throw std::runtime_error("export did not resume after temporary inactivity");
}
'''
with tempfile.TemporaryDirectory(prefix='replay-foreground-cancellation-') as directory:
    cpp = Path(directory) / 'foreground.cpp'
    binary = Path(directory) / ('foreground' + compiler.executable_suffix)
    cpp.write_text(wait_fixture)
    compiler.build([cpp], binary, directory)
    subprocess.run([str(binary)], check=True)
print('Inactive export pause/resume and cancellation passed')
