#!/usr/bin/env python3
"""Exercise the production device constructor/destructor at the miniaudio boundary."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from support.fixture_compiler import FixtureCompiler

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class IOSAudioSessionHandoffTests(unittest.TestCase):
    def test_retiring_preview_keeps_retained_gameplay_session_active(self):
        source = (ROOT / "src/audio/AudioBackend.cpp").read_text()
        constructor = method(source, "  MiniaudioStream(const StreamRequest &request,")
        destructor = method(source, "  ~MiniaudioStream() override").replace(" override", "")
        fixture = r'''
#include "audio/AudioBackend.h"
#include <cassert>
#include <memory>
#include <string>
#undef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 1
#define TARGET_OS_ANDROID 0
constexpr int MA_TRUE = 1, MA_SUCCESS = 0, ma_format_s16 = 1;
constexpr int ma_device_type_playback = 1, ma_backend_coreaudio = 1;
constexpr int ma_ios_session_category_ambient = 1;
constexpr int ma_ios_session_category_option_mix_with_others = 1;
constexpr int kOutputChannels = 2;
using ma_result = int;
using ma_bool32 = int;
using ma_uint32 = unsigned;
struct ma_device;
struct ma_device_config {
  struct { int format = 0, channels = 0; } playback;
  int sampleRate = 0, periodSizeInFrames = 0, noFixedSizedCallback = 0;
  void (*dataCallback)(ma_device *, void *, const void *, unsigned) = nullptr;
  void *pUserData = nullptr;
};
struct ma_context_config {
  struct { int sessionCategory = 0, sessionCategoryOptions = 0;
           bool noAudioSessionDeactivate = false; } coreaudio;
};
struct ma_context { int backend = ma_backend_coreaudio; bool noDeactivate = false; };
struct ma_device {
  ma_context *pContext = nullptr;
  struct { void (*outputBoundaryCallback)(ma_device *, int, unsigned) = nullptr; } coreaudio;
  unsigned sampleRate = 48000;
};
bool sharedSessionActive = false;
ma_device_config ma_device_config_init(int) { return {}; }
ma_context_config ma_context_config_init() { return {}; }
int ma_device_init_ex(void *, int, const ma_context_config *context,
                      const ma_device_config *, ma_device *device) {
  device->pContext = new ma_context;
  device->pContext->noDeactivate = context->coreaudio.noAudioSessionDeactivate;
  sharedSessionActive = true;
  return MA_SUCCESS;
}
// miniaudio.h's iOS context teardown calls setActive(false) unless this exact
// context option is set. Keep that native boundary effect in the double.
void ma_device_uninit(ma_device *device) {
  if (!device->pContext->noDeactivate) sharedSessionActive = false;
  delete device->pContext;
}
const char *ma_result_description(int) { return "failure"; }
using audio::StreamRequest;
using audio::RenderCallback;
class MiniaudioStream {
public:
CONSTRUCTOR
DESTRUCTOR
private:
  static void dataCallback(ma_device *, void *, const void *, unsigned) {}
  static void outputBoundary(ma_device *, int, unsigned) {}
  StreamRequest request_;
  RenderCallback renderCallback_;
  void *renderUserData_;
  ma_device device_{};
  audio::RuntimeState state_;
  bool initialized_ = false;
};
int main() {
  std::string error;
  MiniaudioStream gameplay({}, nullptr, nullptr, error);
  {
    MiniaudioStream preview({}, nullptr, nullptr, error);
    assert(sharedSessionActive);
  }
  assert(sharedSessionActive && "retiring preview must preserve retained gameplay session");
}
'''.replace("CONSTRUCTOR", constructor).replace("DESTRUCTOR", destructor)
        compiler = FixtureCompiler.from_environment()
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            cpp = folder / "handoff.cpp"
            executable = folder / ("handoff" + compiler.executable_suffix)
            cpp.write_text(fixture)
            compiler.build([cpp], executable, folder, includes=[ROOT / "src"])
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
