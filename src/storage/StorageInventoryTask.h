#pragma once
#include "ChartResourceInventory.h"
#include <memory>
#include <mutex>
#include <thread>

namespace storage_management {
// Read-only work owned by the browser. No view, scene or application pointers
// cross into the worker; cancelling joins before retiring its immutable result.
class StorageInventoryTask {
public:
  using Builder = std::function<StorageInventory(const InventoryRequest &, Checkpoint)>;
  explicit StorageInventoryTask(Builder builder = {});
  ~StorageInventoryTask();
  void Start(InventoryRequest);
  void Cancel();
  std::optional<StorageInventory> TakeResult();
private:
  struct State {
    std::mutex mutex;
    std::optional<StorageInventory> result;
  };
  Builder builder_;
  std::shared_ptr<State> state_;
  std::jthread worker_;
};
}
