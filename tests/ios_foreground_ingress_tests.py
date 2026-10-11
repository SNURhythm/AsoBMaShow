"""Exercise the main-loop iOS resume sites across deferred frame restoration."""
from pathlib import Path
import subprocess
import tempfile

from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler

root = Path(__file__).resolve().parents[1]
source = (root / "src/main.cpp").read_text()
foreground_start = source.index("#if TARGET_OS_IPHONE\n      if (isAppForegroundEvent(event))")
foreground = extract(source[foreground_start:], "if (isAppForegroundEvent(event))")
early_signature = "if (!context.appInBackground.load() && !context.quitFlag.load())"
early = ""
if early_signature in source:
    start = source.index(early_signature)
    early = source[start:source.index("ResumeIOSGameplayTouchInput();", start) + len("ResumeIOSGameplayTouchInput();")]
late_signature = "if (renderedFrame && !iosPreparedFrame.pending()"
late = extract(source, late_signature) if late_signature in source else ""
resume_call = "ResumeIOSGameplayTouchInput("
assert sum(block.count(resume_call) for block in (foreground, early, late)) == source.count(resume_call), "uncovered native input resume site"
if late:
    assert source.index(late_signature) > source.index("sceneManager.update(deltaTime);"), "resume must follow scene geometry synchronization"

fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
std::uint64_t iosInputGeneration = 0;
struct { bool prepared = true; bool pending() const { return prepared; } } iosPreparedFrame;
bool iosViewportRestorePending = false, hasDeferredRenderResize = false;
bool renderedFrame = false, sceneUpdated = false, geometryRendered = false;
struct { std::atomic_bool appInBackground{false}, quitFlag{false}; } context;
bool nativeActive = true;
bool IOSApplicationActive() { return nativeActive; }
int resumes = 0;
void ResumeIOSGameplayTouchInput(std::uint64_t = 0) {
  assert(!iosPreparedFrame.pending() && !iosViewportRestorePending && !hasDeferredRenderResize);
  assert(sceneUpdated && geometryRendered && renderedFrame);
  ++resumes;
}
int main() {
  int event = 0;
  const auto isAppForegroundEvent = [](int) { return true; };
  const auto restoreIOSViewportAfterKeyboardFocus = [&] {
    if (iosPreparedFrame.pending()) {
      iosViewportRestorePending = true;
      return;
    }
    iosViewportRestorePending = false;
    hasDeferredRenderResize = true;
  };
  const auto drainForeground = [&] {
    PRODUCTION_FOREGROUND
    PRODUCTION_EARLY
  };
  const auto finishFrame = [&] {
    PRODUCTION_LATE
  };
  // Foreground and rotation can arrive while old draw commands are pending.
  drainForeground();
  finishFrame();
  assert(resumes == 0 && iosViewportRestorePending);
  sceneUpdated = geometryRendered = renderedFrame = true;
  finishFrame(); // Prepared commands still prevent admission.
  assert(resumes == 0);
  iosPreparedFrame.prepared = false; // Old frame has now been discarded.
  finishFrame(); // Viewport restoration is still pending.
  assert(resumes == 0);
  restoreIOSViewportAfterKeyboardFocus();
  finishFrame(); // A deferred/failed resize still blocks native input.
  assert(resumes == 0);
  hasDeferredRenderResize = false;
  renderedFrame = false;
  finishFrame();
  assert(resumes == 0);
  renderedFrame = true;
  finishFrame();
  assert(resumes == 1);
  context.appInBackground = true;
  finishFrame();
  context.appInBackground = false;
  context.quitFlag = true;
  finishFrame();
  context.quitFlag = false;
  nativeActive = false;
  finishFrame();
  assert(resumes == 1);
}
'''.replace("PRODUCTION_FOREGROUND", foreground).replace("PRODUCTION_EARLY", early).replace("PRODUCTION_LATE", late)
compiler = FixtureCompiler.from_environment()
with tempfile.TemporaryDirectory(prefix="ios-foreground-ingress-") as directory:
    cpp = Path(directory) / "foreground.cpp"
    binary = Path(directory) / ("foreground" + compiler.executable_suffix)
    cpp.write_text(fixture)
    try:
        compiler.build([cpp], binary, directory)
    except subprocess.CalledProcessError as error:
        print(error.stderr)
        raise
    subprocess.run([str(binary)], check=True)
print("iOS input waits for prepared-frame retirement and restored geometry")

# Reproduce a UIKit lifecycle pulse after the owner's event drain, and another
# pulse while its synchronous main-thread resume operation is being dispatched.
runtime_source = (root / "src/platform/IOSApplicationRuntime.mm").read_text()
resume = extract(runtime_source, "void ResumeIOSGameplayTouchInput(")
has_generation = "std::uint64_t expectedGeneration" in resume
call = "ResumeIOSGameplayTouchInput(ownerGeneration);" if has_generation else "ResumeIOSGameplayTouchInput();"
race_fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
struct Presentation { bool active = true; std::uint64_t generation = 0; };
struct Lifecycle {
  Presentation value;
  Presentation presentation() const { return value; }
  bool exporting() const { return false; }
};
struct Runtime { Lifecycle lifecycle; std::atomic_bool ingressPaused{true}; bool inputSuppressed = false; };
auto state = std::make_shared<Runtime>();
auto currentRuntime() { return state; }
bool enabled = false, interruptDuringDispatch = false;
void SetIOSGameplayTouchInputEnabled(bool value) { enabled = value; }
void RunIOSMainThread(std::function<void()> operation) {
  if (interruptDuringDispatch) ++state->lifecycle.value.generation;
  operation();
}
PRODUCTION_RESUME
int main() {
  std::uint64_t ownerGeneration = 0;
  // Owner drained generation 0, but UIKit completed background/resize/foreground
  // before scene update. The newly active state alone cannot acknowledge it.
  ++state->lifecycle.value.generation;
  PRODUCTION_CALL
  assert(!enabled && state->ingressPaused);
  ownerGeneration = 1; // Next owner pass consumes the published lifecycle events.
  PRODUCTION_CALL
  assert(enabled && !state->ingressPaused);
  enabled = false;
  state->ingressPaused = true;
  interruptDuringDispatch = true;
  PRODUCTION_CALL
  assert(!enabled && state->ingressPaused);
}
'''.replace("PRODUCTION_RESUME", resume).replace("PRODUCTION_CALL", call)
with tempfile.TemporaryDirectory(prefix="ios-ingress-generation-") as directory:
    cpp = Path(directory) / "generation.cpp"
    binary = Path(directory) / ("generation" + compiler.executable_suffix)
    cpp.write_text(race_fixture)
    compiler.build([cpp], binary, directory)
    subprocess.run([str(binary)], check=True)
if has_generation:
    # The owner must sample only a completed producer batch, before draining its
    # events. Sampling native state directly could precede event publication.
    sample = "const auto iosInputGeneration = GetIOSCompletedPumpLifecycleGeneration();"
    assert source.index(sample) < source.index("while (pollApplicationEvent(&e))")
    pump_start = runtime_source.index("const auto pumpGeneration = state->lifecycle.presentation().generation;")
    pump_end = runtime_source.index("state->completedPumpLifecycleGeneration.store(pumpGeneration, std::memory_order_release);")
    assert pump_start < runtime_source.index("while (SDL_PollEvent(&event))") < pump_end
    assert runtime_source.index("state->events.push(event") < pump_end
    assert "completedPumpLifecycleGeneration.load(std::memory_order_acquire)" in runtime_source
print("iOS input rejects lifecycle generations not consumed by the rendered owner pass")
