#include "ArchiveFile.h"
#include "rendering/UniformCache.h"
#include "view/ImageView.h"

#include <bgfx/bgfx.h>

#include <array>
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

#ifndef _WIN32
bool writeAll(int descriptor, const char *data, std::size_t size) {
  while (size > 0) {
    const ssize_t written = write(descriptor, data, size);
    if (written <= 0) {
      return false;
    }
    data += written;
    size -= static_cast<std::size_t>(written);
  }
  return true;
}

bool writePpm(int descriptor, int width, int height) {
  const std::string header =
      "P6\n" + std::to_string(width) + ' ' + std::to_string(height) +
      "\n255\n";
  if (!writeAll(descriptor, header.data(), header.size())) {
    return false;
  }
  const std::vector<char> row(static_cast<std::size_t>(width) * 3U,
                              static_cast<char>(0x66));
  for (int y = 0; y < height; ++y) {
    if (!writeAll(descriptor, row.data(), row.size())) {
      return false;
    }
  }
  return true;
}
#endif
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

  {
    const std::filesystem::path fixtureRoot =
        std::filesystem::temp_directory_path() /
        ("asobmashow-image-evicted-ticket-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));
    std::filesystem::create_directories(fixtureRoot);
    const std::filesystem::path artworkPath = fixtureRoot / "artwork.ppm";
    const path_t imagePath = fspath_to_path_t(artworkPath);
    writeSinglePixelPpm(artworkPath);
    ImageView::dropAllCache();
    ImageView image(0, 0, 8, 8);
    image.setImageAsync(imagePath, true);

    ImageView::evictDecodedImageCache();
    const auto reloadDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (image.imageWidth() == 0 &&
           std::chrono::steady_clock::now() < reloadDeadline) {
      image.setImageAsync(imagePath, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(image.imageWidth() == 1 && image.imageHeight() == 1,
            "memory eviction lets a live image replace its stale ticket");
    std::filesystem::remove_all(fixtureRoot);
  }

  {
    const std::filesystem::path fixtureRoot =
        std::filesystem::temp_directory_path() /
        ("asobmashow-image-failed-ticket-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));
    std::filesystem::create_directories(fixtureRoot);
    const std::filesystem::path artworkPath = fixtureRoot / "artwork.ppm";
    const path_t imagePath = fspath_to_path_t(artworkPath);
    ImageView::dropAllCache();
    ImageView image(0, 0, 8, 8);
    image.setImageAsync(imagePath, true);

    const auto failureDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (ImageView::pendingAsyncDecodeCountForTesting(imagePath) != 0 &&
           std::chrono::steady_clock::now() < failureDeadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(ImageView::pendingAsyncDecodeCountForTesting(imagePath) == 0,
            "missing image decode reaches a terminal failure");

    writeSinglePixelPpm(artworkPath);
    const auto retryDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (image.imageWidth() == 0 &&
           std::chrono::steady_clock::now() < retryDeadline) {
      image.setImageAsync(imagePath, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(image.imageWidth() == 1 && image.imageHeight() == 1,
            "a failed async decode can retry when its source becomes ready");
    ImageView::dropAllCache();
    std::filesystem::remove_all(fixtureRoot);
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
    while (coldReload.imageWidth() != 256 &&
           std::chrono::steady_clock::now() < refreshDeadline) {
      coldReload.setImageAsync(artworkPath.string(), false);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(coldReload.imageWidth() == 256 && coldReload.imageHeight() == 128,
            "ordinary artwork is downsampled to its rendered dimensions");

    ImageView::dropAllCache();
    std::filesystem::remove_all(fixtureRoot);
    require(!restoredThumbnailImmediately,
            "ordinary jacket or banner does not restore a disk preview");
  }

  {
    const std::filesystem::path fixtureRoot =
        std::filesystem::temp_directory_path() /
        ("asobmashow-image-resize-refresh-" + std::to_string(getpid()));
    std::filesystem::remove_all(fixtureRoot);
    std::filesystem::create_directories(fixtureRoot);
    const std::filesystem::path artworkPath = fixtureRoot / "jacket.ppm";
    writePpm(artworkPath, 512, 256);

    ImageView::dropAllCache();
    ImageView artwork(0, 0, 64, 32);
    artwork.setImageAsync(artworkPath.string(), true);
    const auto initialDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (artwork.imageWidth() != 64 &&
           std::chrono::steady_clock::now() < initialDeadline) {
      artwork.setImageAsync(artworkPath.string(), true);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(artwork.imageWidth() == 64 && artwork.imageHeight() == 32,
            "artwork initially decodes for its compact layout");

    std::filesystem::remove(artworkPath);
    require(mkfifo(artworkPath.c_str(), 0600) == 0,
            "resize refresh fixture creates a named pipe");
    {
      View::LayoutBatchScope layoutBatch;
      artwork.setWidth(256);
      artwork.setHeight(128);
    }

    int writer = -1;
    const auto readerDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (writer < 0 && std::chrono::steady_clock::now() < readerDeadline) {
      writer = open(artworkPath.c_str(), O_WRONLY | O_NONBLOCK);
      if (writer < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    require(writer >= 0,
            "enlarging a bound image starts a higher-resolution decode");
    const int writerFlags = fcntl(writer, F_GETFL);
    require(writerFlags >= 0 &&
                fcntl(writer, F_SETFL, writerFlags & ~O_NONBLOCK) == 0,
            "resize refresh fixture enables blocking writes");
    require(writePpm(writer, 512, 256),
            "resize refresh worker receives a complete image");
    close(writer);

    const auto refreshDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (artwork.imageWidth() != 256 &&
           std::chrono::steady_clock::now() < refreshDeadline) {
      artwork.setImageAsync(artworkPath.string(), true);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(artwork.imageWidth() == 256 && artwork.imageHeight() == 128,
            "enlarged artwork replaces its compact decoded texture");

    ImageView::dropAllCache();
    std::filesystem::remove_all(fixtureRoot);
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
        ("asobmashow-image-newly-visible-first-" +
         std::to_string(getpid()));
    std::filesystem::remove_all(fixtureRoot);
    std::filesystem::create_directories(fixtureRoot);

    std::array<std::filesystem::path, 2> activePaths;
    std::array<std::unique_ptr<ImageView>, 2> activeArtwork;
    std::array<int, 2> activeWriters = {-1, -1};
    ImageView::dropAllCache();
    for (std::size_t index = 0; index < activePaths.size(); ++index) {
      activePaths[index] =
          fixtureRoot / ("active-" + std::to_string(index) + ".ppm");
      require(mkfifo(activePaths[index].c_str(), 0600) == 0,
              "active background artwork fixture creates a named pipe");
      activeArtwork[index] = std::make_unique<ImageView>(0, 0, 8, 8);
      activeArtwork[index]->setImageAsync(activePaths[index].string(), false);
    }

    for (std::size_t index = 0; index < activePaths.size(); ++index) {
      const auto readerDeadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (activeWriters[index] < 0 &&
             std::chrono::steady_clock::now() < readerDeadline) {
        activeWriters[index] =
            open(activePaths[index].c_str(), O_WRONLY | O_NONBLOCK);
        if (activeWriters[index] < 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      }
      require(activeWriters[index] >= 0,
              "all background artwork workers start their active reads");
    }

    const std::filesystem::path stalePath = fixtureRoot / "stale.ppm";
    const std::filesystem::path newlyVisiblePath =
        fixtureRoot / "newly-visible.ppm";
    require(mkfifo(stalePath.c_str(), 0600) == 0,
            "stale queued artwork fixture creates a named pipe");
    writeSinglePixelPpm(newlyVisiblePath);

    ImageView staleArtwork(0, 0, 8, 8);
    ImageView newlyVisibleArtwork(0, 0, 8, 8);
    staleArtwork.setImageAsync(stalePath.string(), false);
    newlyVisibleArtwork.setImageAsync(newlyVisiblePath.string(), true);

    const char ppm[] = "P6\n1 1\n255\n\x33\x66\x99";
    require(write(activeWriters[0], ppm, sizeof(ppm) - 1) ==
                static_cast<ssize_t>(sizeof(ppm) - 1),
            "one active worker receives a complete image");
    close(activeWriters[0]);
    activeWriters[0] = -1;

    const auto newlyVisibleDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (newlyVisibleArtwork.imageWidth() == 0 &&
           std::chrono::steady_clock::now() < newlyVisibleDeadline) {
      newlyVisibleArtwork.setImageAsync(newlyVisiblePath.string(), false);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool newlyVisibleLoadedBeforeStaleQueueBlocked =
        newlyVisibleArtwork.imageWidth() == 1 &&
        newlyVisibleArtwork.imageHeight() == 1;

    ImageView::dropAllCache();
    int staleWriter = -1;
    const auto staleReaderDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (staleWriter < 0 &&
           std::chrono::steady_clock::now() < staleReaderDeadline) {
      staleWriter = open(stalePath.c_str(), O_WRONLY | O_NONBLOCK);
      if (staleWriter < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    if (staleWriter >= 0) {
      (void)write(staleWriter, ppm, sizeof(ppm) - 1);
      close(staleWriter);
    }
    for (int &writer : activeWriters) {
      if (writer >= 0) {
        (void)write(writer, ppm, sizeof(ppm) - 1);
        close(writer);
        writer = -1;
      }
    }
    const auto drainDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (ImageView::pendingAsyncDecodeCountForTesting(stalePath.string()) !=
               0 &&
           std::chrono::steady_clock::now() < drainDeadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::filesystem::remove_all(fixtureRoot);
    require(newlyVisibleLoadedBeforeStaleQueueBlocked,
            "newly visible list artwork bypasses stale queued work");
  }

  {
    const std::filesystem::path fixtureRoot =
        std::filesystem::temp_directory_path() /
        ("asobmashow-image-visible-folder-priority-" +
         std::to_string(getpid()));
    std::filesystem::remove_all(fixtureRoot);
    std::filesystem::create_directories(fixtureRoot);

    std::array<std::filesystem::path, 5> visiblePaths;
    std::array<std::unique_ptr<ImageView>, 5> visibleArtwork;
    std::array<int, 5> writers = {-1, -1, -1, -1, -1};
    ImageView::dropAllCache();
    for (std::size_t index = 0; index < visiblePaths.size(); ++index) {
      visiblePaths[index] =
          fixtureRoot / ("visible-" + std::to_string(index) + ".ppm");
      require(mkfifo(visiblePaths[index].c_str(), 0600) == 0,
              "folder-priority fixture creates a named pipe");
      visibleArtwork[index] = std::make_unique<ImageView>(0, 0, 8, 8);
      visibleArtwork[index]->setImageAsync(visiblePaths[index].string(), true);
    }

    const auto readersDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::size_t openedCount = 0;
    while (openedCount < writers.size() &&
           std::chrono::steady_clock::now() < readersDeadline) {
      openedCount = 0;
      for (std::size_t index = 0; index < writers.size(); ++index) {
        if (writers[index] < 0) {
          writers[index] =
              open(visiblePaths[index].c_str(), O_WRONLY | O_NONBLOCK);
        }
        if (writers[index] >= 0) {
          ++openedCount;
        }
      }
      if (openedCount < writers.size()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }

    ImageView::dropAllCache();
    const char ppm[] = "P6\n1 1\n255\n\x33\x66\x99";
    for (int &writer : writers) {
      if (writer >= 0) {
        (void)write(writer, ppm, sizeof(ppm) - 1);
        close(writer);
        writer = -1;
      }
    }
    const auto drainDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool pending = true;
    while (pending && std::chrono::steady_clock::now() < drainDeadline) {
      pending = false;
      for (const auto &path : visiblePaths) {
        pending = pending ||
                  ImageView::pendingAsyncDecodeCountForTesting(path.string()) !=
                      0;
      }
      if (pending) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    std::filesystem::remove_all(fixtureRoot);
    require(openedCount == 2,
            "priority artwork obeys the hard two-worker decode limit");
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

  {
    const std::filesystem::path fixtureRoot =
        std::filesystem::temp_directory_path() /
        ("asobmashow-image-cancelled-fifo-" + std::to_string(getpid()));
    std::filesystem::remove_all(fixtureRoot);
    std::filesystem::create_directories(fixtureRoot);
    const std::array<std::filesystem::path, 2> blockedPaths = {
        fixtureRoot / "never-0.ppm", fixtureRoot / "never-1.ppm"};
    for (const auto &blockedPath : blockedPaths) {
      require(mkfifo(blockedPath.c_str(), 0600) == 0,
              "cancelled FIFO fixture creates a named pipe");
    }
    const std::filesystem::path readyPath = fixtureRoot / "ready.ppm";
    writeSinglePixelPpm(readyPath);
    ImageView::dropAllCache();
    std::array<std::unique_ptr<ImageView>, 2> blockedImages;
    std::array<int, 2> writers = {-1, -1};
    for (std::size_t index = 0; index < blockedPaths.size(); ++index) {
      blockedImages[index] = std::make_unique<ImageView>(0, 0, 8, 8);
      blockedImages[index]->setImageAsync(blockedPaths[index].string(), true);
    }
    const auto readerDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((writers[0] < 0 || writers[1] < 0) &&
           std::chrono::steady_clock::now() < readerDeadline) {
      for (std::size_t index = 0; index < blockedPaths.size(); ++index) {
        if (writers[index] < 0) {
          writers[index] = open(blockedPaths[index].c_str(), O_WRONLY | O_NONBLOCK);
        }
      }
      if (writers[0] < 0 || writers[1] < 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(writers[0] >= 0 && writers[1] >= 0,
            "cancelled FIFO decodes occupy both workers");
    for (auto &image : blockedImages) image->freeImage();
    ImageView ready(0, 0, 8, 8);
    ready.setImageAsync(readyPath.string(), true);
    const auto cancelledDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (ready.imageWidth() == 0 &&
           std::chrono::steady_clock::now() < cancelledDeadline) {
      ready.setImageAsync(readyPath.string(), true);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool workersReturnedBeforeWriterClose = ready.imageWidth() == 1;
    for (int writer : writers) close(writer);
    std::filesystem::remove_all(fixtureRoot);
    require(workersReturnedBeforeWriterClose,
            "cancelling never-producing FIFOs returns both workers promptly");
  }

  {
    const std::filesystem::path fixtureRoot =
        std::filesystem::temp_directory_path() /
        ("asobmashow-image-empty-fifo-" + std::to_string(getpid()));
    std::filesystem::remove_all(fixtureRoot);
    std::filesystem::create_directories(fixtureRoot);
    const std::array<std::filesystem::path, 2> emptyPaths = {
        fixtureRoot / "empty-0.ppm", fixtureRoot / "empty-1.ppm"};
    for (const auto &emptyPath : emptyPaths) {
      require(mkfifo(emptyPath.c_str(), 0600) == 0,
              "empty FIFO fixture creates a named pipe");
    }
    const std::filesystem::path readyPath = fixtureRoot / "ready.ppm";
    writeSinglePixelPpm(readyPath);

    ImageView::dropAllCache();
    std::array<std::unique_ptr<ImageView>, 2> emptyImages;
    std::array<int, 2> writers = {-1, -1};
    for (std::size_t index = 0; index < emptyPaths.size(); ++index) {
      emptyImages[index] = std::make_unique<ImageView>(0, 0, 8, 8);
      emptyImages[index]->setImageAsync(emptyPaths[index].string(), true);
    }
    const auto writerDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((writers[0] < 0 || writers[1] < 0) &&
           std::chrono::steady_clock::now() < writerDeadline) {
      for (std::size_t index = 0; index < emptyPaths.size(); ++index) {
        if (writers[index] < 0) {
          writers[index] =
              open(emptyPaths[index].c_str(), O_WRONLY | O_NONBLOCK);
        }
      }
      if (writers[0] < 0 || writers[1] < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    require(writers[0] >= 0 && writers[1] >= 0,
            "empty FIFO writers attach to both decode workers");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (int &writer : writers) {
      if (writer >= 0) {
        close(writer);
        writer = -1;
      }
    }

    ImageView ready(0, 0, 8, 8);
    ready.setImageAsync(readyPath.string(), true);
    const auto readyDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (ready.imageWidth() == 0 &&
           std::chrono::steady_clock::now() < readyDeadline) {
      ready.setImageAsync(readyPath.string(), true);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool workersReturnedAfterEmptyWriterDisconnect =
        ready.imageWidth() == 1 && ready.imageHeight() == 1;
    std::filesystem::remove_all(fixtureRoot);
    require(workersReturnedAfterEmptyWriterDisconnect,
            "an empty FIFO writer disconnect releases both image workers");
  }
#endif

  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
