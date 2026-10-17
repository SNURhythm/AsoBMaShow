#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace storage_test {
inline void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
class TempDirectory {
public:
  TempDirectory() {
    root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
           ("asobmashow-storage-" + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  void write(const std::filesystem::path &relative, std::string_view bytes = "fixture") {
    const auto path = root / relative;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "write fixture");
  }
  std::filesystem::path root;
};
} // namespace storage_test
