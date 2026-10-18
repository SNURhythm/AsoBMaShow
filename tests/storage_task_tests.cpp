#include "../src/storage/StorageTask.h"
#include "../src/storage/StorageRecoveryPresentation.h"
#include "StorageTestSupport.h"
#include <atomic>
#include <future>
#include <iostream>
#include <semaphore>

using namespace storage_management;
using storage_test::require;

namespace {
void startupRecoveryKeepsPresentationAlive() {
  const auto owner = std::this_thread::get_id();
  std::atomic_bool entered = false, completed = false;
  std::binary_semaphore phasePresented{0};
  bool firstFrameBeforeWork = false, onOwner = true, released = false;
  unsigned frames = 0;
  const bool recovered = RunStorageRecoveryPresentation([&](auto notify) {
    entered = true;
    StorageRecoveryProgress progress{.phase = StorageRecoveryPhase::CheckingArchive,
        .currentPath = "library/songs.zip"};
    notify(progress);
    // A large burst must not enqueue thousands of display messages, while a
    // new phase must bypass throttling before its next long I/O operation.
    for (unsigned i = 0; i < 20000; ++i) { progress.completedOperations = i; notify(progress); }
    progress.phase = StorageRecoveryPhase::FinalizingArchive;
    progress.archiveBytesDone = 123456;
    notify(progress);
    require(phasePresented.try_acquire_for(std::chrono::seconds(2)),
        "presentation continues while recovery is blocked on an operation");
    completed = true;
    return true;
  }, [&](const StorageRecoveryProgress &progress) {
    if (++frames == 1) firstFrameBeforeWork = !entered;
    onOwner &= std::this_thread::get_id() == owner;
    if (!released && progress.phase == StorageRecoveryPhase::FinalizingArchive &&
        progress.completedOperations == 19999 && progress.archiveBytesDone == 123456) {
      released = true;
      phasePresented.release();
    }
    throw std::runtime_error("unavailable progress display");
  });
  require(recovered && completed && firstFrameBeforeWork && onOwner && released && frames > 1,
      "startup displays first, preserves renderer ownership, ignores display errors and joins recovery");
  require(!RunStorageRecoveryPresentation([](auto) { return false; }, [](const auto &) {}),
      "failed recovery never becomes successful through its presentation loop");
  bool propagated = false;
  try {
    RunStorageRecoveryPresentation([](auto) -> bool { throw std::runtime_error("recovery failed"); },
        [](const auto &) { throw std::runtime_error("display failed"); });
  } catch (const std::runtime_error &error) { propagated = std::string(error.what()) == "recovery failed"; }
  require(propagated, "recovery exception reaches the startup failure gate after joining");
}
}

int main() {
  try {
    startupRecoveryKeepsPresentationAlive();
    // The handoff must preserve owned storage, not duplicate the confirmed tree.
    OptimizationRequest transfer;
    transfer.preflight.emplace();
    transfer.preflight->groups.resize(1);
    transfer.preflight->eligibleCount = 42;
    const auto *ownedGroups = transfer.preflight->groups.data();
    bool ownershipMoved = false;
    bool rejectAdmission = true;
    StorageTask moveTask({
      .admit = [&](const OptimizationRequest &, std::string &error) -> std::optional<StorageMaintenanceGuard> {
        if (rejectAdmission) { error = "busy"; return {}; }
        return StorageMaintenanceGuard({}, {});
      },
      .run = [&](OptimizationRequest received, Checkpoint, ProgressCallback) {
        ownershipMoved = received.preflight && received.preflight->groups.data() == ownedGroups;
        return StorageReport{};
      },
    });
    require(!moveTask.Start(std::move(transfer)), "busy admission rejects work");
    require(transfer.preflight && transfer.preflight->groups.data() == ownedGroups,
            "rejected admission retains reviewed preflight for retry");
    require(moveTask.Snapshot().admissionError == "busy", "admission failure remains visible");
    rejectAdmission = false;
    require(moveTask.Start(std::move(transfer)), "owned preflight starts on retry");
    require(moveTask.Snapshot().progress.phase == StoragePhase::Prepare &&
        moveTask.Snapshot().progress.filesTotal == 42, "confirmed work starts with preparation and reviewed totals");
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

    int refreshFailureLive = 0;
    StorageTask refreshFailure({
      .admit = [&](const OptimizationRequest &, std::string &) -> std::optional<StorageMaintenanceGuard> {
        ++refreshFailureLive;
        return StorageMaintenanceGuard([](const StorageReport &) {
          throw std::runtime_error("refresh queue unavailable");
        }, [&] { --refreshFailureLive; });
      },
      .run = [](const OptimizationRequest &, Checkpoint, ProgressCallback) {
        StorageReport report;
        report.converted = 1;
        report.reclaimedBytes = 42;
        return report;
      },
    });
    require(refreshFailure.Start({}), "job with failing completion refresh starts");
    auto refreshFailureReport = refreshFailure.CancelAndWait();
    require(refreshFailureReport && !refreshFailureReport->recoveryRequired,
            "refresh failure after settled transactions does not require recovery");
    require(refreshFailureReport->converted == 1 && refreshFailureReport->reclaimedBytes == 42,
            "refresh failure preserves committed results");
    require(refreshFailureReport->failed == 1 && !refreshFailureReport->files.empty() &&
        refreshFailureReport->files.front().outcome == FileOutcome::Failed &&
        refreshFailureReport->files.front().reason.code == StorageReason::IoError &&
        refreshFailureReport->files.front().reason.detail.find("refresh") != std::string::npos,
        "refresh failure retains a visible diagnostic");
    const auto refreshFailureSnapshot = refreshFailure.Snapshot();
    require(!refreshFailureSnapshot.running && !refreshFailureSnapshot.recoveryRequired &&
        refreshFailureSnapshot.lastReport && refreshFailureSnapshot.lastReport->failed == 1,
        "refresh failure snapshot completes without requiring recovery");
    require(refreshFailureLive == 0 && refreshFailure.Start({}),
            "refresh failure releases admission and permits a later job");
    refreshFailure.CancelAndWait();

    int live = 0;
    {
      StorageTask fatal({
        .admit = [&](const OptimizationRequest &, std::string &) -> std::optional<StorageMaintenanceGuard> {
          ++live;
          return StorageMaintenanceGuard([](const StorageReport &) {
            throw std::runtime_error("completion failed during recovery");
          }, [&] { --live; });
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
