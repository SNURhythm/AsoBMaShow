#include "archive/ArchiveSourceAccess.h"
#include "archive/ArchiveReferenceRegistry.h"
#include <atomic>
#include <cassert>
#include <fstream>
#include <future>
#include <iostream>
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

int main() {
  using namespace archive_source;
  const auto local = std::filesystem::temp_directory_path() / "asobmashow-source-access-test.bin";
  { std::ofstream out(local); out << "abcdefgh"; }
  assert(resolve(local).path == local);
  assert(!resolve("@androidarchive@/missing/test.zip"));
  std::atomic_int opens{0}, closes{0};
  Registry registry([&](const std::string &uri) -> Access {
    ++opens;
    if (uri == "unavailable") return {{}, {}, "permission unavailable"};
    return {local, std::shared_ptr<void>(new int(1), [&](void *p) { delete static_cast<int *>(p); ++closes; }), {}};
  }, 2);
  const std::filesystem::path first = "@androidarchive@/one/test.zip";
  registry.registerSource(first, "content://first");
  auto active = registry.acquire(first);
  assert(active && opens == 1);
  std::vector<std::future<void>> readers;
  for (int i=0; i<8; ++i) readers.push_back(std::async(std::launch::async, [&] {
    auto source = registry.acquire(first);
    std::ifstream stream(source.path);
    char c = 0;
    stream.seekg(3); stream.get(c); assert(c == 'd');
  }));
  for (auto &reader : readers) reader.get();
  assert(opens == 1);
  for (const auto *id : {"two", "three", "four"}) {
    auto path = std::filesystem::path("@androidarchive@") / id / "test.zip";
    registry.registerSource(path, "content://" + std::string(id));
    assert(registry.acquire(path));
  }
  assert(closes >= 1);
  assert(registry.acquire(first).owner == active.owner);
  assert(registry.remove(first) == "content://first");
  assert(!registry.referencesUri("content://first"));
  assert(!registry.acquire(first));
  std::ifstream stillReadable(active.path);
  assert(stillReadable.get() == 'a');
  const int before = closes;
  active = {};
  assert(closes == before + 1);
  registry.registerSource(first, "unavailable");
  assert(!registry.acquire(first));
  registry.registerSource(first, "content://restored");
  assert(registry.acquire(first));
  setResolver([&](const auto &path) { return registry.acquire(path); });
  assert(resolve(first));
  assert(!resolve("@androidarchive@/one/../../escape.zip"));
  setResolver({});
  assert(!resolve(first));
#if defined(__linux__)
  std::atomic_int descriptorOpens{0}, descriptorCloses{0};
  Registry descriptorRegistry([&](const std::string &) -> Access {
    const int fd = ::open(local.c_str(), O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    ++descriptorOpens;
    return {std::filesystem::path("/proc/self/fd") / std::to_string(fd),
            std::shared_ptr<void>(new int(fd), [&](void *value) {
              auto descriptor = static_cast<int *>(value);
              ::close(*descriptor); delete descriptor; ++descriptorCloses;
            }), {}};
  }, 1);
  descriptorRegistry.registerSource(first, "content://descriptor");
  setResolver([&](const auto &path) { return descriptorRegistry.acquire(path); });
  {
    InputFile held(first, std::ios::binary);
    assert(held.get() == 'a');
    std::vector<std::future<void>> descriptorReaders;
    for (int n = 0; n < 8; ++n) descriptorReaders.push_back(std::async(std::launch::async, [&] {
      InputFile input(first, std::ios::binary);
      for (int i = 0; i < 100; ++i) {
        input.seekg(i % 8);
        assert(input.get() == 'a' + i % 8);
      }
    }));
    for (auto &reader : descriptorReaders) reader.get();
    assert(descriptorOpens == 1 && held.get() == 'b');
    const std::filesystem::path other = "@androidarchive@/other/test.zip";
    descriptorRegistry.registerSource(other, "content://other");
    assert(descriptorRegistry.acquire(other));
    assert(descriptorCloses == 0);
    descriptorRegistry.remove(first);
    assert(descriptorCloses == 0 && held.get() == 'c');
  }
  assert(descriptorCloses == 1);
  setResolver({});
#endif
  std::filesystem::remove(local);
  std::cout << "Archive source access tests passed\n";
}
