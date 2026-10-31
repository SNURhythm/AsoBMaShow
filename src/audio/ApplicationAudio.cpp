#include "ApplicationAudio.h"

namespace audio {
ApplicationAudio::ApplicationAudio(Stopwatch *clock)
    : ApplicationAudio(clock, CreatePlatformBackendFactory()) {}

ApplicationAudio::ApplicationAudio(Stopwatch *clock, std::unique_ptr<IBackendFactory> factory)
    : output_(std::move(factory)),
      gameplay_(clock, output_.createSubmixFactory(0)),
      system_(&systemClock_, output_.createSubmixFactory(1)) {}

Capabilities ApplicationAudio::capabilities() const { return output_.capabilities(); }
RuntimeState ApplicationAudio::runtimeState() const { return output_.runtimeState(); }
std::optional<bool> ApplicationAudio::outputIsBluetooth(bool refresh) const {
  return output_.outputIsBluetooth(refresh);
}

bool ApplicationAudio::restart(const StreamRequest &request, std::string &error) {
  error.clear();
  // Exclude producers across the whole output transaction. A system play or
  // disposal cannot restart a node while either PCM store is being converted.
  std::scoped_lock lock(gameplay_.deviceLifecycleMutex, system_.deviceLifecycleMutex);
  for (auto *processor : {&gameplay_, &system_}) {
    const auto stopped = processor->suspendRenderingWithLifecycleLocked();
    if (!stopped.success) { error = stopped.diagnostic; return false; }
  }
  if (!output_.reopen(request, error)) return false;
  if (!gameplay_.restartWithLifecycleLocked(request, error) ||
      !system_.restartWithLifecycleLocked(request, error) ||
      !output_.start(error)) {
    // In particular, native start may synchronously invoke callbacks and then
    // report failure. Close both nodes before reporting a stopped transaction.
    (void)gameplay_.suspendRenderingWithLifecycleLocked();
    (void)system_.suspendRenderingWithLifecycleLocked();
    return false;
  }
  return true;
}

bool ApplicationAudio::restore(const RuntimeState &previous, std::string &error) {
  return restart(previous.request, error);
}
void ApplicationAudio::setVolumes(const Volumes &volumes) {
  gameplay_.setVolumes(volumes);
  // System sounds retain their per-source gain contract (including Lua's
  // system-volume multiplier), so master must not be multiplied twice here.
  system_.setVolumes(volumes);
}
void ApplicationAudio::setMonoAudio(bool enabled) {
  gameplay_.setMonoAudio(enabled);
  system_.setMonoAudio(enabled);
}
void ApplicationAudio::setApplicationSuspended(bool suspended) noexcept {
  gameplay_.setApplicationSuspended(suspended);
  system_.setApplicationSuspended(suspended);
}
}
