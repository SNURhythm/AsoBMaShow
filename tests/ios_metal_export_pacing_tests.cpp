#include "video/MetalDisplayLink.h"
#include "video/MetalDisplayLinkFrame.h"
#include "video/RendererAccessCoordinator.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>

namespace {
thread_local video::MetalDisplayLinkFrame pendingFrame;
display::RendererAccessCoordinator *coordinatorUnderTest = nullptr;
std::mutex *rendererMutex = nullptr;
void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
}
// Substitute the native run-loop boundary while retaining its real lease state.
// The coordinator must invoke it for synchronous as well as queued exports.
void video::MetalDisplayLink::pauseCurrent() {
  require(coordinatorUnderTest->exportRequested(), "publish request before detaching lease");
  require(rendererMutex->try_lock(), "detach before acquiring renderer ownership");
  rendererMutex->unlock();
  pendingFrame.finish();
}

int main() {
  std::mutex mutex;
  std::atomic_bool exporting{false};
  display::RendererAccessCoordinator coordinator(mutex, exporting);
  coordinatorUnderTest = &coordinator;
  rendererMutex = &mutex;
  pendingFrame.begin(1);
  require(pendingFrame.receive(10.0, 10.008), "scene owns a fresh display-link frame");
  {
    auto reservation = coordinator.acquireExport();
    require(!pendingFrame.managed() && !pendingFrame.claim(true, 1, true),
            "inline export must retire the scene drawable before taking the renderer");
    require(reservation.ownsLock() && exporting && coordinator.exportRequested(),
            "pausing presentation must preserve export ownership");
  }
  require(!exporting && !coordinator.exportRequested(), "export releases ownership normally");
  pendingFrame.begin(2);
  require(pendingFrame.receive(11.0, 11.008) && pendingFrame.claim(true, 2, true),
          "the next scene iteration can receive a fresh drawable after export");
}
