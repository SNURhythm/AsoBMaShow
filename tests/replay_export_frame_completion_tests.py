#!/usr/bin/env python3
"""Exercise both production export readback gates without a GPU or encoder."""
from pathlib import Path
import subprocess
import tempfile
from support.fixture_compiler import FixtureCompiler

root = Path(__file__).resolve().parents[1]
source = (root / 'src/ReplayVideoExporter.cpp').read_text()
fixture = r'''
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>
int readbacks = 0, blits = 0, acquired = 0, released = 0, begun = 0, advances = 0;
bool drawsAccepted = true, uiComplete = true, allocationFailure = false;
namespace rendering {
constexpr int readback_view = 7;
}
namespace bgfx {
void blit(int, int, int, int, int) { ++blits; }
uint32_t frame() { return ++advances; }
uint32_t readTexture(int, uint8_t *) { ++readbacks; return advances + 1; }
}
bool waitForReplayExportForeground(std::stop_token) { return true; }
long long elapsedMicros(std::chrono::steady_clock::time_point) { return 0; }
struct Encoder {
  int acquireFrameBuffer(std::string &) { ++acquired; return 0; }
  void releaseFrameBuffer(size_t) { ++released; }
  uint8_t *frameData(size_t) { return nullptr; }
};
struct UiBatch { void beginFrame() { ++begun; } bool frameComplete() { return uiComplete; } };
struct Context { UiBatch uiBatchRenderer; };
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
'''
fixture += (root / 'src/rendering/FrameSubmission.h').read_text().replace('#pragma once', '')
fixture += '\nnamespace rendering::detail { extern "C" bool asobmashow_bgfx_frame_draws_accepted() { return drawsAccepted; } }\n'
start = 0
for path in range(2):
    start = source.index('  auto renderAndQueueFrame =', start)
    end = source.index('\n  };', start) + len('\n  };')
    fixture += f'\nvoid testPath{path}(bool complete) {{\n'
    fixture += r'''
  readbacks = blits = acquired = released = begun = advances = 0;
  struct { std::stop_token stop; } options;
  Encoder encoder;
  Context context;
  std::string errorMessage;
  std::deque<size_t> freeReadbackTextures{0};
  struct PendingReadback {
    size_t frameIndex, frameBufferIndex, readbackTextureIndex;
    long long songTimeMicros;
    uint32_t expectedFrame;
  };
  std::deque<PendingReadback> pendingReadbacks;
  auto drainReadyReadbacks = [] { return true; };
  auto drainOldestReadback = [] { return true; };
  auto maybeReportFrameProgress = [](size_t, bool) {};
  long long bufferWaitMicros = 0, drawMicros = 0, frameAdvanceMicros = 0;
  long long readbackRequestMicros = 0, renderSubmitMicros = 0;
  std::vector<int> readbackTextures{1};
  int outputTexture = 2;
  size_t frameCount = 1, totalFrameCount = 1;
  uint32_t currentFrame = 0;
'''
    fixture += source[start:end]
    fixture += r'''
  const bool accepted = renderAndQueueFrame(0, 0, [&] {
    if (allocationFailure) rendering::rejectFrameSubmission();
    return complete;
  });
  if (complete && drawsAccepted && uiComplete && !allocationFailure) {
    require(accepted && blits == 1 && readbacks == 1 && pendingReadbacks.size() == 1,
            "complete frame was not queued exactly once");
    require(released == 0 && freeReadbackTextures.empty(), "queued readback storage recycled early");
  } else {
    require(!accepted && blits == 0 && readbacks == 0 && pendingReadbacks.empty(),
            "failed render was accepted for readback/encoding");
    require(acquired == released && freeReadbackTextures.size() == 1,
            "rejected frame retained encoder/readback reservation");
  }
}
'''
    start = end
fixture += r'''
struct PreparedGameplayBgaFrame {};
int failureStage = 0, stage = 0, liveLeases = 0;
struct Jukebox {
  void finalizePrepared(const PreparedGameplayBgaFrame &) { --liveLeases; }
  bool submitFullscreenComplete(const PreparedGameplayBgaFrame &frame) {
    finalizePrepared(frame);
    return ++stage != failureStage;
  }
};
namespace rendering {
struct BlurPass {
  bool prepareOutput() { return ++stage != failureStage; }
  bool executeComplete() { return ++stage != failureStage; }
  bool drawOutput(float brightness) {
    require(brightness == 0.5f, "export brightness changed");
    return ++stage != failureStage;
  }
};
}
'''
helper_start = source.index('bool submitReplayFullscreenBga(')
helper_end = source.index('\nbool waitForReplayExportForeground(', helper_start)
fixture += source[helper_start:helper_end]
fixture += r'''
void testFullscreenFailureStages() {
  for (failureStage = 0; failureStage <= 4; ++failureStage) {
    stage = 0; liveLeases = 1;
    Jukebox jukebox; rendering::BlurPass blur;
    std::string error;
    const bool complete = submitReplayFullscreenBga(jukebox, blur, {}, 0.5f, error);
    require(complete == (failureStage == 0), "fullscreen failure was ignored");
    require(stage == (failureStage == 0 ? 4 : failureStage), "fullscreen work continued after failure");
    require(liveLeases == 0, "fullscreen failure leaked or double-released the prepared lease");
    require(complete || !error.empty(), "fullscreen failure omitted its diagnostic");
  }
}
'''
fixture += r'''
int main() {
  testFullscreenFailureStages();
  testPath0(false); testPath1(false);
  drawsAccepted = false;
  testPath0(true); testPath1(true);
  drawsAccepted = true;
  uiComplete = false;
  testPath0(true); testPath1(true);
  uiComplete = true;
  allocationFailure = true;
  testPath0(true); testPath1(true);
  allocationFailure = false;
  testPath0(true); testPath1(true);
}
'''
compiler = FixtureCompiler.from_environment()
with tempfile.TemporaryDirectory(prefix='replay-export-completion-') as directory:
    cpp = Path(directory) / 'completion.cpp'
    binary = Path(directory) / ('completion' + compiler.executable_suffix)
    cpp.write_text(fixture)
    try:
        compiler.build([cpp], binary, directory)
    except subprocess.CalledProcessError as error:
        print(error.stderr)
        raise
    subprocess.run([str(binary)], check=True)
print('Replay export frame completion gates passed')
