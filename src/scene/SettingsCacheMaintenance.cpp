#include "SettingsCacheMaintenance.h"

#include <exception>
#include <utility>

SettingsCacheMaintenance::SettingsCacheMaintenance(Cleanup cleanup, Measure measure)
    : cleanup_(std::move(cleanup)), measure_(std::move(measure)) {}

SettingsCacheMaintenance::~SettingsCacheMaintenance() { stopAndWait(); }

bool SettingsCacheMaintenance::startCleanup() { return start(Operation::Cleanup); }
bool SettingsCacheMaintenance::startMeasure() { return start(Operation::Measure); }

bool SettingsCacheMaintenance::start(Operation operation) {
  const bool cleanup = operation == Operation::Cleanup;
  std::uint64_t generation;
  {
    std::lock_guard lock(mutex_);
    if (cleanupRunning_ || (!cleanup && measureRunning_)) {
      return false;
    }
    (cleanup ? cleanupRunning_ : measureRunning_) = true;
    generation = ++generation_;
    completion_.reset();
  }

  auto &thread = cleanup ? cleanupThread_ : measureThread_;
  if (thread.joinable()) {
    thread.join();
  }
  try {
    thread = std::jthread([this, operation, generation](const std::stop_token &token) {
      run(operation, generation, token);
    });
  } catch (...) {
    std::lock_guard lock(mutex_);
    (cleanup ? cleanupRunning_ : measureRunning_) = false;
    throw;
  }
  return true;
}

void SettingsCacheMaintenance::run(Operation operation, std::uint64_t generation,
                                   const std::stop_token &token) {
  Completion result;
  result.operation = operation;
  const bool cleanup = operation == Operation::Cleanup;
  try {
    result.succeeded = cleanup ? cleanup_(result.cleanup, result.error)
                               : measure_(result.usage, result.error, token);
  } catch (const std::exception &error) {
    result.error = error.what();
    if (result.error.empty()) result.error = "Unknown archive cache error";
  } catch (...) {
    result.error = "Unknown archive cache error";
  }
  std::lock_guard lock(mutex_);
  (cleanup ? cleanupRunning_ : measureRunning_) = false;
  if (!token.stop_requested() && generation == generation_) {
    completion_ = std::move(result);
  }
}

bool SettingsCacheMaintenance::cleanupRunning() const {
  std::lock_guard lock(mutex_);
  return cleanupRunning_;
}

bool SettingsCacheMaintenance::running() const {
  std::lock_guard lock(mutex_);
  return cleanupRunning_ || measureRunning_;
}

std::optional<SettingsCacheMaintenance::Completion>
SettingsCacheMaintenance::takeCompletion() {
  std::lock_guard lock(mutex_);
  return std::exchange(completion_, std::nullopt);
}

void SettingsCacheMaintenance::stopAndWait() {
  if (cleanupThread_.joinable()) {
    cleanupThread_.request_stop();
    cleanupThread_.join();
  }
  if (measureThread_.joinable()) {
    measureThread_.request_stop();
    measureThread_.join();
  }
  std::lock_guard lock(mutex_);
  completion_.reset();
}
