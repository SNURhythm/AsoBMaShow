#include "AtomicFile.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  explicit TempDirectory(std::string_view label) {
    static std::atomic<unsigned long long> counter = 0;
    const auto nonce = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-atomic-file-" + std::string(label) + "-" +
            std::to_string(nonce) + "-" + std::to_string(counter++));
    std::filesystem::create_directories(path);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }

  std::filesystem::path path;
};

void writeText(const std::filesystem::path &path, std::string_view text) {
  std::ofstream output(path, std::ios::binary);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  expect(output.good(), "test file is written");
}

std::string readText(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}

void testNoReplaceRenameSuccessAndCollision() {
  TempDirectory root("rename");
  const auto source = root.path / "source";
  const auto destination = root.path / "destination";
  writeText(source, "new");
  std::string diagnostic;
  expect(atomic_file::renameNoReplaceDurably(source, destination, diagnostic) ==
             atomic_file::RenameNoReplaceResult::Renamed,
         "no-replace rename succeeds for an unused destination");
  expect(!std::filesystem::exists(source),
         "successful no-replace rename removes the source name");
  expect(readText(destination) == "new",
         "successful no-replace rename preserves bytes");

  writeText(source, "second");
  diagnostic.clear();
  expect(atomic_file::renameNoReplaceDurably(source, destination, diagnostic) ==
             atomic_file::RenameNoReplaceResult::DestinationExists,
         "no-replace rename reports destination collision");
  expect(readText(source) == "second",
         "destination collision leaves source intact");
  expect(readText(destination) == "new",
         "destination collision never overwrites existing bytes");
}

void testNoReplaceRenameMissingSource() {
  TempDirectory root("missing");
  std::string diagnostic;
  expect(atomic_file::renameNoReplaceDurably(
             root.path / "missing", root.path / "destination", diagnostic) ==
             atomic_file::RenameNoReplaceResult::Failed,
         "no-replace rename rejects a missing source");
  expect(!diagnostic.empty(), "missing-source failure has a diagnostic");
}

void testPrivateNoReplaceWritePreservesExistingBytes() {
  TempDirectory root("private-write");
  const auto destination = root.path / "snapshot";
  const std::vector<std::byte> first{std::byte{'n'}, std::byte{'e'},
                                     std::byte{'w'}};
  std::string diagnostic;
  expect(atomic_file::writePrivateNoReplace(destination, first, diagnostic) ==
             atomic_file::WriteNoReplaceResult::Written,
         "private no-replace write creates a new file");
  expect(readText(destination) == "new",
         "private no-replace write preserves written bytes");

  const std::vector<std::byte> second{std::byte{'b'}, std::byte{'a'},
                                      std::byte{'d'}};
  diagnostic.clear();
  expect(atomic_file::writePrivateNoReplace(destination, second, diagnostic) ==
             atomic_file::WriteNoReplaceResult::DestinationExists,
         "private no-replace write reports a destination collision");
  expect(readText(destination) == "new",
         "private no-replace collision never overwrites existing bytes");
}

#ifndef _WIN32
void testNoReplaceRenameCrossDeviceWhenAvailable() {
  TempDirectory root("cross-device");
  const std::filesystem::path candidate = "/dev/shm";
  struct stat sourceStatus{};
  struct stat destinationStatus{};
  if (!std::filesystem::is_directory(candidate) ||
      ::stat(root.path.c_str(), &sourceStatus) != 0 ||
      ::stat(candidate.c_str(), &destinationStatus) != 0 ||
      sourceStatus.st_dev == destinationStatus.st_dev) {
    return;
  }
  const auto source = root.path / "source";
  const auto destination = candidate / ("asobmashow-atomic-cross-device-" +
                                        std::to_string(::getpid()));
  writeText(source, "cross-device");
  std::error_code ignored;
  std::filesystem::remove(destination, ignored);
  std::string diagnostic;
  expect(atomic_file::renameNoReplaceDurably(source, destination, diagnostic) ==
             atomic_file::RenameNoReplaceResult::Failed,
         "no-replace rename reports a cross-device failure");
  expect(std::filesystem::exists(source),
         "cross-device failure leaves source intact");
  expect(!std::filesystem::exists(destination),
         "cross-device failure exposes no destination");
  std::filesystem::remove(destination, ignored);
}
#endif

} // namespace

int main() {
  testNoReplaceRenameSuccessAndCollision();
  testNoReplaceRenameMissingSource();
  testPrivateNoReplaceWritePreservesExistingBytes();
#ifndef _WIN32
  testNoReplaceRenameCrossDeviceWhenAvailable();
#endif
  if (failures != 0) {
    std::cerr << failures << " atomic file test(s) failed\n";
    return 1;
  }
  std::cout << "Atomic file tests passed\n";
  return 0;
}
