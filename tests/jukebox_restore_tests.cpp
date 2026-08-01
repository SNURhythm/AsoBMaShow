#include "audio/AudioDeviceManager.h"
#include "audio/Jukebox.h"
#include "rendering/UniformCache.h"

#include <bgfx/bgfx.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

struct BackendControl {
  std::deque<bool> startResults;
  audio::RenderCallback renderCallback = nullptr;
  void *renderUserData = nullptr;
};

class TestStream final : public audio::IBackend {
public:
  TestStream(std::shared_ptr<BackendControl> control,
             audio::StreamRequest request)
      : control_(std::move(control)) {
    state_.request = std::move(request);
    state_.effectiveSampleRate =
        state_.request.sampleRate == 0 ? 44100 : state_.request.sampleRate;
    state_.effectiveBufferFrames = state_.request.bufferFrames;
  }

  bool start(std::string &errorMessage) override {
    const bool result =
        control_->startResults.empty() ? true : control_->startResults.front();
    if (!control_->startResults.empty()) {
      control_->startResults.pop_front();
    }
    if (!result) {
      errorMessage = "injected candidate start failure";
      return false;
    }
    running_ = true;
    return true;
  }

  bool stop(std::string &) override {
    running_ = false;
    return true;
  }

  [[nodiscard]] bool isStarted() const override { return running_; }

  [[nodiscard]] audio::RuntimeState runtimeState() const override {
    return state_;
  }

private:
  std::shared_ptr<BackendControl> control_;
  audio::RuntimeState state_;
  bool running_ = false;
};

class TestFactory final : public audio::IBackendFactory {
public:
  explicit TestFactory(std::shared_ptr<BackendControl> control)
      : control_(std::move(control)) {}

  [[nodiscard]] audio::Capabilities capabilities() const override {
    return {
        .canSelectOutputDevice = true,
        .canSelectSampleRate = true,
        .canSelectBufferFrames = true,
        .outputDevices =
            {
                {.id = "default",
                 .name = "Default",
                 .isDefault = true,
                 .sampleRates = {44100},
                 .bufferFrames = {0, 128}},
                {.id = "usb",
                 .name = "USB",
                 .sampleRates = {48000},
                 .bufferFrames = {64}},
            },
    };
  }

  std::unique_ptr<audio::IBackend> open(const audio::StreamRequest &request,
                                        audio::RenderCallback renderCallback,
                                        void *renderUserData,
                                        std::string &) override {
    control_->renderCallback = renderCallback;
    control_->renderUserData = renderUserData;
    return std::make_unique<TestStream>(control_, request);
  }

private:
  std::shared_ptr<BackendControl> control_;
};

class TemporaryVideoFixture {
public:
  TemporaryVideoFixture() {
    directory =
        std::filesystem::temp_directory_path() /
        ("asobmashow-jukebox-restore-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    std::ofstream stream(directory / "clip.mp4", std::ios::binary);
    stream << "YUV4MPEG2 W2 H2 F1:1 Ip A1:1 C420jpeg\nFRAME\n";
    constexpr char frame[] = {
        16, 16, 16, 16, static_cast<char>(128), static_cast<char>(128)};
    stream.write(frame, sizeof(frame));
    require(stream.good(), "temporary video fixture is written completely");
  }

  ~TemporaryVideoFixture() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }

  std::filesystem::path directory;
};

void populateVisualChart(bms_parser::Chart &chart, bool layer,
                         const std::filesystem::path &folder,
                         std::string resource, long long timingMicros = 0) {
  chart.Meta.Folder = folder;
  chart.ReferencedBmpTable.emplace(1, std::move(resource));

  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(1, false);
  timeline->Timing = timingMicros;
  if (layer) {
    timeline->BgaLayer = 1;
  } else {
    timeline->BgaBase = 1;
  }
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);
}

void runManagerVisualRestoreCase(bool layer, bool paused, bool candidateStarts,
                                 const std::filesystem::path &folder,
                                 std::string resource, long long snapshotMicros,
                                 bool expectVideo) {
  Stopwatch stopwatch;
  auto control = std::make_shared<BackendControl>();
  Jukebox jukebox(&stopwatch, std::make_unique<TestFactory>(control));
  bms_parser::Chart chart;
  populateVisualChart(chart, layer, folder, std::move(resource));
  std::atomic_bool cancelled = false;
  jukebox.loadVisuals(chart, cancelled);
  require(!expectVideo || jukebox.activeMaterializedVideoPaths().empty(),
          "video descriptors do not eagerly create decoder threads");

  require(jukebox.stop().success,
          "production Jukebox drains before selecting a playback rate");
  std::string rateError;
  require(jukebox.setPlaybackRate({.percent = 50}, rateError),
          "visual restore fixture selects a stopped pitch-shift rate");
  require(jukebox.play(snapshotMicros).success,
          "production Jukebox starts the visual restoration fixture");
  if (paused) {
    jukebox.pause();
  }
  jukebox.seekVisualsToSongTime(snapshotMicros);
  require(jukebox.hasActiveVisuals(),
          "fixture publishes the visual active at the snapshot");
  require(!expectVideo || jukebox.activeMaterializedVideoPaths().size() == 1,
          "first activation materializes its video descriptor");

  player_settings::AudioSettings initial;
  audio::AudioDeviceManager manager(jukebox.audioRuntime(), jukebox, initial);
  player_settings::AudioSettings candidate;
  candidate.outputDeviceId = "usb";
  candidate.requestedSampleRate = 48000;
  candidate.requestedBufferFrames = 64;
  control->startResults =
      candidateStarts ? std::deque<bool>{true} : std::deque<bool>{false, true};

  const auto result = manager.apply(candidate);
  require(result.status == (candidateStarts
                                ? audio::ApplyStatus::Applied
                                : audio::ApplyStatus::FailedRolledBack),
          "manager reaches the expected successful or rollback restore path");
  require(jukebox.hasActiveVisuals(),
          layer ? "rollback/restart reconstructs the active BMP layer"
                : "rollback/restart reconstructs the active base BGA");
  require(jukebox.isPaused() == paused,
          "visual restoration preserves the snapshot clock mode");
  require(jukebox.getTimeMicros() == snapshotMicros,
          "visual restoration preserves the exact song position");
  require(jukebox.playbackRate() == audio::PlaybackRate{.percent = 50},
          "visual restoration preserves the playback rate");
}

void testVideoMaterializationHasThreeSlotHardLimit() {
  TemporaryVideoFixture video;
  Stopwatch stopwatch;
  auto control = std::make_shared<BackendControl>();
  Jukebox jukebox(&stopwatch, std::make_unique<TestFactory>(control));
  bms_parser::Chart chart;
  chart.Meta.Folder = video.directory;
  for (int id = 1; id <= 4; ++id) {
    chart.ReferencedBmpTable.emplace(id, "clip.bmp");
    auto *measure = new bms_parser::Measure();
    auto *timeline = new bms_parser::TimeLine(1, false);
    timeline->Timing = static_cast<long long>(id - 1) * 1'000;
    timeline->BgaBase = id;
    measure->TimeLines.push_back(timeline);
    chart.Measures.push_back(measure);
  }
  std::atomic_bool cancelled = false;
  jukebox.loadVisuals(chart, cancelled);
  require(jukebox.activeMaterializedVideoPaths().empty(),
          "descriptor load keeps zero eager video players");
  require(jukebox.play(0).success, "bounded video fixture starts playback");
  for (int id = 1; id <= 4; ++id) {
    jukebox.seekVisualsToSongTime(static_cast<long long>(id - 1) * 1'000);
    require(jukebox.activeMaterializedVideoPaths().size() <= 3,
            "video materialization never exceeds three slots");
  }
}

void testManagerRestartAndRollbackRestoreProductionJukeboxVisuals() {
  const std::filesystem::path imageFolder =
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / "SDL" / "test";
  for (const bool candidateStarts : {true, false}) {
    for (const bool paused : {true, false}) {
      runManagerVisualRestoreCase(false, paused, candidateStarts, imageFolder,
                                  "sample.bmp", 1'500'000, false);
      runManagerVisualRestoreCase(true, paused, candidateStarts, imageFolder,
                                  "sample.bmp", 1'500'000, false);
    }
  }

  TemporaryVideoFixture video;
  for (const bool candidateStarts : {true, false}) {
    for (const bool paused : {true, false}) {
      runManagerVisualRestoreCase(false, paused, candidateStarts,
                                  video.directory, "clip.bmp", 500'000, true);
    }
  }
}

void testRateScaledSnapshotRestoresBgaTimeline() {
  const std::filesystem::path imageFolder =
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / "SDL" / "test";
  Stopwatch stopwatch;
  auto control = std::make_shared<BackendControl>();
  Jukebox jukebox(&stopwatch, std::make_unique<TestFactory>(control));
  bms_parser::Chart chart;
  populateVisualChart(chart, false, imageFolder, "sample.bmp", 750'000);
  std::atomic_bool cancelled = false;
  jukebox.loadVisuals(chart, cancelled);
  require(jukebox.stop().success,
          "scaled BGA fixture drains before rate mutation");

  std::string error;
  require(jukebox.setPlaybackRate({.percent = 200}, error),
          "scaled BGA fixture selects 200 percent pitch shift");
  require(jukebox.play(0).success,
          "scaled BGA fixture starts from chart time zero");
  require(!jukebox.hasActiveVisuals(),
          "the future BGA is inactive at chart time zero");
  require(control->renderCallback != nullptr &&
              control->renderUserData != nullptr,
          "Jukebox backend exposes the production render callback");

  std::vector<std::int16_t> output(22'050 * 2);
  control->renderCallback(output.data(), 22'050, 2, control->renderUserData);
  std::array<std::int16_t, 1> emptyOutput{};
  control->renderCallback(emptyOutput.data(), 0, 2, control->renderUserData);
  jukebox.pause();
  require(jukebox.getTimeMicros() == 1'000'000,
          "half a real second publishes one chart second at 200 percent");

  const audio::PlaybackSnapshot snapshot = jukebox.suspendAndDrain();
  require(snapshot.rate == audio::PlaybackRate{.percent = 200} &&
              snapshot.positionMicros == 1'000'000 && snapshot.paused,
          "snapshot retains scaled chart position, rate, and paused state");
  jukebox.seekVisualsToSongTime(0);
  require(!jukebox.hasActiveVisuals(),
          "test reset clears the future BGA before restoration");
  require(jukebox.restorePlayback(snapshot, error),
          "scaled snapshot restores through the production Jukebox");
  require(jukebox.hasActiveVisuals(),
          "restoration applies the scaled chart position to the BGA timeline");
  require(jukebox.playbackRate() == audio::PlaybackRate{.percent = 200} &&
              jukebox.getTimeMicros() == 1'000'000 && jukebox.isPaused(),
          "restoration retains rate, exact position, and paused state");
}

void testNegativeCountInKeepsBgaAtPreChartState() {
  const std::filesystem::path imageFolder =
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / "SDL" / "test";
  Stopwatch stopwatch;
  auto control = std::make_shared<BackendControl>();
  Jukebox jukebox(&stopwatch, std::make_unique<TestFactory>(control));
  bms_parser::Chart chart;
  populateVisualChart(chart, false, imageFolder, "sample.bmp");
  std::atomic_bool cancelled = false;
  jukebox.loadVisuals(chart, cancelled);

  require(jukebox.play(-2'000'000).success,
          "negative count-in starts the production Jukebox");
  require(!jukebox.hasActiveVisuals(),
          "time-zero BGA remains inactive when negative count-in starts");
  jukebox.seekVisualsToSongTime(-1'000'000);
  require(!jukebox.hasActiveVisuals(),
          "negative visual resync retains the pre-chart BGA state");
  jukebox.seekVisualsToSongTime(0);
  require(jukebox.hasActiveVisuals(),
          "time-zero BGA activates when the chart timeline begins");
}

} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for image resources");

  try {
    testManagerRestartAndRollbackRestoreProductionJukeboxVisuals();
    testVideoMaterializationHasThreeSlotHardLimit();
    testRateScaledSnapshotRestoresBgaTimeline();
    testNegativeCountInKeepsBgaAtPreChartState();
    rendering::UniformCache::getInstance().destroyAll();
    bgfx::shutdown();
    return 0;
  } catch (const std::exception &error) {
    rendering::UniformCache::getInstance().destroyAll();
    bgfx::shutdown();
    std::cerr << "jukebox_restore_tests: " << error.what() << '\n';
    return 1;
  }
}
