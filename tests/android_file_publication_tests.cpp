#include "platform/AndroidFilePublication.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
  int failures = 0;
  const auto expect = [&](bool condition, const char *message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
  };
  auto pattern = (fs::temp_directory_path() / "android-publication-XXXXXX").string();
  const auto root = fs::canonical(::mkdtemp(pattern.data()));
  const auto owned = root / "owned";
  const auto outside = root / "outside";
  fs::create_directories(owned);
  fs::create_directories(outside);
  const std::array roots{owned};
  const auto write = [](const fs::path &path, const char *value) { std::ofstream(path) << value; };
  const auto read = [](const fs::path &path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  const auto source = owned / "staged";
  const auto destination = owned / "published";
  std::error_code error;
  write(source, "complete encoded bytes");
  struct stat before{}, after{};
  ::stat(source.c_str(), &before);
  expect(android_file::renameUnderMutationLock(source, destination, roots, error),
         "publish complete regular file without exclusive-rename support");
  ::stat(destination.c_str(), &after);
  expect(!fs::exists(source) && read(destination) == "complete encoded bytes" &&
             before.st_dev == after.st_dev && before.st_ino == after.st_ino,
         "publication preserves inode identity and bytes for journal recovery");
  write(source, "losing publisher");
  expect(!android_file::renameUnderMutationLock(source, destination, roots, error) &&
             error == std::errc::file_exists && read(destination) == "complete encoded bytes" &&
             read(source) == "losing publisher", "occupied destination is never overwritten");
  fs::remove(destination);
  fs::create_symlink(outside / "missing", destination);
  expect(!android_file::renameUnderMutationLock(source, destination, roots, error) &&
             fs::is_symlink(fs::symlink_status(destination)), "dangling destination link is occupied");
  fs::remove(destination);
  fs::create_directory(destination);
  expect(!android_file::renameUnderMutationLock(source, destination, roots, error) &&
             fs::is_directory(destination), "destination directory is occupied");
  fs::remove(destination);
  expect(!android_file::renameUnderMutationLock(source, outside / "file", roots, error) &&
             !fs::exists(outside / "file"), "unowned destination fails closed");
  write(outside / "file", "unowned source");
  expect(!android_file::renameUnderMutationLock(outside / "file", destination, roots, error) &&
             read(outside / "file") == "unowned source", "unowned source fails closed");
  fs::create_directory_symlink(outside, owned / "escape");
  expect(!android_file::renameUnderMutationLock(source, owned / "escape/file", roots, error) &&
             read(outside / "file") == "unowned source", "parent symlink cannot escape owned scope");
  fs::remove(source);
  fs::create_symlink(outside / "file", source);
  expect(!android_file::renameUnderMutationLock(source, destination, roots, error) &&
             fs::is_symlink(fs::symlink_status(source)), "source symlink is refused");
  fs::remove(source);
  fs::create_directory(source);
  expect(!android_file::renameUnderMutationLock(source, destination, roots, error) &&
             fs::is_directory(source), "source directory is refused");
  fs::remove(source);

  // Mirror the shared monitor contract: both publishers inspect and rename
  // under one lock, and exactly one complete file wins.
  write(source, "first");
  const auto second = owned / "second";
  write(second, "second");
  std::mutex monitor;
  std::array<bool, 2> published{};
  const auto publish = [&](int index, const fs::path &path) {
    std::lock_guard lock(monitor);
    std::error_code ec;
    published[index] = android_file::renameUnderMutationLock(path, destination, roots, ec);
  };
  std::thread firstThread(publish, 0, source), secondThread(publish, 1, second);
  firstThread.join(); secondThread.join();
  expect(published[0] != published[1] &&
             read(destination) == (published[0] ? "first" : "second") &&
             fs::exists(published[0] ? second : source), "competing publishers retain loser bytes");
  fs::remove_all(root);
  return failures ? 1 : 0;
}
