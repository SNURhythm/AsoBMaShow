"""Run streak UI callbacks through the production Scene deferred queue."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler


class StreakSceneTests(unittest.TestCase):
    def test_actions_complete_once_and_allow_the_next_action(self):
        root = Path(__file__).resolve().parents[1]
        scene = (root / "src/scene/Scene.h").read_text()
        streak = (root / "src/scene/StreakScene.cpp").read_text()
        methods = "\n".join(extract(scene, signature) for signature in (
            "void defer(", "void handleDeferred()"))
        later = extract(streak, "void StreakScene::later(")
        fixture = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>
using Uint64 = std::uint64_t;
Uint64 SDL_GetTicks() { return 100; }
namespace ui { void invalidate() {} }
class StreakScene {
 public:
  struct { Uint64 currentFrame = 0; } context;
  std::map<Uint64, std::pair<Uint64, std::vector<std::function<bool()>>>> deferred;
  std::mutex postedDeferredMutex_;
  std::vector<std::function<bool()>> postedDeferred_;
  bool isDead = false;
  int builds = 0;
  void build() { ++builds; }
  void later(std::function<void()>);
  METHODS
};
LATER
int main() {
 StreakScene scene;
 int first = 0, second = 0;
 scene.later([&] { ++first; });
 scene.handleDeferred();
 assert(first == 0);
 ++scene.context.currentFrame;
 scene.handleDeferred();
 assert(first == 1 && scene.builds == 1);
 scene.later([&] { ++second; });
 ++scene.context.currentFrame;
 scene.handleDeferred();
 scene.handleDeferred();
 assert(first == 1 && second == 1 && scene.builds == 2);
 assert(scene.deferred.empty());
}
'''.replace("METHODS", methods).replace("LATER", later)
        with tempfile.TemporaryDirectory(prefix="asobmashow-streak-ui-") as directory:
            source = Path(directory) / "streak.cpp"
            binary = Path(directory) / "streak"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build([source], binary, source.parent)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
