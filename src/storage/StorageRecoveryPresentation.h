#pragma once

#include "StorageRecoveryProgress.h"
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace storage_management {
// The caller retains renderer ownership. Recovery has one joined worker and
// one latest-value mailbox, never a growing queue of progress notifications.
template <class Recover, class Present>
bool RunStorageRecoveryPresentation(Recover recover, Present present) {
  std::mutex mutex;
  std::condition_variable wake;
  StorageRecoveryProgress latest;
  bool done = false, recovered = false;
  std::exception_ptr failure;
  auto show = [&](const StorageRecoveryProgress &value) noexcept {
    try { present(value); } catch (...) {}
  };
  show(latest);
  std::thread worker([&] {
    auto nextUpdate = std::chrono::steady_clock::time_point{};
    auto lastPhase = StorageRecoveryPhase::CheckingFolders;
    try {
      recovered = recover([&](const StorageRecoveryProgress &value) {
        const auto now = std::chrono::steady_clock::now();
        if (now < nextUpdate && value.phase == lastPhase) return;
        nextUpdate = now + std::chrono::milliseconds(200);
        lastPhase = value.phase;
        const std::lock_guard lock(mutex);
        latest = value;
      });
    } catch (...) { failure = std::current_exception(); }
    {
      const std::lock_guard lock(mutex);
      done = true;
    }
    wake.notify_one();
  });
  // Even a presentation failure must join recovery before its state or the
  // application context can be destroyed. It must not cancel durable work.
  try {
    std::unique_lock lock(mutex);
    while (!done) {
      const auto snapshot = latest;
      lock.unlock();
      show(snapshot);
      lock.lock();
      wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return done; });
    }
  } catch (...) {
    worker.join();
    if (failure) std::rethrow_exception(failure);
    return recovered;
  }
  worker.join();
  if (failure) std::rethrow_exception(failure);
  return recovered;
}
}
