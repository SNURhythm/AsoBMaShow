#include "settings/PresentationOrientationState.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>

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
void *s_window = nullptr;
uint32_t s_bgfxResetFlags = 0;
struct { void resize(int, int) {} } s_postProcess;
void SDL_GetWindowSize(void *, int *width, int *height) {
  *width = windowWidth;
  *height = windowHeight;
}
void getWindowDrawableSize(void *, int width, int height, int &rw, int &rh) {
  rw = width;
  rh = height;
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

  windowWidth = 0;
  assert(!syncDisplay(0, error));
  assert(sceneManager.changes == changesBeforeDuplicate);
  std::cout << "display orientation synchronization tests passed\n";
}
