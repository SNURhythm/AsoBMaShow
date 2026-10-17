#include "../src/storage/StorageInventoryTask.h"
#include <cassert>
#include <condition_variable>
using namespace storage_management;
int main() {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool exited = false;
  StorageInventoryTask task([&](const InventoryRequest &request, Checkpoint check) {
    {
      std::lock_guard lock(mutex);
      entered = true;
    }
    changed.notify_one();
    while (check()) std::this_thread::yield();
    exited = true;
    StorageInventory result;
    result.request = request;
    return result;
  });
  InventoryRequest request;
  request.policy.audio = AudioAction::LossyVorbis;
  task.Start(request);
  {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return entered; });
  }
  task.Cancel();
  assert(exited);
  assert(!task.TakeResult()); // A cancelled/replaced scene never sees stale evidence.
  StorageInventoryTask failing([](const InventoryRequest &, Checkpoint) -> StorageInventory {
    throw std::runtime_error("unreadable");
  });
  request.groups.push_back({"folder", "/unreadable"});
  failing.Start(request);
  std::optional<StorageInventory> result;
  while (!(result = failing.TakeResult())) std::this_thread::yield();
  assert(!result->complete && !result->totalBytes);
  assert(result->groups.size() == 1 && !result->groups[0].totalBytes);
  assert(result->groups[0].reason.code == StorageReason::IoError);
  assert(result->request.policy.audio == AudioAction::LossyVorbis);
}
