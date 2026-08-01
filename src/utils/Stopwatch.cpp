//
// Created by XF on 9/2/2024.
//

#include "Stopwatch.h"

int64_t Stopwatch::nowMicrosInternal() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Stopwatch::start() {
  bool expected = false;
  if (running.compare_exchange_strong(expected, true,
                                      std::memory_order_acq_rel)) {
    start_time_us.store(nowMicrosInternal(), std::memory_order_release);
  }
}

void Stopwatch::pause() {
  if (running.exchange(false, std::memory_order_acq_rel)) {
    const int64_t pauseTimeUs = nowMicrosInternal();
    const int64_t startTimeUs = start_time_us.load(std::memory_order_acquire);
    if (pauseTimeUs > startTimeUs) {
      elapsed_time_us.fetch_add(pauseTimeUs - startTimeUs,
                                std::memory_order_acq_rel);
    }
  }
}

void Stopwatch::resume() {
  start();
}

void Stopwatch::reset() {
  elapsed_time_us.store(0, std::memory_order_release);
  start_time_us.store(nowMicrosInternal(), std::memory_order_release);
  running.store(false, std::memory_order_release);
}

long long Stopwatch::elapsedMicros() const {
  const int64_t elapsedUs = elapsed_time_us.load(std::memory_order_acquire);
  if (running.load(std::memory_order_acquire)) {
    const int64_t currentTimeUs = nowMicrosInternal();
    const int64_t startTimeUs = start_time_us.load(std::memory_order_acquire);
    if (currentTimeUs > startTimeUs) {
      return elapsedUs + (currentTimeUs - startTimeUs);
    }
  }
  return elapsedUs;
}

void Stopwatch::seek(long long int micro) {
  elapsed_time_us.store(micro, std::memory_order_release);
  start_time_us.store(nowMicrosInternal(), std::memory_order_release);
}

bool Stopwatch::isRunning() const {
  return running.load(std::memory_order_acquire);
}
