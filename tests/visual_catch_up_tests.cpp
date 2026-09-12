#include "audio/Jukebox.h"
#include "rendering/ShaderManager.h"
#include "rendering/UniformCache.h"
#include "visual_catch_up_video_fixture.h"

#include <filesystem>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = 64;
int window_height = 64;
int render_width = 64;
int render_height = 64;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = 64;
int ui_view_height = 64;
}

namespace {
namespace fixture = visual_catch_up_fixture;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

class TestStream final : public audio::IBackend {
public:
  explicit TestStream(const audio::StreamRequest &request) {
    state.request = request;
    state.effectiveSampleRate = request.sampleRate == 0 ? 44100 : request.sampleRate;
    state.effectiveBufferFrames = request.bufferFrames;
  }
  bool start(std::string &) override { running = true; return true; }
  bool stop(std::string &) override { running = false; return true; }
  bool isStarted() const override { return running; }
  audio::RuntimeState runtimeState() const override { return state; }

private:
  audio::RuntimeState state;
  bool running = false;
};

class TestFactory final : public audio::IBackendFactory {
public:
  audio::Capabilities capabilities() const override { return {}; }
  std::unique_ptr<audio::IBackend>
  open(const audio::StreamRequest &request, audio::RenderCallback, void *,
       std::string &) override {
    return std::make_unique<TestStream>(request);
  }
};

void appendEvent(bms_parser::Chart &chart, long long micros, int base, int layer) {
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(1, false);
  timeline->Timing = micros;
  timeline->BgaBase = base;
  timeline->BgaLayer = layer;
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);
}

bool decoderAction(const fixture::Event &event) {
  return event.boundary == fixture::Boundary::Receive ||
         event.boundary == fixture::Boundary::Read ||
         event.boundary == fixture::Boundary::Send ||
         event.boundary == fixture::Boundary::Drain;
}

void requireNoDecoderActions(std::size_t after = 0) {
  const auto observed = fixture::events();
  require(std::none_of(observed.begin() + after, observed.end(), decoderAction),
          "a held batch or independent suspension must not admit decoder work");
}

std::vector<fixture::Event> seekEvents() {
  std::vector<fixture::Event> seeks;
  for (const auto &event : fixture::events()) {
    if (event.boundary == fixture::Boundary::Seek) {
      seeks.push_back(event);
    }
  }
  return seeks;
}

void expectSeeks(const std::vector<std::int64_t> &expected) {
  std::vector<std::int64_t> actual;
  for (const auto &event : seekEvents()) {
    actual.push_back(event.micros);
  }
  if (actual != expected) {
    std::string values;
    for (const auto micros : actual) {
      values += " " + std::to_string(micros);
    }
    throw std::runtime_error("seek commands must match the literal timeline history; got" + values);
  }
}

template <typename Present>
void expectUpload(const VideoPlayer *player, std::int64_t micros,
                  std::size_t after, Present present) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    present();
    bgfx::frame();
    const auto observed = fixture::events();
    if (std::any_of(observed.begin() + after, observed.end(), [&](const auto &event) {
          return event.boundary == fixture::Boundary::Upload &&
                 event.player == player && event.micros == micros;
        })) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  std::string seen;
  for (const auto &event : fixture::events()) {
    if (event.player == player &&
        (event.boundary == fixture::Boundary::Frame ||
         event.boundary == fixture::Boundary::Upload)) {
      seen += (event.boundary == fixture::Boundary::Frame ? " decoded=" : " uploaded=") +
              std::to_string(event.micros);
    }
  }
  throw std::runtime_error("real marker upload missing at " + std::to_string(micros) + seen);
}

void testOrderedCatchUp(const std::filesystem::path &clip, bool restore,
                        bool failFinalSeek) {
  Stopwatch stopwatch;
  Jukebox jukebox(&stopwatch, std::make_unique<TestFactory>());
  fixture::Session session;
  bms_parser::Chart chart;
  chart.Meta.Folder = clip.parent_path();
  chart.ReferencedBmpTable.emplace(1, clip.filename().string());
  chart.ReferencedBmpTable.emplace(2, clip.filename().string());
  appendEvent(chart, 100'000, 1, -1);
  appendEvent(chart, 200'000, -1, 1);
  appendEvent(chart, 300'000, 2, -1);
  appendEvent(chart, 500'000, 1, -1);
  appendEvent(chart, 800'000, -1, 2);
  std::atomic_bool cancelled = false;
  jukebox.loadVisuals(chart, cancelled);
  require(jukebox.activeMaterializedVideoPaths().size() == 2,
          "same-file IDs must preload independent video players");
  fixture::awaitWorkers(2);
  fixture::armCatchUpProbe(failFinalSeek ? 5 : 0);
  if (restore) {
    std::string error;
    require(jukebox.restorePlayback(
                {.valid = true, .active = true, .paused = true,
                 .positionMicros = 1'000'000}, error),
            "production playback restoration must succeed");
    require(jukebox.isPaused() && jukebox.getTimeMicros() == 1'000'000,
            "restoration preserves paused audio-clock authority");
  } else {
    jukebox.seekVisualsToSongTime(1'000'000);
  }
  fixture::finishCatchUpProbe();
  const auto observed = fixture::events();
  std::vector<std::int64_t> seekMicros;
  for (const auto &event : observed) {
    if (event.boundary == fixture::Boundary::Seek) {
      seekMicros.push_back(event.micros);
    }
  }
  const std::vector<std::int64_t> expectedSeekMicros{
      900'000, 700'000, 500'000, 800'000, 200'000};
  require(seekMicros == expectedSeekMicros,
          "catch-up must preserve all base-then-layer seek commands");
  require(fixture::rejectedCatchUpAdmission(),
          "multi-event catch-up must reject decoder admission at the worker barrier");
  const auto lastSeek = std::find_if(observed.rbegin(), observed.rend(), [](const auto &event) {
    return event.boundary == fixture::Boundary::Seek;
  });
  require(std::none_of(observed.begin(), lastSeek.base(), decoderAction),
          "no decoder action may run between catch-up commands");
  const auto seeks = seekEvents();
  require(seeks[0].player != seeks[1].player &&
              seeks[2].player == seeks[0].player &&
              seeks[3].player == seeks[0].player &&
              seeks[4].player == seeks[1].player,
          "shared base/layer IDs reuse players, but same-file distinct IDs do not");
  std::size_t commandIndex = 0;
  std::size_t flushes = 0;
  bool awaitingFlush = false;
  for (const auto &event : observed) {
    if (event.boundary == fixture::Boundary::Seek) {
      require(!awaitingFlush, "successful seek flushes before the next command");
      const bool failed = failFinalSeek && commandIndex == 4;
      require((event.result < 0) == failed, "only the selected seek fails");
      awaitingFlush = !failed;
      ++commandIndex;
    } else if (event.boundary == fixture::Boundary::Flush) {
      require(awaitingFlush && event.player == seeks[commandIndex - 1].player,
              "flush belongs to the immediately preceding successful seek");
      awaitingFlush = false;
      ++flushes;
    }
  }
  require(!awaitingFlush && flushes == (failFinalSeek ? 4 : 5),
          "failed seeks retain the previous decoder state without flushing");
  fixture::releaseWorkers();
  fixture::awaitFrame(seeks[0].player, 800'000);
  fixture::awaitFrame(seeks[1].player, failFinalSeek ? 700'000 : 200'000);
  std::uint64_t serial = 1;
  const auto present = [&](std::int64_t micros) {
    const auto frame = jukebox.prepareVisualFrameAt(serial++, micros, {});
    require(frame.base && frame.base->surfaceToken == 2 &&
                frame.layer && frame.layer->surfaceToken == 3,
            "final active IDs survive every ordered command including failed seeks");
    jukebox.finalizePrepared(frame);
  };
  expectUpload(seeks[0].player, 800'000, 0, [&] { present(1'000'000); });
  expectUpload(seeks[1].player, failFinalSeek ? 700'000 : 200'000, 0, [&] {
    present(failFinalSeek ? 1'525'000 : 1'000'000);
  });
  require(seekEvents().size() == 5, "presentation does not replay consumed commands");
  std::cout << "ordered catch-up: restore=" << restore
            << " injected_failure=" << failFinalSeek
            << " seeks=5 flushes=" << flushes
            << " positive rejection; decoded and uploaded final markers\n";
}

void testNestedBatches(const std::filesystem::path &clip) {
  static_assert(!std::is_copy_constructible_v<VideoPlayer::DecodeBatch>);
  static_assert(std::is_nothrow_move_constructible_v<VideoPlayer::DecodeBatch>);
  static_assert(!std::is_move_assignable_v<VideoPlayer::DecodeBatch>);
  Stopwatch stopwatch;
  VideoPlayer player(&stopwatch);
  fixture::Session session;
  std::atomic_bool cancelled = false;
  require(player.loadVideo(clip.string(), cancelled), "nested fixture loads real video");
  fixture::awaitWorkers(1);
  {
    VideoPlayer::DecodeBatch outer(player);
    std::vector<VideoPlayer::DecodeBatch> nested;
    nested.reserve(1);
    nested.emplace_back(player);
    nested.emplace_back(player);
    player.playFrom(200'000);
    fixture::releaseWorkers();
    fixture::awaitBoundary(&player, fixture::Boundary::Rejected);
    nested.pop_back();
    auto after = fixture::events().size();
    player.playFrom(300'000);
    fixture::awaitBoundary(&player, fixture::Boundary::Rejected, after);
    requireNoDecoderActions();
    nested.clear();
    player.setDecodeSuspended(true);
    player.setDecodeSuspended(false);
    after = fixture::events().size();
    player.playFrom(400'000);
    fixture::awaitBoundary(&player, fixture::Boundary::Rejected, after);
    requireNoDecoderActions();
  }
  fixture::awaitFrame(&player, 400'000);
  expectUpload(&player, 400'000, 0, [&] { player.update(); });
  const auto after = fixture::events().size();
  try {
    VideoPlayer::DecodeBatch outer(player);
    VideoPlayer::DecodeBatch inner(player);
    player.playFrom(600'000);
    throw std::logic_error("unwind both nesting contributions");
  } catch (const std::logic_error &) {
  }
  fixture::awaitFrame(&player, 600'000, after);
  expectUpload(&player, 600'000, after, [&] { player.update(); });
  std::cout << "nested batches: vector moves, partial release, suspension toggles, exception unwind\n";
}

void testSuspensionSurvivesRelease(const std::filesystem::path &clip) {
  Stopwatch stopwatch;
  VideoPlayer player(&stopwatch);
  fixture::Session session;
  std::atomic_bool cancelled = false;
  require(player.loadVideo(clip.string(), cancelled), "suspension fixture loads");
  player.setDecodeSuspended(true);
  {
    VideoPlayer::DecodeBatch batch(player);
    player.playFrom(250'000);
    fixture::releaseWorkers();
    fixture::awaitBoundary(&player, fixture::Boundary::Rejected);
  }
  const auto after = fixture::events().size();
  player.playFrom(300'000);
  fixture::awaitBoundary(&player, fixture::Boundary::Rejected, after);
  player.update();
  requireNoDecoderActions();
  {
    VideoPlayer::DecodeBatch batch(player);
    player.setDecodeSuspended(false);
  }
  fixture::awaitFrame(&player, 300'000);
  expectUpload(&player, 300'000, after, [&] { player.update(); });
  std::cout << "independent suspension: preserved across final batch release\n";
}

void testAdmissionRecheckedUnderVideoMutex(const std::filesystem::path &clip) {
  Stopwatch stopwatch;
  VideoPlayer player(&stopwatch);
  fixture::Session session;
  std::atomic_bool cancelled = false;
  require(player.loadVideo(clip.string(), cancelled), "admission race fixture loads");
  player.playFrom(200'000);
  fixture::holdNextOutputWait(&player, true);
  fixture::releaseWorkers();
  fixture::awaitHeldWait();
  {
    VideoPlayer::DecodeBatch batch(player);
    fixture::releaseHeldWait();
    fixture::awaitBoundary(&player, fixture::Boundary::Rejected);
    requireNoDecoderActions();
  }
  fixture::awaitFrame(&player, 200'000);
  std::cout << "admission race: a previously true output predicate is rechecked under videoMutex\n";
}

void testTimelineEdges(const std::filesystem::path &clip) {
  Stopwatch stopwatch;
  Jukebox jukebox(&stopwatch, std::make_unique<TestFactory>());
  fixture::Session session;
  bms_parser::Chart chart;
  chart.Meta.Folder = clip.parent_path();
  const auto eagerClip = clip.parent_path() / "eager.mp4";
  std::filesystem::copy_file(clip, eagerClip);
  chart.ReferencedBmpTable.emplace(1, "eager.mp4");
  chart.ReferencedBmpTable.emplace(3, "image.bmp");
  appendEvent(chart, 100'000, 1, -1);
  appendEvent(chart, 200'000, -1, 1);
  appendEvent(chart, 300'000, 99, -1);
  appendEvent(chart, 400'000, -1, 99);
  appendEvent(chart, 500'000, 3, -1);
  appendEvent(chart, 600'000, 99, -1);
  appendEvent(chart, 700'000, 1, -1);
  appendEvent(chart, 800'000, -1, 1);
  std::atomic_bool cancelled = false;
  jukebox.loadVisuals(chart, cancelled);
  fixture::awaitWorkers(1);
  std::filesystem::remove(eagerClip);
  jukebox.seekVisualsToSongTime(0);
  require(seekEvents().empty() && !jukebox.hasActiveVisuals(),
          "zero due events do not activate or seek a visual");
  jukebox.seekVisualsToSongTime(100'000);
  jukebox.seekVisualsToSongTime(100'000);
  expectSeeks({0});
  std::uint64_t serial = 1;
  const auto checkFrame = [&](std::int64_t micros, std::uint64_t baseToken,
                              std::uint64_t layerToken, GameplayBgaMediaKind baseKind) {
    const auto frame = jukebox.prepareVisualFrameAt(serial++, micros, {});
    require(frame.base && frame.base->surfaceToken == baseToken &&
                frame.base->mediaKind == baseKind,
            "base ID and media kind preserve missing-asset fallback");
    require(layerToken == 0 ? !frame.layer
                            : frame.layer && frame.layer->surfaceToken == layerToken,
            "layer ID preserves missing-asset fallback and backward reset");
    jukebox.finalizePrepared(frame);
  };
  checkFrame(450'000, 2, 2, GameplayBgaMediaKind::Video);
  expectSeeks({0, 250'000});
  checkFrame(650'000, 4, 2, GameplayBgaMediaKind::Image);
  expectSeeks({0, 250'000});
  checkFrame(900'000, 2, 2, GameplayBgaMediaKind::Video);
  expectSeeks({0, 250'000, 200'000, 100'000});
  checkFrame(150'000, 2, 0, GameplayBgaMediaKind::Video);
  checkFrame(900'000, 2, 2, GameplayBgaMediaKind::Video);
  expectSeeks({0, 250'000, 200'000, 100'000, 50'000,
               200'000, 700'000, 100'000});
  const auto seeks = seekEvents();
  require(std::ranges::all_of(seeks, [&](const auto &event) {
    return event.player == seeks.front().player;
  }), "shared base/layer ID keeps one independent decoder through rapid backward seeks");
  fixture::releaseWorkers();
  fixture::awaitFrame(seeks.front().player, 100'000);
  expectUpload(seeks.front().player, 100'000, 0, [&] {
    checkFrame(900'000, 2, 2, GameplayBgaMediaKind::Video);
  });
  jukebox.setVisualsSuspended(true);
  jukebox.seekVisualsToSongTime(0);
  require(seekEvents().size() == 8, "application suspension suppresses catch-up");
  jukebox.setVisualsEnabled(false);
  jukebox.setVisualsSuspended(false);
  jukebox.seekVisualsToSongTime(900'000);
  require(seekEvents().size() == 8 && !jukebox.hasActiveVisuals(),
          "disabled visuals stay disabled after independent suspension changes");
  std::cout << "timeline edges: zero/single, missing/image, shared ID, backward/rapid, eager readiness\n";
}

void testEofAndShutdown(const std::filesystem::path &clip) {
  Stopwatch stopwatch;
  VideoPlayer player(&stopwatch);
  fixture::Session session;
  std::atomic_bool cancelled = false;
  require(player.loadVideo(clip.string(), cancelled), "EOF fixture loads");
  {
    VideoPlayer::DecodeBatch batch(player);
    player.playFrom(950'000);
    fixture::releaseWorkers();
    fixture::awaitBoundary(&player, fixture::Boundary::Rejected);
  }
  fixture::awaitFrame(&player, 950'000);
  fixture::awaitBoundary(&player, fixture::Boundary::EofWait);
  const auto after = fixture::events().size();
  {
    VideoPlayer::DecodeBatch batch(player);
    player.playFrom(100'000);
    fixture::awaitBoundary(&player, fixture::Boundary::Rejected, after);
    requireNoDecoderActions(after);
  }
  fixture::awaitFrame(&player, 100'000, after);
  expectUpload(&player, 100'000, after, [&] { player.update(); });
  {
    VideoPlayer::DecodeBatch batch(player);
    cancelled = true;
    require(!player.loadVideo(clip.string(), cancelled),
            "cancelled reload joins a gated worker without destroying the guard owner");
    fixture::awaitBoundary(&player, fixture::Boundary::WorkerStopped, after);
    require(player.getFrameWidth() == 0 && player.getReservedDecodedBytes() == 0,
            "shutdown releases decoded resources while a batch is alive");
  }
  std::cout << "EOF/shutdown: EOF wake, backward decode, gated cancellation and join\n";
}

}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: visual_catch_up_tests <marker-clip>\n";
    return 2;
  }
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  if (!bgfx::init(init)) {
    return 2;
  }
  int result = 0;
  try {
    testOrderedCatchUp(argv[1], false, false);
    testOrderedCatchUp(argv[1], true, false);
    testOrderedCatchUp(argv[1], false, true);
    testNestedBatches(argv[1]);
    testSuspensionSurvivesRelease(argv[1]);
    testAdmissionRecheckedUnderVideoMutex(argv[1]);
    testTimelineEdges(argv[1]);
    testEofAndShutdown(argv[1]);
  } catch (const std::exception &error) {
    std::cerr << "visual_catch_up_tests: " << error.what() << '\n';
    result = 1;
  }
  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return result;
}
