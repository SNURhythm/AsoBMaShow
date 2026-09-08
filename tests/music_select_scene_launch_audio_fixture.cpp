#include <atomic>
#include <cassert>
#include <memory>

struct Chart {};
struct Jukebox {
  struct Result { bool success; };
  bool success = true;
  void stop() {}
  Result loadChart(Chart &, bool, std::atomic_bool &) { return {success}; }
};
struct Scene {
  struct Context { Jukebox jukebox; } context;
  std::atomic_bool launchCancelled_ = false;
  int aborted = 0;
  int completions = 0;
  void run() {
    auto chart = std::make_unique<Chart>();
    auto &cancelled = launchCancelled_;
    auto resetLaunching = [&] { ++aborted; };
    STAGING_BLOCK
    ++completions;
  }
};
int main() {
  Scene scene;
  scene.context.jukebox.success = false;
  scene.run();
  assert(scene.aborted == 1 && scene.completions == 0 &&
         "failed fallback staging must abort without posting gameplay completion");
  scene.context.jukebox.success = true;
  scene.run();
  assert(scene.aborted == 1 && scene.completions == 1);
  scene.launchCancelled_ = true;
  scene.run();
  assert(scene.aborted == 2 && scene.completions == 1);
}
