#include "settings/PresentationOrientationState.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>

using player_settings::PresentationOrientation;

namespace rendering {
int render_width = 1080, render_height = 2400;
float widthScale = 1, heightScale = 1;
void updateUIScale(int width, int height) {
  render_width = width;
  render_height = height;
}
}
namespace bgfx {
int resets = 0;
void reset(int, int, uint32_t) { ++resets; }
}
struct SceneManager {
  PresentationOrientation orientation = PresentationOrientation::Portrait;
  int changes = 0;
  void setPresentationOrientation(PresentationOrientation next) {
    if (orientation != next) {
      orientation = next;
      ++changes;
    }
  }
};
struct Context {
  SceneManager *sceneManager;
  std::atomic_bool replayVideoExportActive{false};
  std::atomic_uint32_t bgfxResetFlags{0};
  std::mutex bgfxRenderMutex;
  struct {
    void reset(std::chrono::steady_clock::time_point) {}
  } framePacer;
  void restoreGameplayRenderViews() {}
};
int windowWidth = 2400, windowHeight = 1080;
bool iosApplicationActive = true;
bool IOSApplicationActive() { return iosApplicationActive; }
void *s_window = nullptr;
uint32_t s_bgfxResetFlags = 0;
struct { void resize(int, int) {} } s_postProcess;
namespace platform {
void windowSize(void *, int *width, int *height) {
  *width = windowWidth;
  *height = windowHeight;
}
}
void getWindowDrawableSize(void *, int width, int height, int &rw, int &rh) {
  rw = width;
  rh = height;
}
void getIOSMetalDrawableSize(void *window, int width, int height, int &rw, int &rh) {
  getWindowDrawableSize(window, width, height, rw, rh);
}
#define APP_DEBUG_LOG(...) ((void)0)

int main() {
  player_settings::PresentationOrientationState presentationOrientation;
  presentationOrientation.updateViewport(1080, 2400);
  SceneManager sceneManager;
  Context context{.sceneManager = &sceneManager};
  uint32_t activeBgfxResetFlags = 0;
  auto syncDisplay = [&](uint32_t resetFlags, std::string &syncError)
      PRODUCTION_DISPLAY_SYNC;
  PRODUCTION_RESIZE;

  // The OS rotates after scene initialization but before startup VSync is
  // applied. That transaction sees the new geometry before the resize event.
  std::string error;
  assert(syncDisplay(128, error));
  assert(rendering::render_width == 2400);
  assert(sceneManager.orientation == PresentationOrientation::Landscape &&
         "startup display transaction must select the landscape skin profile");
  assert(applyWindowResize(2400, 1080));
  const int changesBeforeStart = sceneManager.changes;
  presentationOrientation.updateViewport(windowWidth, windowHeight);
  presentationOrientation.setGameplayLocked(true);
  sceneManager.setPresentationOrientation(presentationOrientation.orientation());
  assert(sceneManager.changes == changesBeforeStart &&
         "first Start must not invalidate the already prepared skin orientation");

  // A display transaction while gameplay owns orientation may resize the
  // drawable, but the skin profile stays locked until returning to the menu.
  windowWidth = 1080;
  windowHeight = 2400;
  assert(syncDisplay(0, error));
  assert(sceneManager.orientation == PresentationOrientation::Landscape);
  presentationOrientation.setGameplayLocked(false);
  sceneManager.setPresentationOrientation(presentationOrientation.orientation());
  assert(sceneManager.orientation == PresentationOrientation::Portrait);

  // Already synchronized drawable dimensions must not suppress an outstanding
  // orientation update (for example after another renderer reset path).
  rendering::updateUIScale(2400, 1080);
  const int resetsBeforeResize = bgfx::resets;
  assert(applyWindowResize(2400, 1080));
  assert(sceneManager.orientation == PresentationOrientation::Landscape &&
         "same-size resize must still synchronize the active skin orientation");
  assert(bgfx::resets == resetsBeforeResize);
  const int changesBeforeDuplicate = sceneManager.changes;
  assert(applyWindowResize(2400, 1080));
  assert(sceneManager.changes == changesBeforeDuplicate);

  // Fold/unfold may change size without changing portrait/landscape. Resize
  // must still reset rendering and refresh presentation geometry in that case.
  const int resetsBeforeFold = bgfx::resets;
  for (const auto [width, height] : {std::pair{420, 900}, std::pair{840, 900},
                                    std::pair{900, 840}, std::pair{420, 900}}) {
    assert(applyWindowResize(width, height));
    assert(rendering::render_width == width && rendering::render_height == height);
    assert(sceneManager.orientation == (width > height
        ? PresentationOrientation::Landscape : PresentationOrientation::Portrait));
  }
  assert(bgfx::resets == resetsBeforeFold + 4);
#if TARGET_OS_IPHONE
  iosApplicationActive = false;
  const int resetsBeforeBackground = bgfx::resets;
  assert(!applyWindowResize(640, 480));
  assert(bgfx::resets == resetsBeforeBackground);
  iosApplicationActive = true;
  assert(applyWindowResize(640, 480));
  assert(sceneManager.orientation == PresentationOrientation::Landscape);
#endif
  const int changesAfterFold = sceneManager.changes;
  windowWidth = 0;
  assert(!syncDisplay(0, error));
  assert(sceneManager.changes == changesAfterFold);
  std::cout << "display orientation synchronization tests passed\n";
}
