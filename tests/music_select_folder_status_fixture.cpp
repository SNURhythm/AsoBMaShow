#include "REPOSITORY_ROOT/src/music_select/MusicSelectFolderStatusLoader.h"
#include "REPOSITORY_ROOT/src/music_select/MusicSelectBarManager.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <stdexcept>

using namespace std::chrono_literals;

MusicSelectBar folder(const std::string &name) {
  return {.id = {name}, .kind = skin::MusicSelectBarKind::Folder,
          .title = name, .presentation = {.kind = skin::MusicSelectBarKind::Folder},
          .selectable = true};
}

struct Gate {
  std::mutex mutex;
  std::condition_variable_any changed;
  int starts = 0;
  int cancellations = 0;
  bool released = false;
  void block(std::stop_token stop) {
    std::unique_lock lock(mutex);
    ++starts;
    changed.notify_all();
    changed.wait(lock, stop, [&] { return released; });
    if (stop.stop_requested()) ++cancellations;
    changed.notify_all();
  }
  void waitStarts(int expected) {
    std::unique_lock lock(mutex);
    assert(changed.wait_for(lock, 1s, [&] { return starts >= expected; }));
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }
};

template <typename Loader>
bool request(Loader &loader, const std::vector<MusicSelectBar> &bars,
             MusicSelectFolderStatusLoader::Processor process,
             MusicSelectBarId priority = {}, int mode = 1) {
  if constexpr (requires { loader.request(bars, "ALL", mode, process, priority); }) {
    return loader.request(bars, "ALL", mode, std::move(process), priority);
  } else {
    return loader.request(bars, "ALL", mode, std::move(process));
  }
}

template <typename Loader>
void prioritize(Loader &loader, const MusicSelectBarId &id) {
  if constexpr (requires { loader.prioritize(id); }) loader.prioritize(id);
}

std::vector<MusicSelectFolderStatusLoader::Result>
collect(MusicSelectFolderStatusLoader &loader, std::size_t count) {
  std::vector<MusicSelectFolderStatusLoader::Result> results;
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (results.size() < count && std::chrono::steady_clock::now() < deadline) {
    auto batch = loader.takeResults();
    results.insert(results.end(), batch.begin(), batch.end());
    std::this_thread::yield();
  }
  assert(results.size() == count);
  return results;
}

skin::MusicSelectBarFrame status(const MusicSelectBar &bar, int count = 3) {
  auto frame = bar.presentation;
  frame.folderLampCounts[6] = count;
  frame.folderRankCounts[20] = count;
  frame.lamp = 6;
  return frame;
}

void testInitialPriorityAndCompletedResults() {
  MusicSelectFolderStatusLoader loader;
  std::atomic_int calls = 0;
  const std::vector bars{folder("A"), folder("B"), folder("C")};
  auto process = [&](const MusicSelectBar &bar, std::stop_token) {
    ++calls;
    return status(bar);
  };
  assert(request(loader, bars, process, {"C"}));
  const auto results = collect(loader, 3);
  assert(results.front().id.value == "C" && "selected folder must aggregate first");
  prioritize(loader, {"A"});
  assert(!request(loader, bars, process, {"B"}));
  assert(calls == 3 && loader.takeResults().empty());
  assert(request(loader, {bars[2]}, process, {"C"}));
  assert(loader.takeResults().empty() && calls == 3);
  assert(request(loader, {bars[2]}, process, {"C"}, 2));
  assert(collect(loader, 1).front().id.value == "C" && calls == 4);
}

void testSelectionPreemptsUnrelatedWorkWithoutPublishingPartialStats() {
  MusicSelectFolderStatusLoader loader;
  Gate first;
  Gate selected;
  assert(request(loader, {folder("A"), folder("C")},
      [&](const MusicSelectBar &bar, std::stop_token stop) {
        (bar.id.value == "A" ? first : selected).block(stop);
        return status(bar, stop.stop_requested() ? 99 : 3);
      }, {"A"}));
  first.waitStarts(1);
  prioritize(loader, {"C"});
  selected.waitStarts(1);
  assert(loader.takeResults().empty());
  selected.release();
  assert(collect(loader, 1).front().frame.folderLampCounts[6] == 3);
  first.waitStarts(2);
  first.release();
  assert(collect(loader, 1).front().id.value == "A");
  assert(first.cancellations == 1);
}

struct Scene {
  std::unique_ptr<MusicSelectFolderStatusLoader> folderStatusLoader_;
  std::optional<std::uint64_t> folderStatusRowsRevision_;
  std::optional<std::chrono::steady_clock::time_point> folderStatusRetryAt_;
  MusicSelectFolderStatusLoader::Processor process;
  void requestFolderStatus(const MusicSelectBarManagerReadView &snapshot) {
    SCENE_REQUEST_PREFIX
    const auto selected = snapshot.selectedIndex < snapshot.rows.size()
        ? snapshot.rows[snapshot.selectedIndex].id : MusicSelectBarId{};
    request(*folderStatusLoader_, directories, process, selected);
  }
};

void testSceneSelectionReprioritizesWithoutChangingRows() {
  MusicSelectProjection projection;
  projection.root = {{"A"}, {"C"}};
  projection.bars = {folder("A"), folder("C")};
  MusicSelectBarManager manager(std::move(projection));
  Gate first;
  Gate selected;
  Scene scene;
  scene.process = [&](const MusicSelectBar &bar, std::stop_token stop) {
    (bar.id.value == "A" ? first : selected).block(stop);
    return status(bar);
  };
  scene.requestFolderStatus(manager.readView());
  first.waitStarts(1);
  const auto revision = manager.readView().rowsRevision;
  assert(manager.select({"C"}));
  assert(manager.readView().rowsRevision == revision);
  scene.requestFolderStatus(manager.readView());
  selected.waitStarts(1);
  selected.release();
  const auto result = collect(*scene.folderStatusLoader_, 1).front();
  assert(result.id.value == "C");
  manager.installFolderStatus(result.id, result.frame);
  assert(manager.songListFrame().at(manager.readView().selectedIndex)
             .folderLampCounts[6] == 3);
  first.release();
}

void testNavigationPreservesActiveParentAndCompletedTotals() {
  auto parent = folder("Parent");
  parent.children = {{"Song"}};
  MusicSelectProjection projection;
  projection.root = {parent.id};
  projection.bars = {parent, {.id = {"Song"}, .kind = skin::MusicSelectBarKind::Song,
      .presentation = {.kind = skin::MusicSelectBarKind::Song}}};
  MusicSelectBarManager manager(std::move(projection));
  Gate gate;
  Scene scene;
  scene.process = [&](const MusicSelectBar &bar, std::stop_token stop) {
    gate.block(stop);
    return status(bar, stop.stop_requested() ? 99 : 3);
  };
  scene.requestFolderStatus(manager.readView());
  gate.waitStarts(1);
  assert(manager.open(parent.id));
  scene.requestFolderStatus(manager.readView());
  assert(manager.close());
  scene.requestFolderStatus(manager.readView());
  gate.release();
  const auto result = collect(*scene.folderStatusLoader_, 1).front();
  assert(result.id == parent.id && result.frame.folderLampCounts[6] == 3);
  assert(gate.starts == 1 && gate.cancellations == 0);
  manager.installFolderStatus(result.id, result.frame);
  assert(manager.open(parent.id));
  scene.requestFolderStatus(manager.readView());
  assert(manager.readView().directoryBars.back().presentation.folderLampCounts[6] == 3);
  assert(manager.close());
  scene.requestFolderStatus(manager.readView());
  assert(manager.songListFrame().at(0).folderLampCounts[6] == 3 && gate.starts == 1);
}

void testCancelRejectsAnOldCompletedCallback() {
  MusicSelectFolderStatusLoader loader;
  Gate gate;
  assert(request(loader, {folder("A")}, [&](const MusicSelectBar &bar, std::stop_token stop) {
    gate.block(stop);
    return status(bar, 99);
  }));
  gate.waitStarts(1);
  loader.cancel();
  assert(request(loader, {folder("A")}, [](const MusicSelectBar &bar, std::stop_token) {
    return status(bar);
  }));
  const auto result = collect(loader, 1).front();
  assert(result.frame.folderLampCounts[6] == 3);
}

void testFailuresRetryWithoutRepeatingCompletedFolders() {
  MusicSelectFolderStatusLoader loader;
  std::atomic_int completedCalls = 0;
  std::atomic_int failingCalls = 0;
  const std::vector bars{folder("A"), folder("B")};
  auto process = [&](const MusicSelectBar &bar, std::stop_token) {
    if (bar.id.value == "A") ++completedCalls;
    else if (++failingCalls <= 2) throw std::runtime_error("temporary read failure");
    return status(bar);
  };
  assert(request(loader, bars, process));
  const auto failed = collect(loader, 2);
  assert(failed.front().error.empty() && !failed.back().error.empty());
  assert(!request(loader, bars, process, {"B"}));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!loader.retryReady() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  assert(loader.retryReady() && request(loader, bars, process, {"B"}));
  const auto recovered = collect(loader, 1).front();
  assert(recovered.id.value == "B" && recovered.error.empty());
  assert(completedCalls == 1 && failingCalls == 3);
}

void testReturningToCancellingSelectionRestartsItFirst() {
  MusicSelectFolderStatusLoader loader;
  Gate active;
  Gate cancellation;
  std::atomic_int firstCalls = 0;
  assert(request(loader, {folder("A"), folder("C")},
      [&](const MusicSelectBar &bar, std::stop_token stop) {
        if (bar.id.value == "A" && ++firstCalls == 1) {
          active.block(stop);
          cancellation.block({});
        }
        return status(bar);
      }, {"A"}));
  active.waitStarts(1);
  prioritize(loader, {"C"});
  cancellation.waitStarts(1);
  prioritize(loader, {"A"});
  cancellation.release();
  const auto results = collect(loader, 2);
  assert(results.front().id.value == "A" && firstCalls == 2);
}

void testEmptySceneMembershipCancelsRemovedFolders() {
  MusicSelectProjection projection;
  projection.root = {{"A"}};
  projection.bars = {folder("A")};
  MusicSelectBarManager manager(std::move(projection));
  Gate active;
  Gate cancellation;
  std::atomic_int calls = 0;
  Scene scene;
  scene.process = [&](const MusicSelectBar &bar, std::stop_token stop) {
    if (++calls == 1) {
      active.block(stop);
      cancellation.block({});
    }
    return status(bar, stop.stop_requested() ? 99 : 3);
  };
  scene.requestFolderStatus(manager.readView());
  active.waitStarts(1);
  MusicSelectBarManager empty;
  auto emptyView = empty.readView();
  emptyView.rowsRevision = manager.readView().rowsRevision + 1;
  scene.requestFolderStatus(emptyView);
  cancellation.waitStarts(1);
  cancellation.release();
  scene.requestFolderStatus(manager.readView());
  const auto result = collect(*scene.folderStatusLoader_, 1).front();
  assert(calls == 2 && result.frame.folderLampCounts[6] == 3);
}

int main() {
  testInitialPriorityAndCompletedResults();
  testSelectionPreemptsUnrelatedWorkWithoutPublishingPartialStats();
  testSceneSelectionReprioritizesWithoutChangingRows();
  testNavigationPreservesActiveParentAndCompletedTotals();
  testCancelRejectsAnOldCompletedCallback();
  testFailuresRetryWithoutRepeatingCompletedFolders();
  testReturningToCancellingSelectionRestartsItFirst();
  testEmptySceneMembershipCancelsRemovedFolders();
}
