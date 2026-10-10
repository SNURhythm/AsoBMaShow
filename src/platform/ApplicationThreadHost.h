#pragma once

#include <atomic>
#include <cstdlib>
#include <exception>
#include <thread>
#include <utility>

namespace platform {
struct LaunchApplicationThread {
  template <typename F> std::thread operator()(F &&work) const {
    return std::thread(std::forward<F>(work));
  }
};

// Service native requests until worker unwinding and cleanup have finished.
// Joining earlier can deadlock a cleanup operation dispatched back to main.
// Service and reportFailure must not throw. Launch failure uses normal cleanup.
template <typename Application, typename Service, typename ReportFailure,
          typename Launch = LaunchApplicationThread>
int runApplicationThread(Application application, Service service,
                         ReportFailure reportFailure, Launch launch = {}) {
  std::atomic_bool complete{false};
  int result = EXIT_FAILURE;
  std::thread worker;
  try {
    worker = launch([&] {
      try { result = application(); }
      catch (...) { reportFailure(std::current_exception()); }
      complete.store(true, std::memory_order_release);
    });
  } catch (...) {
    reportFailure(std::current_exception());
    return EXIT_FAILURE;
  }
  while (!complete.load(std::memory_order_acquire)) service();
  worker.join();
  return result;
}
}
