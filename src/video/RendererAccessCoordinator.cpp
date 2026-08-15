#include "RendererAccessCoordinator.h"

#include <utility>

namespace display {
RendererAccessCoordinator::DisplayReservation::DisplayReservation(
    std::unique_lock<std::mutex> lockValue)
    : lock(std::move(lockValue)) {}

bool RendererAccessCoordinator::DisplayReservation::ownsLock() const {
  return lock.owns_lock();
}

RendererAccessCoordinator::ExportReservation::ExportReservation(
    std::mutex &rendererMutex, std::atomic<bool> &exportActiveValue,
    std::atomic_size_t &requestedExportsValue,
    std::size_t &activeExportsValue)
    : lock(rendererMutex), exportActive(&exportActiveValue),
      requestedExports(&requestedExportsValue),
      activeExports(&activeExportsValue) {
  ++*activeExports;
  exportActive->store(true, std::memory_order_release);
}

RendererAccessCoordinator::ExportReservation::ExportReservation(
    ExportReservation &&other) noexcept
    : lock(std::move(other.lock)),
      exportActive(std::exchange(other.exportActive, nullptr)),
      requestedExports(std::exchange(other.requestedExports, nullptr)),
      activeExports(std::exchange(other.activeExports, nullptr)),
      released(std::exchange(other.released, true)) {}

RendererAccessCoordinator::ExportReservation::~ExportReservation() {
  release();
}

void RendererAccessCoordinator::ExportReservation::unlockForUiFrame() {
  if (!released && lock.owns_lock()) {
    lock.unlock();
  }
}

void RendererAccessCoordinator::ExportReservation::relockAfterUiFrame() {
  if (!released && !lock.owns_lock()) {
    lock.lock();
  }
}

void RendererAccessCoordinator::ExportReservation::release() {
  if (released || exportActive == nullptr || requestedExports == nullptr ||
      activeExports == nullptr) {
    return;
  }
  if (!lock.owns_lock()) {
    lock.lock();
  }
  if (*activeExports > 0) {
    --*activeExports;
  }
  exportActive->store(*activeExports != 0, std::memory_order_release);
  requestedExports->fetch_sub(1, std::memory_order_release);
  lock.unlock();
  released = true;
}

bool RendererAccessCoordinator::ExportReservation::ownsLock() const {
  return lock.owns_lock();
}

RendererAccessCoordinator::RendererAccessCoordinator(
    std::mutex &rendererMutexValue, std::atomic<bool> &exportActiveValue)
    : rendererMutex(rendererMutexValue), exportActive(exportActiveValue) {}

std::optional<RendererAccessCoordinator::DisplayReservation>
RendererAccessCoordinator::tryAcquireDisplay(std::string &errorMessage) {
  std::unique_lock<std::mutex> lock(rendererMutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    errorMessage = "The renderer is busy with another transaction.";
    return std::nullopt;
  }
  if (activeExports != 0) {
    errorMessage = "Replay export is active.";
    return std::nullopt;
  }
  return DisplayReservation(std::move(lock));
}

RendererAccessCoordinator::ExportReservation
RendererAccessCoordinator::acquireExport() {
  // Publish the request before waiting for the renderer mutex. The main
  // renderer observes this handoff and yields its next frame, preventing an
  // export worker from being starved behind consecutive display frames.
  requestedExports.fetch_add(1, std::memory_order_acq_rel);
  return ExportReservation(rendererMutex, exportActive, requestedExports,
                           activeExports);
}

bool RendererAccessCoordinator::exportRequested() const noexcept {
  return requestedExports.load(std::memory_order_acquire) != 0;
}
} // namespace display
