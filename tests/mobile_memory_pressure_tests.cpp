#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string read(const std::filesystem::path &path) {
  std::ifstream input(path);
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input) {
    throw std::runtime_error("could not read " + path.string());
  }
  return contents.str();
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

} // namespace

int main() {
  try {
    const auto root = std::filesystem::path(ASOBMASHOW_SOURCE_DIR);
    const std::string main = read(root / "src/main.cpp");
    const std::string imageView = read(root / "src/view/ImageView.cpp");
    const std::string jukebox = read(root / "src/audio/Jukebox.cpp");
    const std::string video = read(root / "src/video/VideoPlayer.cpp");

    require(main.contains("event.type == SDL_APP_LOWMEMORY"),
            "main loop must recognize SDL low-memory events");
    require(main.contains("ImageView::evictDecodedImageCache()"),
            "low-memory route must evict decoded artwork");
    require(main.contains("context.jukebox.handleMemoryPressure()"),
            "low-memory route must notify BGA resources");
    require(imageView.contains("imageCache.clearEvictable()") &&
                imageView.contains("imageDecodeCoordinator().dropAll()"),
            "artwork pressure keeps no evictable cache or stale decode work");
    require(jukebox.contains("VideoPlayer::MemoryPressureMode::PreserveActive") &&
                jukebox.contains("VideoPlayer::MemoryPressureMode::DiscardIdle"),
            "Jukebox distinguishes active and idle video pressure");
    require(video.contains("void VideoPlayer::handleMemoryPressure"),
            "VideoPlayer exposes bounded frame eviction");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mobile_memory_pressure_tests: " << error.what() << '\n';
    return 1;
  }
}
