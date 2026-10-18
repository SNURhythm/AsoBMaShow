#include "StorageInventoryTask.h"
#include "../ui/PresentationSchedule.h"
#include <set>
#include <utility>
namespace storage_management {
StorageInventoryTask::StorageInventoryTask(StorageInventoryTaskBuilders builders) : builders_(std::move(builders)) {
  if (!builders_.measurements) builders_.measurements = [](const auto &request, auto check, auto publish, auto stop) {
    StorageScanDependencies deps; deps.limits.stopToken = stop; deps.limits.scratchRoot = request.scanScratchRoot;
    return MeasureStorageGroups(request, std::move(check), std::move(publish), nullptr, std::move(deps));
  };
  if (!builders_.preflight) builders_.preflight = [](const auto &request, auto check, auto stop) {
    StorageScanDependencies deps; deps.limits.stopToken = stop; deps.limits.scratchRoot = request.scanScratchRoot;
    return BuildStoragePreflight(request, std::move(check), nullptr, std::move(deps));
  };
  if (!builders_.now) builders_.now = [] { return std::chrono::steady_clock::now(); };
  if (!builders_.wake) builders_.wake = [] { ui::notifyCompletion(); };
}
StorageInventoryTask::~StorageInventoryTask() { Cancel(); }
void StorageInventoryTask::Cancel() {
  worker_.request_stop();
  if (worker_.joinable()) worker_.join();
  state_.reset();
}
void StorageInventoryTask::Start(InventoryRequest request, StorageInventoryMode mode) {
  Cancel(); ++generation_;
  std::shared_ptr<InventoryRequest> pending;
  try {
    state_ = std::make_shared<State>(); state_->generation = generation_;
    // Retain the selected groups if allocation or thread construction fails
    // after moving the request into the worker's startup state.
    pending = std::make_shared<InventoryRequest>(std::move(request));
    worker_ = std::jthread([state = state_, builders = builders_, pending, mode](std::stop_token stop) {
    const auto &request = *pending;
    std::set<std::string> groups;
    for (const auto &group : request.groups) groups.insert(group.groupId);
    const auto check = [stop] { return !stop.stop_requested(); };
    const auto queue = [&](const std::vector<GroupStorageMeasurement> &rows, bool notify) {
      if (stop.stop_requested() || rows.empty()) return;
      bool wake = false;
      {
        std::lock_guard lock(state->mutex);
        for (const auto &row : rows) if (groups.contains(row.groupId)) {
          auto &measurement = state->measurements[row.groupId]; measurement = row; measurement.generation = state->generation;
        }
        const auto now = builders.now();
        if (notify && (!state->notified || now - *state->notified >= std::chrono::milliseconds(100))) {
          state->notified = now; wake = true;
        }
      }
      if (wake) builders.wake();
    };
    const auto publish = [&](const std::vector<GroupStorageMeasurement> &rows) { queue(rows, true); };
    StorageInventoryTaskResult result{.mode = mode};
    try {
      if (mode == StorageInventoryMode::Measurements) {
        auto measured = builders.measurements(request, check, publish, stop);
        queue(measured.groups, false); result.complete = measured.complete; result.cancelled = measured.cancelled;
      } else {
        auto preflight = builders.preflight(request, check, stop);
        for (auto &group : preflight.groups) group.generation = state->generation;
        // Review evidence must not replace the browser's cached size provenance.
        result.complete = preflight.complete; result.cancelled = preflight.cancelled;
        result.preflight = std::move(preflight);
      }
    } catch (...) {
      std::vector<GroupStorageMeasurement> failed;
      for (const auto &group : request.groups) failed.push_back({.groupId = group.groupId, .root = group.path,
          .reason = {StorageReason::IoError, "Could not measure folder"}, .sourceKind = group.sourceKind});
      queue(failed, false);
    }
    if (stop.stop_requested()) return;
    {
      std::lock_guard lock(state->mutex); state->finished = true; state->result = std::move(result);
    }
    builders.wake();
    });
  } catch (...) {
    if (!state_) { state_ = std::make_shared<State>(); state_->generation = generation_; }
    const auto &failed = pending ? *pending : request;
    {
      std::lock_guard lock(state_->mutex);
      for (const auto &group : failed.groups) state_->measurements[group.groupId] = {
          .groupId = group.groupId, .root = group.path,
          .reason = {StorageReason::IoError, "Could not start folder scan. Try again."},
          .sourceKind = group.sourceKind, .generation = state_->generation};
      state_->result = StorageInventoryTaskResult{.mode = mode}; state_->finished = true;
    }
    builders_.wake();
  }
}
void StorageInventoryTask::StartMeasurements(InventoryRequest request) { Start(std::move(request), StorageInventoryMode::Measurements); }
void StorageInventoryTask::StartPreflight(InventoryRequest request) { Start(std::move(request), StorageInventoryMode::Preflight); }
std::vector<GroupStorageMeasurement> StorageInventoryTask::TakeMeasurements() {
  std::vector<GroupStorageMeasurement> rows;
  if (!state_) return rows;
  std::lock_guard lock(state_->mutex);
  const auto now = builders_.now();
  if (!state_->finished && state_->published && now - *state_->published < std::chrono::milliseconds(100)) return rows;
  if (state_->measurements.empty()) return rows;
  rows.reserve(state_->measurements.size());
  for (auto &[id, row] : state_->measurements) rows.push_back(std::move(row));
  state_->measurements.clear(); state_->published = now;
  return rows;
}
std::optional<StorageInventoryTaskResult> StorageInventoryTask::TakeResult() {
  if (!state_) return {};
  std::lock_guard lock(state_->mutex); return std::exchange(state_->result, {});
}
}
