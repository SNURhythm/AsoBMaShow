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
  unsigned int nThreads = std::thread::hardware_concurrency();
  nThreads = nThreads > 2 ? nThreads - 2 : 1;
  unsigned int batchSize = n / nThreads;
  unsigned int remainder = n % nThreads;
  std::vector<std::thread> threads;
  for (unsigned int i = 0; i < nThreads; i++) {
    unsigned int start = i * batchSize;
    unsigned int end = start + batchSize;
    if (i == nThreads - 1) {
      end += remainder;
    }
    threads.emplace_back(f, start, end);
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
