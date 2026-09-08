#include <cassert>
#include <functional>
#include <string>

struct ChartMetaRecord { int identity = 17; };
struct ModernChartResultRecord { int identity = 42; };
struct Modal { void setStatus(const char *) {} };
struct Callbacks {
  std::function<void(const ChartMetaRecord &, const ModernChartResultRecord &)> gbattle;
  std::function<void(const ChartMetaRecord &, const ModernChartResultRecord &)> recallModernChart;
};

struct MusicSelectScene {
  Modal modal;
  Modal *recordsModal_ = &modal;
  int ghostChart = 0;
  int ghostResult = 0;
  int recalledChart = 0;
  int recalledResult = 0;
  void launchChartGhostBattle(const ChartMetaRecord &record,
                              const ModernChartResultRecord &modern) {
    ghostChart = record.identity;
    ghostResult = modern.identity;
  }
  void recallChartResult(const ChartMetaRecord &record,
                         const ModernChartResultRecord &modern) {
    recalledChart = record.identity;
    recalledResult = modern.identity;
  }
  Callbacks callbacks() {
    Callbacks callbacks;
    SCENE_CALLBACKS
    return callbacks;
  }
};

int main() {
  MusicSelectScene scene;
  const auto callbacks = scene.callbacks();
  callbacks.gbattle({}, {});
  assert(scene.ghostChart == 17 && scene.ghostResult == 42 &&
         "G-BATTLE must dispatch the selected chart and saved result, not a toast");
  callbacks.recallModernChart({}, {});
  assert(scene.recalledChart == 17 && scene.recalledResult == 42 &&
         "Result must dispatch the selected chart and saved result, not a toast");
}
