#pragma once
#include "StorageMeasurement.h"
#include "StoragePreflight.h"
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace storage_management {
enum class StorageInventoryMode { Measurements, Preflight };
struct StorageInventoryTaskResult {
  StorageInventoryMode mode = StorageInventoryMode::Measurements;
  bool complete = false, cancelled = false;
  std::optional<StoragePreflight> preflight;
};
struct StorageInventoryTaskBuilders {
  std::function<StorageMeasurements(const InventoryRequest &, Checkpoint, MeasurementCallback, std::stop_token)> measurements;
  std::function<StoragePreflight(const InventoryRequest &, Checkpoint, std::stop_token)> preflight;
  std::function<std::chrono::steady_clock::time_point()> now;
  std::function<void()> wake;
};
// The browser owns a single generation. Replacing it cancels and joins before
// retiring its queues; workers never capture view or application pointers.
class StorageInventoryTask {
public:
  explicit StorageInventoryTask(StorageInventoryTaskBuilders = {});
  ~StorageInventoryTask();
  void StartMeasurements(InventoryRequest);
  void StartPreflight(InventoryRequest);
  void Cancel();
  std::vector<GroupStorageMeasurement> TakeMeasurements();
  std::optional<StorageInventoryTaskResult> TakeResult();
private:
  void Start(InventoryRequest, StorageInventoryMode);
  struct State {
    std::mutex mutex;
    std::map<std::string, GroupStorageMeasurement> measurements;
    std::optional<StorageInventoryTaskResult> result;
    std::optional<std::chrono::steady_clock::time_point> published, notified;
    bool finished = false;
    std::uint64_t generation = 0;
  };
  StorageInventoryTaskBuilders builders_;
  std::shared_ptr<State> state_;
  std::jthread worker_;
  std::uint64_t generation_ = 0;
};
}
