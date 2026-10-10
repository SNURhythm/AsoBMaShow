"""Exercise the actual inactive main-loop branches without native windows."""
from pathlib import Path
import subprocess
import tempfile

from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler

root = Path(__file__).resolve().parents[1]
source = (root / "src/main.cpp").read_text()
wait_start = source.index("auto waitForBackgroundEvent = [&]()")
# The two preprocessor wait branches intentionally share one closing brace.
wait = source[wait_start:source.index("\n    };", wait_start) + len("\n    }")]
pending_start = source.index("if (iosPreparedFrame.pending()) {\n      // Keep handling")
pending = extract(source[pending_start:], "if (iosPreparedFrame.pending())")
background_start = source.index("if (context.appInBackground.load", pending_start)
background = extract(source[background_start:], "if (context.appInBackground.load")
export_recovery = extract(source, "if (ranApplicationWork)")
assert source.index("if (ranApplicationWork)") < source.index("while (pollApplicationEvent(&e))")
fixture = r'''
#include <atomic>
#include <cassert>
constexpr bool ASOBMASHOW_ENABLE_PERF_TELEMETRY = true;
constexpr int kBackgroundEventWaitTimeoutMs = 100;
struct SDL_Event {};
int waited = 0;
bool deliverEvent = false;
bool WaitIOSApplicationEvent(SDL_Event *, int timeout) {
  waited = timeout;
  return deliverEvent;
}
namespace platform {
bool waitApplicationEvent(SDL_Event *event, int timeout) {
  return WaitIOSApplicationEvent(event, timeout);
}
}
bool IOSApplicationActive() { return false; }
struct Scene {
  bool playing = true;
  int ticks = 0, cancellations = 0;
  void onInputQueueOverflow() { ++cancellations; }
  bool continuesAudioInBackground() const { return playing; }
  void updateWhileBackgrounded() { ++ticks; }
};
int main() {
  Scene scene;
  struct { Scene *currentScene; } sceneManager{&scene};
  struct {
    struct Registry {
      int events = 0, pumps = 0, clears = 0;
      void clearSdlInputState() { ++clears; }
      void handleSdlEventAndDispatch(SDL_Event &) { ++events; }
      void pump() { ++pumps; }
    } inputDeviceRegistry;
    std::atomic_bool appInBackground{true}, replayVideoExportActive{false};
    int currentFrame = 0;
  } context;
  struct { bool pending() const { return true; } } iosPreparedFrame;
  int rawEventsInWindow = 0, processed = 0, foregroundUpdates = 0;
  const auto processEvent = [&](SDL_Event) { ++processed; };
  bool ranApplicationWork = false;
  const auto recoverExportInput = [&] { PRODUCTION_EXPORT_RECOVERY };
  recoverExportInput();
  assert(scene.cancellations == 0 && context.inputDeviceRegistry.clears == 0);
  ranApplicationWork = true;
  recoverExportInput();
  assert(scene.cancellations == 1 && context.inputDeviceRegistry.clears == 1 && scene.playing);
  sceneManager.currentScene = nullptr;
  recoverExportInput();
  assert(context.inputDeviceRegistry.clears == 2);
  sceneManager.currentScene = &scene;
  PRODUCTION_WAIT;
  for (int frame = 0; frame < 1; ++frame) {
    PRODUCTION_BACKGROUND
    ++foregroundUpdates;
  }
  assert(scene.ticks == 1 && waited == 16 && foregroundUpdates == 0);
  assert(context.inputDeviceRegistry.pumps == 1 && context.currentFrame == 1);
  scene.playing = false; // Explicit pause keeps the gameplay hook idle.
  waitForBackgroundEvent();
  assert(scene.ticks == 1 && waited == kBackgroundEventWaitTimeoutMs);
  sceneManager.currentScene = nullptr;
  waitForBackgroundEvent();
  assert(waited == kBackgroundEventWaitTimeoutMs);
  sceneManager.currentScene = &scene;
  scene.playing = true;
  deliverEvent = true;
  waitForBackgroundEvent();
  assert(scene.ticks == 2 && rawEventsInWindow == 1 && processed == 1);
  assert(context.inputDeviceRegistry.events == 1);
#if TARGET_OS_IPHONE
  for (int frame = 0; frame < 1; ++frame) {
    PRODUCTION_PENDING
    ++foregroundUpdates;
  }
  assert(scene.ticks == 3 && foregroundUpdates == 0);
  assert(context.inputDeviceRegistry.pumps == 2);
#endif
}
'''.replace("PRODUCTION_EXPORT_RECOVERY", export_recovery).replace("PRODUCTION_WAIT", wait).replace("PRODUCTION_BACKGROUND", background).replace("PRODUCTION_PENDING", pending)
compiler = FixtureCompiler.from_environment()
with tempfile.TemporaryDirectory(prefix="background-gameplay-dispatch-") as directory:
    for name, ios, android in (("desktop", 0, 0), ("ios", 1, 0), ("android", 0, 1)):
        cpp = Path(directory) / f"{name}.cpp"
        binary = Path(directory) / (name + compiler.executable_suffix)
        cpp.write_text(f"#define TARGET_OS_IPHONE {ios}\n#define TARGET_OS_ANDROID {android}\n" + fixture)
        try:
            compiler.build([cpp], binary, directory)
        except subprocess.CalledProcessError as error:
            print(error.stderr)
            raise
        subprocess.run([str(binary)], check=True)
print("Background gameplay dispatch passed on desktop, iOS, and Android")
