// Opt-in benchmark only; not part of CTest. See scripts/benchmark_chart_audio_preparation.py.
#include "ArchiveFile.h"
#include "audio/AudioDeviceManager.h"
#include "audio/Jukebox.h"
#include "audio/DecodedAssetCache.h"
#include "audio/PagedPreviewSource.h"
#include "utils/ParallelWork.h"
#include "rendering/UniformCache.h"

#include <archive.h>
#include <archive_entry.h>
#include <bgfx/bgfx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !TARGET_OS_ANDROID
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

#endif
#include <cstdlib>

namespace {
void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

struct BackendControl {
  std::deque<bool> startResults;
  int forcedSampleRate = 0;
  bool running = false;
  unsigned int startCalls = 0;
  audio::RenderCallback renderCallback = nullptr;
  void *renderUserData = nullptr;
  audio::RenderTimingCallback timingCallback = nullptr;
  void *timingUserData = nullptr;
};

class TestStream final : public audio::IBackend {
public:
  TestStream(std::shared_ptr<BackendControl> control,
             audio::StreamRequest request)
      : control_(std::move(control)) {
    state_.request = std::move(request);
    state_.effectiveSampleRate =
        state_.request.sampleRate == 0 ? 44100 : state_.request.sampleRate;
    if (control_->forcedSampleRate) state_.effectiveSampleRate = control_->forcedSampleRate;
    state_.effectiveBufferFrames = state_.request.bufferFrames;
  }

  bool start(std::string &errorMessage) override {
    ++control_->startCalls;
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
    control_->running = true;
    return true;
  }

  void setRenderTimingCallback(audio::RenderTimingCallback callback,
                               void *userData) override {
    control_->timingCallback = callback;
    control_->timingUserData = userData;
  }

  bool stop(std::string &) override {
    running_ = false;
    control_->running = false;
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

int benchmarkChartAudio(int argc, char **argv) {
  require(argc >= 5, "benchmark needs sample rate, rounds and chart paths");
  const int rate = std::stoi(argv[2]);
  const int rounds = std::stoi(argv[3]);
  require((rate == 44100 || rate == 48000) && rounds > 0 && rounds <= 100,
          "invalid chart benchmark arguments");
  const char *budgetText = std::getenv("ASOBMASHOW_BENCHMARK_CACHE_BYTES");
  const auto cacheBytes = budgetText ? std::stoull(budgetText) :
      audio::DecodedAssetCache::kUnlimitedRetainedBytes;
  using Clock = std::chrono::steady_clock;
  auto milliseconds = [](auto duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
  };
  for (int index = 4; index < argc; ++index) {
    const auto path = std::filesystem::path(argv[index]);
    std::atomic_bool cancelled{false};
    const auto parseStart = Clock::now();
    std::vector<unsigned char> bytes;
    std::string error;
    require(archive_file::readFileBounded(path, bytes, 16U * 1024U * 1024U, &error), error);
    bms_parser::Parser parser;
    parser.SetRandomSeed(1);
    bms_parser::Chart *raw = nullptr;
    parser.Parse(bytes, &raw, false, false, cancelled, path);
    std::unique_ptr<bms_parser::Chart> chart(raw);
    require(chart != nullptr, "chart benchmark parse failed");
    chart->Meta.BmsPath = path;
    chart->Meta.Folder = path.parent_path();
    const auto parseMs = milliseconds(Clock::now() - parseStart);
    std::cout << "CHART_INPUT case=" << index - 4 << " rate=" << rate
              << " cache_budget_bytes=" << cacheBytes
              << " definitions=" << chart->WavTable.size()
              << " references=" << chart->ReferencedWavTable.size()
              << " workers=" << parallel_worker_count(chart->ReferencedWavTable.size())
              << " parse_ms=" << parseMs << '\n';
    std::optional<std::vector<int>> expectedLoaded;
    // Rotate mode order between rounds. Round -1 warms filesystem and code;
    // every mode starts with a fresh application cache and Jukebox.
    for (int round = -1; round < rounds; ++round) {
      for (int offset = 0; offset < 3; ++offset) {
        const int mode = (round + 1 + offset) % 3;
        Stopwatch stopwatch;
        auto control = std::make_shared<BackendControl>();
        control->forcedSampleRate = rate;
        Jukebox jukebox(&stopwatch, std::make_unique<TestFactory>(control));
        jukebox.setVisualsEnabled(false);
        auto cache = std::make_shared<audio::DecodedAssetCache>(
            cacheBytes);
        std::shared_ptr<audio::PagedPreviewSource> source;
        double startupMs = 0, previewMs = 0, cancelMs = 0;
        if (mode != 0) {
          const auto previewStart = Clock::now();
          source = audio::PagedPreviewSource::Create(*chart, cache, 0);
          require(source->Begin(rate), "benchmark preview Begin failed");
          bool observedStartup = false;
          for (;;) {
            const auto state = source->GetState();
            require(state.error.empty(), state.error);
            require(!state.cancelled, "benchmark preview unexpectedly cancelled");
            if (state.startupReady && !observedStartup) {
              startupMs = milliseconds(Clock::now() - previewStart);
              observedStartup = true;
            }
            if ((mode == 2 && state.startupReady) || state.preparationComplete) break;
            require(Clock::now() - previewStart < std::chrono::seconds(60),
                    "benchmark preview preparation timed out");
            source->WaitForChange(state.revision, std::chrono::milliseconds(100));
          }
          previewMs = milliseconds(Clock::now() - previewStart);
          const auto cancelStart = Clock::now();
          source->Cancel(); // Join before capturing counters; published buffers stay alive.
          cancelMs = milliseconds(Clock::now() - cancelStart);
          jukebox.setDecodedAssetCache(cache);
        }
        const auto before = cache->GetStatistics();
        const auto loadStart = Clock::now();
        const auto loaded = mode == 0 ? jukebox.loadChart(*chart, true, cancelled) :
            jukebox.prepareGameplayAudioFromPreview(*chart, cancelled);
        const auto loadMs = milliseconds(Clock::now() - loadStart);
        require(loaded.success && !cancelled, loaded.diagnostic);
        const auto after = cache->GetStatistics();
        if (mode == 1 && cacheBytes == audio::DecodedAssetCache::kUnlimitedRetainedBytes) require(after.decodedFrames == before.decodedFrames,
                               "fully prepared handoff unexpectedly decodes playable audio again");
        std::vector<int> loadedIds;
        for (const auto &[id, name] : chart->ReferencedWavTable) {
          const auto handle = jukebox.resolveRealtimeKeySound(id);
          if (handle && handle->valid()) loadedIds.push_back(id);
        }
        require(!loadedIds.empty(), "benchmark chart has no playable gameplay sounds");
        std::sort(loadedIds.begin(), loadedIds.end());
        if (!expectedLoaded) expectedLoaded = loadedIds;
        require(loadedIds == *expectedLoaded,
                "direct and preview handoff disagree about loaded gameplay sounds");
        jukebox.setDecodedAssetCache(nullptr);
        cache->Clear();
        source.reset();
        for (const int id : loadedIds) {
          const auto handle = jukebox.resolveRealtimeKeySound(id);
          require(handle && handle->valid(), "handoff lost gameplay ownership after preview release");
        }
        std::cout << "CHART_AUDIO case=" << index - 4 << " rate=" << rate
                  << " mode=" << (mode == 0 ? "direct" : mode == 1 ? "prepared" : "early")
                  << " round=" << round << " startup_ms=" << startupMs
                  << " preview_ms=" << previewMs << " cancel_ms=" << cancelMs
                  << " gameplay_ms=" << loadMs
                  << " total_ms=" << previewMs + cancelMs + loadMs
                  << " loaded_ids=" << loadedIds.size()
                  << " cache_observed=" << (mode != 0)
                  << " preview_opens=" << before.decoderOpens
                  << " handoff_opens=" << after.decoderOpens - before.decoderOpens
                  << " handoff_decoded_frames=" << after.decodedFrames - before.decodedFrames
                  << " decoded_frames=" << after.decodedFrames
                  << " peak_decoders=" << after.peakLiveDecoders
                  << " native_pcm_bytes=" << after.retainedBytes << std::endl;
      }
    }
  }
  return 0;
}
} // namespace

#if TARGET_OS_ANDROID
// This headless executable has no Java asset manager. Only graphics setup is
// bypassed; audio preparation uses the production Android library unchanged.
bgfx::ProgramHandle Jukebox::prepareGameplayBgaProgram(const char *, const char *) noexcept {
  return BGFX_INVALID_HANDLE;
}
#endif

int main(int argc, char **argv) {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  if (!bgfx::init(init)) return 1;
  int result = 0;
  try {
    result = benchmarkChartAudio(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    result = 1;
  }
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return result;
}
