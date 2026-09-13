#pragma once

#include "../ThreadCompat.h"
#include "../archive/TemporaryCacheTypes.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>

// Application-thread admission and consumption; workers publish data only.
// Dependencies supplied through operations must outlive this owner. Cleanup
// intentionally has no stop token: cancellation suppresses its completion but
// lets protected filesystem cleanup finish. Measurement is cooperatively stopped.
// Operation exceptions are delivered through the same failed completion as errors.
class SettingsCacheMaintenance final {
public:
  enum class Operation { Cleanup, Measure };
  struct Completion {
    Operation operation = Operation::Measure;
    bool succeeded = false;
    archive_file::TemporaryCacheCleanupResult cleanup;
    archive_file::TemporaryCacheUsageResult usage;
    std::string error;
  };
  using Cleanup = std::function<bool(archive_file::TemporaryCacheCleanupResult &,
                                     std::string &)>;
  using Measure = std::function<bool(archive_file::TemporaryCacheUsageResult &,
                                     std::string &, const std::stop_token &)>;

  SettingsCacheMaintenance(Cleanup cleanup, Measure measure);
  ~SettingsCacheMaintenance();
  SettingsCacheMaintenance(const SettingsCacheMaintenance &) = delete;
  SettingsCacheMaintenance &operator=(const SettingsCacheMaintenance &) = delete;

  bool startCleanup();
  bool startMeasure();
  bool cleanupRunning() const;
  bool running() const;
  std::optional<Completion> takeCompletion();
  // Joins cleanup first, then stops/joins measurement, matching scene teardown.
  // May be called repeatedly; later admission is allowed for scene reuse.
  void stopAndWait();

private:
  bool start(Operation operation);
  void run(Operation operation, std::uint64_t generation,
           const std::stop_token &token);

  Cleanup cleanup_;
  Measure measure_;
  mutable std::mutex mutex_;
  bool cleanupRunning_ = false;
  bool measureRunning_ = false;
  std::uint64_t generation_ = 0;
  std::optional<Completion> completion_;
  // Joined before operation captures, synchronization, or completion storage die.
  std::jthread cleanupThread_;
  std::jthread measureThread_;
};
