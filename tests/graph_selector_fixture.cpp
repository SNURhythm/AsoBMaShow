#include "graph_allocation_guard.h"
#include "CoursePlaySession.h"
#include "scene/play/PlayfieldChartVisualModel.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

enum class PausePoint { None, Parsed, ModelBuilt };

struct WorkerControl {
  std::mutex mutex;
  std::condition_variable condition;
  PausePoint pausePoint = PausePoint::None;
  bool reached = false;
  bool released = false;
  std::atomic_size_t rejectedBytes = 0;
  std::atomic_bool unexpectedException = false;
  std::vector<std::thread> threads;

  void pauseAt(PausePoint point) {
    std::unique_lock lock(mutex);
    if (pausePoint != point) return;
    reached = true;
    condition.notify_all();
    require(condition.wait_for(lock, std::chrono::seconds(3),
                               [this] { return released; }),
            "selector fixture worker barrier timed out");
  }

  void arm(PausePoint point) {
    std::scoped_lock lock(mutex);
    pausePoint = point;
    reached = false;
    released = false;
    rejectedBytes = 0;
    unexpectedException = false;
  }

  void waitUntilReached() {
    std::unique_lock lock(mutex);
    require(condition.wait_for(lock, std::chrono::seconds(3),
                               [this] { return reached; }),
            "selector fixture did not reach its real worker boundary");
  }

  void release() {
    std::scoped_lock lock(mutex);
    released = true;
    condition.notify_all();
  }

  void join() {
    for (auto &thread : threads) thread.join();
    threads.clear();
    require(rejectedBytes.load() == 0,
            "selector attempted an oversized ordinary allocation");
    require(!unexpectedException.load(), "selector worker escaped unexpectedly");
  }
} workerControl;

class GraphTestThread {
public:
  template <typename Callback>
  explicit GraphTestThread(Callback callback)
      : thread_([callback = std::move(callback)] {
          graph_test::AllocationGuard guard;
          try {
            callback();
          } catch (...) {
            workerControl.unexpectedException = true;
          }
          workerControl.rejectedBytes = graph_test::rejectedAllocationBytes;
        }) {}

  void detach() { workerControl.threads.push_back(std::move(thread_)); }

private:
  std::thread thread_;
};

PlayfieldChartVisualModel buildSelectorFixtureModel(
    const bms_parser::Chart &chart, int longNoteMode) {
  auto model = buildPlayfieldChartVisualModel(chart, longNoteMode);
  workerControl.pauseAt(PausePoint::ModelBuilt);
  return model;
}

}

namespace play_options {
std::unique_ptr<bms_parser::Chart> parseChart(
    const std::filesystem::path &path, std::atomic_bool &cancelled,
    const char *) {
  bms_parser::Parser parser;
  bms_parser::Chart *parsed = nullptr;
  parser.Parse(path, &parsed, false, false, cancelled);
  std::unique_ptr<bms_parser::Chart> result(parsed);
  workerControl.pauseAt(PausePoint::Parsed);
  return result;
}
}

namespace skin {
enum class MusicSelectBarKind { Song };
}

class MusicSelectScene {
public:
  struct SelectedChartAnalysis {
    std::atomic_bool cancelled{false};
    std::atomic_bool finished{false};
    std::uint64_t generation = 0;
    std::mutex mutex;
    std::shared_ptr<const SkinGameplayChartGraphState> result;
  };
  struct ChartRecord {
    bms_parser::ChartMeta meta;
  };
  struct Row {
    skin::MusicSelectBarKind kind = skin::MusicSelectBarKind::Song;
    struct { bool exists = true; } presentation;
    std::shared_ptr<ChartRecord> chart = std::make_shared<ChartRecord>();
  };
  struct Bars {
    std::size_t selectedIndex = 0;
    Row row;
    std::size_t rowCount() const { return 1; }
    const Row &rowAt(std::size_t) const { return row; }
    Bars readView() const { return *this; }
  } bars_;
  struct {
    struct { std::string selectedLnMode = "LN"; } settings;
  } context;
  std::shared_ptr<SelectedChartAnalysis> selectedChartAnalysis_;
  std::shared_ptr<const SkinGameplayChartGraphState> selectedChartInformation_;
  std::uint64_t selectedChartAnalysisGeneration_ = 1;
  bool selectedChartAnalysisStarted_ = false;
  bool launching_ = false;
  long long songBarChangeMicros_ = 0;
  long long clockMicros = 350'001;

  explicit MusicSelectScene(const char *filename) {
    bars_.row.chart->meta.BmsPath =
        std::filesystem::path(GRAPH_FIXTURE_ROOT) / filename;
  }

  long long elapsedMicros() const { return clockMicros; }
  void cancelSelectedChartAnalysis();
  void updateSelectedChartAnalysis();
};

ASOBMS_GRAPH_SELECTOR_METHODS

namespace {

void testPublication(const char *filename, bool distant) {
  workerControl.arm(PausePoint::None);
  MusicSelectScene scene(filename);
  scene.clockMicros = 350'000;
  scene.updateSelectedChartAnalysis();
  require(scene.selectedChartAnalysis_ == nullptr,
          "selector debounce must not start early");
  scene.clockMicros = 350'001;
  scene.updateSelectedChartAnalysis();
  const auto mailbox = scene.selectedChartAnalysis_;
  require(mailbox != nullptr, "actual selector must launch analysis");
  workerControl.join();
  require(mailbox->finished.load() && mailbox->result != nullptr,
          "selector must produce a chart graph rather than swallow allocation failure");
  const auto produced = mailbox->result;
  scene.updateSelectedChartAnalysis();
  require(scene.selectedChartInformation_ == produced &&
              !scene.selectedChartInformation_->bpmSeries.empty(),
          "actual selector must publish the completed current graph");
  require(scene.selectedChartInformation_->normalDistribution.empty() == distant,
          "selector must omit only overlong distributions");
  if (distant) {
    require(scene.selectedChartInformation_->judgementDistributionSeconds >=
                240'000'000,
            "selector graph must preserve logical duration despite empty bins");
  }
}

void testCancelled(PausePoint point) {
  workerControl.arm(point);
  MusicSelectScene scene("graph_short_timeline.bms");
  auto previous = std::make_shared<SkinGameplayChartGraphState>();
  scene.selectedChartInformation_ = previous;
  scene.updateSelectedChartAnalysis();
  const auto mailbox = scene.selectedChartAnalysis_;
  workerControl.waitUntilReached();
  scene.cancelSelectedChartAnalysis();
  workerControl.release();
  workerControl.join();
  scene.updateSelectedChartAnalysis();
  require(mailbox->cancelled.load() && mailbox->finished.load() &&
              mailbox->result == nullptr &&
              scene.selectedChartInformation_ == previous &&
              scene.selectedChartAnalysis_ == nullptr,
          "cancelled real selector work must not publish an obsolete graph");
}

void testGenerationMismatch() {
  workerControl.arm(PausePoint::None);
  MusicSelectScene scene("graph_short_timeline.bms");
  auto previous = std::make_shared<SkinGameplayChartGraphState>();
  scene.selectedChartInformation_ = previous;
  scene.updateSelectedChartAnalysis();
  const auto mailbox = scene.selectedChartAnalysis_;
  workerControl.join();
  require(mailbox->result != nullptr, "generation fixture must finish real work");
  ++scene.selectedChartAnalysisGeneration_;
  scene.updateSelectedChartAnalysis();
  require(scene.selectedChartInformation_ == previous &&
              scene.selectedChartAnalysis_ == nullptr,
          "completed stale-generation graph must not replace current publication");
}

}

int main() {
  testPublication("graph_short_timeline.bms", false);
  testCancelled(PausePoint::Parsed);
  testCancelled(PausePoint::ModelBuilt);
  testGenerationMismatch();
  testPublication("graph_distant_timeline.bms", true);
}
