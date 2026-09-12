#include "skin/beatoraja/SkinMovieCatalog.h"
#include "rendering/ShaderManager.h"
#include "rendering/UniformCache.h"
#include "rendering/common.h"
#include "skin_movie_clock_video_fixture.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>

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
using namespace std::chrono_literals;

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void drainFrames() {
  for (int frame = 0; frame < 4; ++frame) {
    bgfx::frame();
  }
}

struct Movie {
  std::shared_ptr<skin::SkinMovieDevice> device = skin::createSkinMovieDevice();
  skin::SkinMovieLoadResult loaded;
  skin::PlaySkinViewport viewport{
      .drawableAuthoredBounds = {0, 0, 64, 64},
      .safeUiBounds = {0, 0, 64, 64},
      .valid = true};

  explicit Movie(const std::filesystem::path &path) {
    require(device && device->ownsCurrentThread(),
            "production movie device must own the render thread");
    const auto result = device->load(
        path, {.maximumDimension = 16,
               .maximumRgbaBytes = 1024,
               .maximumDecodedBytes = 16 * 1024}, {});
    require(result.has_value(), "real FFmpeg movie must load within 16 KiB");
    loaded = *result;
    require(loaded.width == 16 && loaded.height == 16 &&
                loaded.durationMillis == 1000 && loaded.decodedBytes > 0 &&
                loaded.decodedBytes <= 16 * 1024,
            "fixture must be a bounded one-second 16x16 movie");
  }

  ~Movie() {
    device->discardFrame();
    device->destroy(loaded.handle);
  }

  bool present(std::int64_t sourceMillis, bool submit = true) {
    device->beginFrame();
    skin::SkinMovieCommand command{
        .resource = 1,
        .sourceTimeMillis = sourceMillis,
        .geometry = {.rect = {0, 0, 64, 64}}};
    const auto prepared = device->prepareFrame(loaded.handle, command, viewport);
    require(prepared.ready, "production movie preparation must be ready");
    if (prepared.drawable && submit) {
      device->commitFrame();
      device->submitPrepared(0);
    }
    device->discardFrame();
    bgfx::frame();
    return prepared.drawable;
  }

  void expectFrame(std::int64_t sourceMillis, std::int64_t expectedMicros) {
    const auto previousUploads = movie_clock_fixture::observation.uploads;
    const auto deadline = std::chrono::steady_clock::now() + 250ms;
    do {
      const bool drawable = present(sourceMillis);
      const auto observed = movie_clock_fixture::observation;
      if (drawable && observed.uploads > previousUploads && observed.validMarker &&
          observed.displayedMicros == expectedMicros) {
        std::cout << "source_ms=" << sourceMillis
                  << " uploaded_pts_us=" << observed.displayedMicros
                  << " uploads=" << observed.uploads << '\n';
        return;
      }
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    const auto observed = movie_clock_fixture::observation;
    throw std::runtime_error(
        "source_ms=" + std::to_string(sourceMillis) +
        " expected_uploaded_pts_us=" + std::to_string(expectedMicros) +
        " actual_uploaded_pts_us=" + std::to_string(observed.displayedMicros) +
        " new_uploads=" + std::to_string(observed.uploads - previousUploads));
  }
};

void testThreeWraps(const std::filesystem::path &path, bool delayed) {
  Movie movie(path);
  if (delayed) {
    std::this_thread::sleep_for(1200ms);
  }
  for (std::int64_t loop = 0; loop < 4; ++loop) {
    for (std::int64_t frame = 0; frame < 20; ++frame) {
      movie.expectFrame(loop * 1000 + frame * 50 + 25, frame * 50'000);
    }
  }
}

void testBackwardSeekAndHeldSource(const std::filesystem::path &path) {
  Movie movie(path);
  movie.present(0);
  movie.expectFrame(325, 300'000);
  movie.expectFrame(625, 600'000);
  movie.expectFrame(125, 100'000);
  movie.expectFrame(225, 200'000);
  const auto deadline = std::chrono::steady_clock::now() + 150ms;
  do {
    movie.present(225, false);
    require(movie_clock_fixture::observation.displayedMicros == 200'000,
            "held source time must not advance the uploaded movie frame");
    std::this_thread::sleep_for(2ms);
  } while (std::chrono::steady_clock::now() < deadline);
  movie.expectFrame(325, 300'000);
}

void testCancelledLoadAndCleanup(const std::filesystem::path &path) {
  drainFrames();
  const auto baselineTextures = bgfx::getStats()->numTextures;
  auto device = skin::createSkinMovieDevice();
  std::stop_source cancelled;
  cancelled.request_stop();
  require(!device->load(path, {}, cancelled.get_token()),
          "pre-cancelled real-device load must not retain a decoder");
  require(!device->load(path, {.maximumDecodedBytes = 1}, {}),
          "real-device load must reject an insufficient decoded budget");
  for (int iteration = 0; iteration < 3; ++iteration) {
    {
      Movie movie(path);
      movie.present(0, false);
      movie.present(325, false);
      movie.device->discardFrame();
    }
    drainFrames();
    require(bgfx::getStats()->numTextures == baselineTextures,
            "destroy after discarded preparation must release YUV textures");
  }
  device.reset();
  drainFrames();
  require(bgfx::getStats()->numTextures == baselineTextures,
          "cancelled and rejected loads must not leak bgfx textures");
}
}

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: skin_movie_clock_tests CLIP SCENARIO\n";
    return 2;
  }
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  if (!bgfx::init(init)) {
    std::cerr << "headless bgfx initialization failed\n";
    return 2;
  }
  rendering::PosTexCoord0Vertex::init();
  rendering::PosColorVertex::init();
  rendering::PosTexVertex::init();
  int result = 0;
  try {
    const std::string scenario = argv[2];
    if (scenario == "immediate" || scenario == "delayed") {
      testThreeWraps(argv[1], scenario == "delayed");
    } else if (scenario == "seek") {
      testBackwardSeekAndHeldSource(argv[1]);
    } else if (scenario == "cleanup") {
      testCancelledLoadAndCleanup(argv[1]);
    } else {
      throw std::runtime_error("unknown scenario: " + scenario);
    }
    std::cout << "PASS " << scenario << '\n';
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    result = 1;
  }
  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  drainFrames();
  bgfx::shutdown();
  return result;
}
