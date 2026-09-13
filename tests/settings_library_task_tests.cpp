#include "scene/SettingsLibraryTask.h"

#include <cassert>
#include <chrono>
#include <future>

using namespace std::chrono_literals;
using Task = SettingsLibraryTask;

namespace {
struct Gate {
  std::promise<void> entered, release;
  std::shared_future<void> released = release.get_future().share();
  void block() {
    entered.set_value();
    assert(released.wait_for(5s) == std::future_status::ready);
  }
  void wait() { assert(entered.get_future().wait_for(5s) == std::future_status::ready); }
};

void waitIdle(Task &task) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (task.running() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  assert(!task.running());
}

void testExclusiveAdmissionAndProgressHandoff() {
  Task task;
  Gate gate;
  const auto caller = std::this_thread::get_id();
  assert(task.start([&](const auto &, const Task::Publisher &updates) {
    assert(std::this_thread::get_id() != caller);
    updates.importProgress({1, 3, "First", "Downloading", false, false, {}});
    updates.importProgress({2, 3, "Second", "Downloading", false, false, {}});
    updates.tableStatus("Table added.", true, true);
    updates.tableStatus("Follow-up detail", false);
    updates.folderStatus("Folder removed.", true);
    gate.block();
    updates.importProgress({3, 3, "Third", "Table added.", true, true, "submitted-url"});
  }));
  gate.wait();
  assert(task.running());
  assert(!task.start([](const auto &, const auto &) { assert(false); }));
  auto pending = task.takeUpdates();
  assert(pending.tableStatus && pending.tableStatus->text == "Follow-up detail");
  assert(!pending.tableStatus->succeeded && pending.reload);
  assert(pending.folderStatus && pending.folderStatus->succeeded);
  assert(pending.importProgress && pending.importProgress->current == 2);
  assert(pending.importProgress->tableName == "Second" && !pending.importProgress->finished);
  auto empty = task.takeUpdates();
  assert(!empty.tableStatus && !empty.folderStatus && !empty.importProgress && !empty.reload);
  gate.release.set_value();
  waitIdle(task);
  pending = task.takeUpdates();
  assert(pending.importProgress && pending.importProgress->finished);
  assert(pending.importProgress->succeeded && pending.importProgress->submittedUrl == "submitted-url");
}

void testCompletedUpdatesSurviveAdmissionUntilConsumed() {
  Task task;
  assert(task.start([](const auto &, const Task::Publisher &updates) {
    updates.folderStatus("Refresh failed.", false, true);
  }));
  waitIdle(task);
  Gate gate;
  assert(task.start([&](const auto &, const auto &) { gate.block(); }));
  gate.wait();
  const auto pending = task.takeUpdates();
  assert(pending.folderStatus && pending.folderStatus->text == "Refresh failed.");
  assert(pending.reload);
  gate.release.set_value();
  task.stopAndWait();
}

void testStopJoinsUncancellableWorkAndDiscardsLateAndQueuedUpdates() {
  Task task;
  Gate gate;
  std::promise<void> stopped;
  std::atomic_bool finished{false};
  assert(task.start([&](const auto &token, const Task::Publisher &updates) {
    updates.tableStatus("Queued", true, true);
    gate.entered.set_value();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    assert(token.stop_requested());
    stopped.set_value();
    assert(gate.released.wait_for(5s) == std::future_status::ready);
    updates.tableStatus("Late", true, true);
    updates.folderStatus("Late", true, true);
    updates.importProgress({1, 1, "Late", "Late", true, true, "old-url"});
    finished = true;
  }));
  gate.wait();
  auto stop = std::async(std::launch::async, [&] { task.stopAndWait(); });
  assert(stopped.get_future().wait_for(5s) == std::future_status::ready);
  assert(stop.wait_for(50ms) == std::future_status::timeout);
  gate.release.set_value();
  assert(stop.wait_for(5s) == std::future_status::ready);
  stop.get();
  assert(finished && !task.running());
  const auto pending = task.takeUpdates();
  assert(!pending.tableStatus && !pending.folderStatus && !pending.importProgress && !pending.reload);
  task.stopAndWait();
  assert(task.start([](const auto &, const Task::Publisher &updates) {
    updates.tableStatus("New", true);
  }));
  waitIdle(task);
  assert(task.takeUpdates().tableStatus->text == "New");
}

void testOwnerDestructionJoinsBeforeWorkCapturesAreReleased() {
  auto task = std::make_unique<Task>();
  auto resource = std::make_shared<int>(42);
  std::weak_ptr<int> observed = resource;
  Gate gate;
  assert(task->start([resource, &gate](const auto &, const Task::Publisher &updates) {
    gate.block();
    assert(*resource == 42);
    updates.tableStatus("Finished", true);
  }));
  gate.wait();
  resource.reset();
  auto destroy = std::async(std::launch::async, [&] { task.reset(); });
  assert(destroy.wait_for(50ms) == std::future_status::timeout);
  assert(!observed.expired());
  gate.release.set_value();
  assert(destroy.wait_for(5s) == std::future_status::ready);
  destroy.get();
  assert(observed.expired());
}
} // namespace

#include "settings_library_scene_fixture.h"

int main() {
  testExclusiveAdmissionAndProgressHandoff();
  testCompletedUpdatesSurviveAdmissionUntilConsumed();
  testStopJoinsUncancellableWorkAndDiscardsLateAndQueuedUpdates();
  testOwnerDestructionJoinsBeforeWorkCapturesAreReleased();
  testSceneImportProgressAndSuccessfulUrlCompletion();
  testScenePreservesEditedUrlAndReportsDatabaseFailure();
  testSceneTableUpdateDeletionConfirmationAndAbsentViews();
}
