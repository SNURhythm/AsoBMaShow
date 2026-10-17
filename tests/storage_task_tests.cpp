#include "../src/storage/StorageTask.h"
#include "StorageTestSupport.h"
#include <atomic>
#include <future>
#include <iostream>
#include <semaphore>

using namespace storage_management;
using storage_test::require;

int main() {
  try {
    // The handoff must preserve owned storage, not duplicate the confirmed tree.
    OptimizationRequest transfer;
    transfer.preflight.emplace();
    transfer.preflight->groups.resize(1);
    const auto *ownedGroups = transfer.preflight->groups.data();
    bool ownershipMoved = false;
    StorageTask moveTask({
      .admit = [](const OptimizationRequest &, std::string &) -> std::optional<StorageMaintenanceGuard> {
        return StorageMaintenanceGuard({}, {});
      },
      .run = [&](OptimizationRequest received, Checkpoint, ProgressCallback) {
        ownershipMoved = received.preflight && received.preflight->groups.data() == ownedGroups;
        return StorageReport{};
      },
    });
    require(moveTask.Start(std::move(transfer)), "owned preflight starts");
    moveTask.CancelAndWait();
    require(ownershipMoved, "preflightOwnershipMovesWithoutAssetCopies");
    std::atomic<int> admitted{0}, released{0}, refreshed{0};
    std::binary_semaphore entered{0};
    std::counting_semaphore<16> finish{0};
    StorageTask task({
      .admit = [&](const OptimizationRequest &, std::string &) -> std::optional<StorageMaintenanceGuard> {
        ++admitted;
        return StorageMaintenanceGuard([&](const StorageReport &) {
          require(released == 0, "reservation lasts through refresh");
          ++refreshed;
        }, [&] { ++released; });
      },
      .run = [&](const OptimizationRequest &, Checkpoint checkpoint, ProgressCallback progress) {
        for (int i = 0; i < 10000; ++i) progress({.filesDone = static_cast<std::uint64_t>(i)});
        entered.release();
        finish.acquire();
        StorageReport report;
        report.cancelled = !checkpoint();
        report.reclaimedBytes = 42; // a committed file survives later cancellation
        return report;
      },
      .wake = [&] { finish.release(); },
    });
    auto first = std::async(std::launch::async, [&] { return task.Start({}); });
    auto second = std::async(std::launch::async, [&] { return task.Start({}); });
    require(first.get() != second.get(), "simultaneous starts admit exactly one job");
    entered.acquire();
    require(admitted == 1 && released == 0, "one guard retained");
    require(task.Snapshot().progress.filesDone == 9999, "progress handoff stores latest value");
    task.RequestCancel();
    auto report = task.CancelAndWait();
    require(report && report->cancelled && report->reclaimedBytes == 42, "cancel preserves committed savings");
    require(refreshed == 1 && released == 1 && !task.Snapshot().running, "completion refreshes then releases");
    require(!task.TakeCompletion(), "completion consumed once");
    task.Shutdown();
    require(!task.Start({}), "shutdown rejects restart");

    int live = 0;
    {
      StorageTask fatal({
        .admit = [&](const OptimizationRequest &, std::string &) -> std::optional<StorageMaintenanceGuard> {
          ++live;
          return StorageMaintenanceGuard({}, [&] { --live; });
        },
        .run = [](const OptimizationRequest &, Checkpoint, ProgressCallback) {
          StorageReport report; report.recoveryRequired = true; return report;
        },
      });
      require(fatal.Start({}), "fatal job starts");
      require(fatal.CancelAndWait()->recoveryRequired, "fatal recovery reported");
      require(live == 1 && !fatal.Start({}), "fatal recovery keeps admission closed");
    }
    require(live == 0, "shutdown releases after worker joins");

    int shutdownRelease = 0;
    std::binary_semaphore started{0};
    std::counting_semaphore<16> wake{0};
    {
      StorageTask owned({
        .admit = [&](const OptimizationRequest &, std::string &) -> std::optional<StorageMaintenanceGuard> {
          return StorageMaintenanceGuard({}, [&] { ++shutdownRelease; });
        },
        .run = [&](const OptimizationRequest &, Checkpoint check, ProgressCallback) {
          started.release(); wake.acquire();
          require(!check(), "destruction cancels worker before releasing owner");
          require(shutdownRelease == 0, "owner alive until worker returns");
          return StorageReport{};
        },
        .wake = [&] { wake.release(); },
      });
      require(owned.Start({}), "owned worker starts");
      started.acquire();
    }
    require(shutdownRelease == 1, "destruction joins before release");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
