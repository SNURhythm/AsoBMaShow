#include "Utils.h"
#include <codecvt>
#include <iostream>
#include <fstream>
#include "targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "iOSNatives.hpp"
#include <CoreFoundation/CoreFoundation.h>
#endif
void parallel_for(size_t n, std::function<void(int start, int end)> f) {
  if (n == 0) {
    return;
  }

  unsigned int hwThreads = std::thread::hardware_concurrency();
  if (hwThreads == 0) {
    hwThreads = 4;
  }

  // Keep headroom for render/audio/main threads to reduce frame-time spikes.
  unsigned int reservedThreads = 1;
  if (hwThreads > 8) {
    reservedThreads = 4;
  } else if (hwThreads > 4) {
    reservedThreads = 2;
  }

  unsigned int workerThreads =
      hwThreads > reservedThreads ? hwThreads - reservedThreads : 1;
  workerThreads = std::min<unsigned int>(workerThreads,
                                         static_cast<unsigned int>(n));

  if (workerThreads <= 1) {
    f(0, static_cast<int>(n));
    return;
  }

  const size_t batchSize = (n + workerThreads - 1) / workerThreads;
  std::vector<std::thread> threads;
  threads.reserve(workerThreads);

  for (unsigned int i = 0; i < workerThreads; ++i) {
    const size_t start = static_cast<size_t>(i) * batchSize;
    if (start >= n) {
      break;
    }
    const size_t end = std::min(n, start + batchSize);
    threads.emplace_back([&f, start, end]() {
      f(static_cast<int>(start), static_cast<int>(end));
    });
  }

  for (auto &t : threads) {
    t.join();
  }
}

std::string ws2s(const std::wstring &wstr) {
  return std::string().assign(wstr.begin(), wstr.end());
}

std::string ws2s_utf8(const std::wstring &wstr) {
  using convert_typeX = std::codecvt_utf8<wchar_t>;
  std::wstring_convert<convert_typeX, wchar_t> converterX;

  return converterX.to_bytes(wstr);
}

std::filesystem::path
Utils::GetDocumentsPath(const std::filesystem::path &SubPath) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return GetIOSDocumentsPath() / SubPath;
#elif PLATFORM_ANDROID
  return GetAndroidExternalFilesDir() / SubPath;
#else
#ifdef _WIN32
  static std::wstring WindowsUserDir;
  if (WindowsUserDir.empty()) {
    wchar_t *UserPath;

    // get the My Documents directory
    HRESULT Ret =
        SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &UserPath);
    if (SUCCEEDED(Ret)) {
      // make the base user dir path
      WindowsUserDir = std::wstring(UserPath);
      CoTaskMemFree(UserPath);
    }
  }
  return std::filesystem::path(WindowsUserDir) / GameName / SubPath;
#else
  // assume Unix
  return std::filesystem::path(std::getenv("HOME")) / GameName / SubPath;
#endif
#endif
}

threadRAII::threadRAII(std::thread &&_th) { th = std::move(_th); }
threadRAII::~threadRAII() {
  if (th.joinable()) {
    th.join();
  }
}
