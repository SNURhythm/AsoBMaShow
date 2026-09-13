#include "ThreadCompat.h"

#include <cassert>
#include <chrono>
#include <future>

using namespace std::chrono_literals;

namespace {
struct Lifetime {
  std::atomic_bool workerFinished{false};
  std::atomic_bool dependenciesAlive{true};
};

struct WorkerDependencies {
  Lifetime &lifetime;
  ~WorkerDependencies() {
    // The production status fields follow the worker in SettingsScene.h, so
    // implicit jthread destruction cannot protect them from a late worker.
    assert(lifetime.workerFinished.load());
    lifetime.dependenciesAlive = false;
  }
};

struct NoopOwner {
  void stopAndWait() {}
  void reset() {}
};

class SettingsScene {
public:
  explicit SettingsScene(Lifetime &lifetime) : dependencies{lifetime} {}
  ~SettingsScene();
  struct {
    struct { SettingsScene *scene = nullptr; } profileSwitchBlockers;
  } context;
  // Match the production declaration order: worker before callback state.
  std::jthread difficultyTableJobThread;
  WorkerDependencies dependencies;
  NoopOwner archiveCacheMaintenance;
  NoopOwner inputProfileReplacementRegistration;
  void stopProfileArchiveWork() {}
};

#include "settings_library_lifecycle_methods.inc"

void testDirectDestructionJoinsBeforeCallbackDependenciesDie() {
  Lifetime lifetime;
  std::promise<void> entered, stopped, release;
  auto released = release.get_future().share();
  auto scene = std::make_unique<SettingsScene>(lifetime);
  scene->difficultyTableJobThread = std::jthread([&](std::stop_token token) {
    entered.set_value();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    assert(token.stop_requested());
    stopped.set_value();
    assert(released.wait_for(5s) == std::future_status::ready);
    // Import/database operations may finish after stop is requested.
    assert(lifetime.dependenciesAlive.load());
    lifetime.workerFinished = true;
  });
  assert(entered.get_future().wait_for(5s) == std::future_status::ready);
  auto destroyed = std::async(std::launch::async, [&] { scene.reset(); });
  assert(stopped.get_future().wait_for(5s) == std::future_status::ready);
  assert(lifetime.dependenciesAlive.load());
  assert(destroyed.wait_for(50ms) == std::future_status::timeout);
  release.set_value();
  assert(destroyed.wait_for(5s) == std::future_status::ready);
  destroyed.get();
  assert(lifetime.workerFinished && !lifetime.dependenciesAlive);
}

void testIdleAndPreviouslyJoinedDestruction() {
  Lifetime idle;
  idle.workerFinished = true;
  { SettingsScene scene(idle); }
  assert(!idle.dependenciesAlive);

  Lifetime finished;
  {
    SettingsScene scene(finished);
    scene.difficultyTableJobThread = std::jthread([&] { finished.workerFinished = true; });
    scene.difficultyTableJobThread.join();
  }
  assert(!finished.dependenciesAlive);
}
} // namespace

int main() {
  testDirectDestructionJoinsBeforeCallbackDependenciesDie();
  testIdleAndPreviouslyJoinedDestruction();
}
