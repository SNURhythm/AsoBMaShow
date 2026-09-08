#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using Uint64 = std::uint64_t;
Uint64 SDL_GetTicks64() { return 1; }
struct SDL_Event {};
struct ApplicationContext { Uint64 currentFrame = 0; int uiBatchRenderer = 0; };
struct RenderContext {
  explicit RenderContext(int) {}
  struct UiBatchScope { explicit UiBatchScope(RenderContext &) {} };
};
struct View {
  bool handleEvents(SDL_Event &) { return true; }
  void render(RenderContext &) {}
};

SCENE_HEADER

struct TestScene : Scene {
  explicit TestScene(ApplicationContext &context) : Scene(context) {}
  void init() override {}
  void update(float) override {}
  void renderScene() override {}
  void cleanupScene() override {}
  void submit(std::function<bool()> callback) { SUBMIT_CALLBACK; }
};

int main() {
  ApplicationContext context;
  TestScene scene(context);
  const auto uiThread = std::this_thread::get_id();
  int completed = 0;
  auto callback = [&] {
    assert(std::this_thread::get_id() == uiThread);
    ++completed;
    return true;
  };
  std::jthread worker([&] { scene.submit(callback); });
  worker.join();
  assert(scene.deferred.empty() && "worker must not access the UI-owned deferred map");
  scene.handleDeferred();
  ++context.currentFrame;
  scene.handleDeferred();
  assert(completed == 1);
  std::vector<std::jthread> workers;
  for (int index = 0; index < 4; ++index) {
    workers.emplace_back([&] {
      for (int count = 0; count < 500; ++count) scene.submit(callback);
    });
  }
  for (int frame = 0; frame < 100; ++frame) {
    ++context.currentFrame;
    scene.handleDeferred();
  }
  workers.clear();
  scene.handleDeferred();
  ++context.currentFrame;
  scene.handleDeferred();
  assert(completed == 2001);
  scene.submit(callback);
  scene.cleanup();
  scene.prepareForUse();
  ++context.currentFrame;
  scene.handleDeferred();
  ++context.currentFrame;
  scene.handleDeferred();
  assert(completed == 2001 && "teardown must discard pending worker callbacks");
}
