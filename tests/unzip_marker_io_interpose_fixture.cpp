#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

struct MarkerReadCookie {
  FILE *file;
  int remaining;
};

int readMarker(void *context, char *buffer, int length) {
  auto *cookie = static_cast<MarkerReadCookie *>(context);
  if (cookie->remaining == 0) {
    std::fputs("injected marker read EIO\n", stderr);
    errno = EIO;
    return -1;
  }
  const int count = static_cast<int>(std::fread(
      buffer, 1, std::min(length, cookie->remaining), cookie->file));
  cookie->remaining -= count;
  return count;
}

int closeMarker(void *context) {
  auto *cookie = static_cast<MarkerReadCookie *>(context);
  const int result = std::fclose(cookie->file);
  delete cookie;
  return result;
}

extern "C" FILE *openMarker(const char *path, const char *mode) {
  FILE *file = std::fopen(path, mode);
  const char *target = std::getenv("ASOBMSHOW_MARKER_IO_PATH");
  const char *after = std::getenv("ASOBMSHOW_MARKER_IO_AFTER");
  if (file == nullptr || target == nullptr || after == nullptr ||
      std::strcmp(path, target) != 0 || mode[0] != 'r') return file;
  auto *cookie = new (std::nothrow) MarkerReadCookie{file, std::atoi(after)};
  if (cookie == nullptr) {
    std::fclose(file);
    errno = ENOMEM;
    return nullptr;
  }
  FILE *injected = funopen(cookie, readMarker, nullptr, nullptr, closeMarker);
  if (injected == nullptr) closeMarker(cookie);
  return injected;
}

__attribute__((used, section("__DATA,__interpose")))
static const struct {
  const void *replacement;
  const void *original;
} markerInterposition = {
    reinterpret_cast<const void *>(&openMarker),
    reinterpret_cast<const void *>(&fopen),
};
