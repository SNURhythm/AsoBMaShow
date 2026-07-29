#include "ArchiveFile.h"
#include "rendering/UniformCache.h"
#include "view/ImageView.h"

#include <bgfx/bgfx.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool sameColor(const Color &actual, const Color &expected) {
  return actual.r == expected.r && actual.g == expected.g &&
         actual.b == expected.b && actual.a == expected.a;
}

void writeSinglePixelPpm(const std::filesystem::path &path) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << "P6\n1 1\n255\n";
  const char pixel[] = {static_cast<char>(0x33), static_cast<char>(0x66),
                        static_cast<char>(0x99)};
  output.write(pixel, sizeof(pixel));
}

void writePpm(const std::filesystem::path &path, int width, int height) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << "P6\n" << width << ' ' << height << "\n255\n";
  const char pixel[] = {static_cast<char>(0x33), static_cast<char>(0x66),
                        static_cast<char>(0x99)};
  for (int index = 0; index < width * height; ++index) {
    output.write(pixel, sizeof(pixel));
  }
}
} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for image fade state");

  {
    ImageView image(0, 0, 100, 50);
    require(!image.fade().has_value(), "image starts without a fade");
    require(!image.scrimColor().has_value(),
            "image starts without a readability scrim");

    image.setScrimColor(Color(3, 4, 5, 96));
    require(image.scrimColor().has_value() &&
                sameColor(*image.scrimColor(), Color(3, 4, 5, 96)),
            "image stores a fixed scrim independently of fade state");

    bool useLightScrim = false;
    image.setThemedScrimColor([&useLightScrim] {
      return useLightScrim ? Color(255, 255, 255, 168)
                           : Color(5, 10, 18, 144);
    });
    require(image.scrimColor().has_value() &&
                sameColor(*image.scrimColor(), Color(5, 10, 18, 144)),
            "themed scrim evaluates immediately");
    useLightScrim = true;
    image.propagateThemeChange();
    require(image.scrimColor().has_value() &&
                sameColor(*image.scrimColor(),
                          Color(255, 255, 255, 168)),
            "themed scrim reevaluates during theme propagation");

    image.setFade(ImageFadeDirection::RightToLeft, 2.0F);
    require(image.fade().has_value() &&
                image.fade()->direction == ImageFadeDirection::RightToLeft &&
                image.fade()->strength == 1.0F,
            "image stores direction and clamps high fade strength");

    image.setFade(ImageFadeDirection::TopToBottom, 0.25F);
    require(image.fade().has_value() &&
                image.fade()->direction == ImageFadeDirection::TopToBottom &&
                image.fade()->strength == 0.25F,
            "setting fade replaces direction and strength");

    image.clearFade();
    require(!image.fade().has_value(), "clear fade restores normal rendering");

    image.clearScrimColor();
    require(!image.scrimColor().has_value(),
            "clearing scrim restores untreated image color");
  }

  {
    const std::filesystem::path sourcePath =
        "/definitely-missing/asobmashow-jacket-performance.png";
    int sourcePathNormalizations = 0;
    archive_file::setCachePathNormalizer(
        [&sourcePathNormalizations,
         sourcePath](std::filesystem::path &normalized) {
          if (normalized == sourcePath) {
            ++sourcePathNormalizations;
          }
        });
    ImageView::dropAllCache();

    ImageView image(0, 0, 100, 50);
    image.setImageAsync(fspath_to_path_t(sourcePath), true);
    const int sourceNormalizationsAfterBinding = sourcePathNormalizations;
    RenderContext renderContext;
    image.render(renderContext);
    image.render(renderContext);
    image.render(renderContext);

    archive_file::setCachePathNormalizer({});
    require(sourceNormalizationsAfterBinding == 0,
            "normal async jacket binding skips source filesystem identity");
    require(sourcePathNormalizations == 0,
            "normal async jacket polling remains source-metadata-free");
  }

#ifndef _WIN32
  {
    const std::filesystem::path fixtureRoot =
        std::filesystem::temp_directory_path() /
        ("asobmashow-image-persisted-file-thumbnail-" +
         std::to_string(getpid()) + "-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));
    std::filesystem::remove_all(fixtureRoot);
    std::filesystem::create_directories(fixtureRoot);
    const std::filesystem::path artworkPath = fixtureRoot / "jacket.ppm";
    writePpm(artworkPath, 512, 256);

    ImageView::dropAllCache();
    ImageView firstLoad(0, 0, 256, 128);
    firstLoad.setImageAsync(artworkPath.string(), false);
    const auto firstLoadDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (firstLoad.imageWidth() == 0 &&
           std::chrono::steady_clock::now() < firstLoadDeadline) {
      firstLoad.setImageAsync(artworkPath.string(), false);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(firstLoad.imageWidth() > 0,
            "ordinary artwork finishes its initial decode");

    ImageView::dropAllCache();
    ImageView coldReload(0, 0, 256, 128);
    coldReload.setImageAsync(artworkPath.string(), false);
    const bool restoredThumbnailImmediately = coldReload.imageWidth() != 0;

    const auto refreshDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (coldReload.imageWidth() != 512 &&
           std::chrono::steady_clock::now() < refreshDeadline) {
      coldReload.setImageAsync(artworkPath.string(), false);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(coldReload.imageWidth() == 512 && coldReload.imageHeight() == 256,
            "ordinary artwork cold reload finishes from its source");

    ImageView::dropAllCache();
    std::filesystem::remove_all(fixtureRoot);
    require(!restoredThumbnailImmediately,
            "ordinary jacket or banner does not restore a disk preview");
  }

  {
    const std::filesystem::path fixtureRoot =
        std::filesystem::temp_directory_path() /
        ("asobmashow-image-independent-list-artwork-" +
         std::to_string(getpid()));
    std::filesystem::remove_all(fixtureRoot);
    std::filesystem::create_directories(fixtureRoot);
    const std::filesystem::path blockedPath = fixtureRoot / "blocked.ppm";
    const std::filesystem::path readyPath = fixtureRoot / "ready.ppm";
    require(mkfifo(blockedPath.c_str(), 0600) == 0,
            "blocked list artwork fixture creates a named pipe");
    writeSinglePixelPpm(readyPath);

    ImageView::dropAllCache();
    ImageView blockedArtwork(0, 0, 8, 8);
    blockedArtwork.setImageAsync(blockedPath.string(), false);

    int writer = -1;
    const auto readerDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (writer < 0 && std::chrono::steady_clock::now() < readerDeadline) {
      writer = open(blockedPath.c_str(), O_WRONLY | O_NONBLOCK);
      if (writer < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    require(writer >= 0,
            "one list artwork decode starts reading the blocked fixture");

    ImageView readyArtwork(0, 0, 8, 8);
    readyArtwork.setImageAsync(readyPath.string(), false);
    const auto readyDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (readyArtwork.imageWidth() == 0 &&
           std::chrono::steady_clock::now() < readyDeadline) {
      readyArtwork.setImageAsync(readyPath.string(), false);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool loadedBeforeBlockedArtworkReleased =
        readyArtwork.imageWidth() == 1 && readyArtwork.imageHeight() == 1;

    close(writer);
    std::filesystem::remove_all(fixtureRoot);
    require(loadedBeforeBlockedArtworkReleased,
            "each list jacket or banner is drawn as soon as it loads");
  }

  {
    const std::filesystem::path fixtureRoot =
        std::filesystem::temp_directory_path() /
        ("asobmashow-image-priority-" + std::to_string(getpid()));
    std::filesystem::remove_all(fixtureRoot);
    std::filesystem::create_directories(fixtureRoot);
    const std::filesystem::path blockedPath = fixtureRoot / "blocked.ppm";
    const std::filesystem::path priorityPath = fixtureRoot / "priority.ppm";
    require(mkfifo(blockedPath.c_str(), 0600) == 0,
            "blocked image fixture creates a named pipe");
    writeSinglePixelPpm(priorityPath);

    ImageView::dropAllCache();
    ImageView backgroundImage(0, 0, 8, 8);
    backgroundImage.setImageAsync(blockedPath.string(), false);

    int writer = -1;
    const auto readerDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (writer < 0 && std::chrono::steady_clock::now() < readerDeadline) {
      writer = open(blockedPath.c_str(), O_WRONLY | O_NONBLOCK);
      if (writer < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    require(writer >= 0, "background image decode starts reading the fixture");

    ImageView priorityImage(0, 0, 8, 8);
    priorityImage.setImageAsync(priorityPath.string(), true);
    const auto priorityDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (priorityImage.imageWidth() == 0 &&
           std::chrono::steady_clock::now() < priorityDeadline) {
      priorityImage.setImageAsync(priorityPath.string(), true);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool priorityLoadedBeforeBackgroundReleased =
        priorityImage.imageWidth() == 1 && priorityImage.imageHeight() == 1;

    close(writer);
    std::filesystem::remove_all(fixtureRoot);
    require(priorityLoadedBeforeBackgroundReleased,
            "priority jacket decode bypasses a blocked background image");
  }

  {
    const std::filesystem::path fixtureRoot =
        std::filesystem::temp_directory_path() /
        ("asobmashow-image-priority-dedup-" + std::to_string(getpid()));
    std::filesystem::remove_all(fixtureRoot);
    std::filesystem::create_directories(fixtureRoot);
    const std::filesystem::path blockedPath = fixtureRoot / "same.ppm";
    require(mkfifo(blockedPath.c_str(), 0600) == 0,
            "priority dedup fixture creates a named pipe");

    ImageView::dropAllCache();
    ImageView listArtwork(0, 0, 8, 8);
    listArtwork.setImageAsync(blockedPath.string(), false);
    int writer = -1;
    const auto readerDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (writer < 0 && std::chrono::steady_clock::now() < readerDeadline) {
      writer = open(blockedPath.c_str(), O_WRONLY | O_NONBLOCK);
      if (writer < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    require(writer >= 0, "normal list decode starts reading the fixture");

    ImageView selectedArtwork(0, 0, 8, 8);
    selectedArtwork.setImageAsync(blockedPath.string(), true);
    const std::size_t pending =
        ImageView::pendingAsyncDecodeCountForTesting(blockedPath.string());

    close(writer);
    std::filesystem::remove_all(fixtureRoot);
    require(pending == 1,
            "priority selection reuses an ordinary in-flight decode");
  }
#endif

  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
