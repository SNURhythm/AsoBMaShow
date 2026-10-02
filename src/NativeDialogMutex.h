#pragma once

#include <mutex>

namespace platform_native_dialog {

inline std::timed_mutex &operationMutex() {
  // Native desktop pickers share result buffers and cannot be forcibly
  // dismissed. Keep their shared gate alive for detached workers at shutdown.
  static auto *mutex = new std::timed_mutex();
  return *mutex;
}

} // namespace platform_native_dialog
