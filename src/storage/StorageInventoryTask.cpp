#include "StorageInventoryTask.h"
#include <utility>
#include "../ui/PresentationSchedule.h"
namespace storage_management {
StorageInventoryTask::StorageInventoryTask(Builder builder) : builder_(std::move(builder)) {
  if (!builder_) builder_ = [](const InventoryRequest &request, Checkpoint checkpoint) {
    return BuildInventory(request, std::move(checkpoint));
  };
}
StorageInventoryTask::~StorageInventoryTask() { Cancel(); }
void StorageInventoryTask::Cancel() {
  worker_.request_stop();
  if (worker_.joinable()) worker_.join();
  state_.reset();
}
void StorageInventoryTask::Start(InventoryRequest request) {
  Cancel();
  state_ = std::make_shared<State>();
  worker_ = std::jthread([state = state_, build = builder_, request = std::move(request)](std::stop_token stop) {
    StorageInventory result;
    try { result = build(request, [stop] { return !stop.stop_requested(); }); }
    catch (...) {
      result.request = request;
      result.complete = false;
      for (const auto &group : request.groups)
        result.groups.push_back({.groupId = group.groupId, .root = group.path,
            .reason = {StorageReason::IoError, "Could not measure folder"}});
    }
    if (stop.stop_requested()) return;
    {
      std::lock_guard lock(state->mutex);
      state->result = std::move(result);
    }
    ui::notifyCompletion();
  });
}
std::optional<StorageInventory> StorageInventoryTask::TakeResult() {
  if (!state_) return {};
  std::lock_guard lock(state_->mutex);
  return std::exchange(state_->result, {});
}
}
