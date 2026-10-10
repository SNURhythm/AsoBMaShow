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
