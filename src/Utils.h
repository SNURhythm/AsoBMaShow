#pragma once
#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#endif

#include <atomic>
#include <string>
#include <filesystem>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

unsigned int parallel_worker_count(size_t n);
void parallel_for(size_t n, std::function<void(int start, int end)> f);

template <typename Func>
void parallel_for_each_index(size_t n, Func &&func) {
  const unsigned int workerThreads = parallel_worker_count(n);
  if (workerThreads == 0) {
    return;
  }

  if (workerThreads <= 1) {
    for (size_t i = 0; i < n; ++i) {
      func(i);
    }
    return;
  }

  auto &&work = std::forward<Func>(func);
  std::atomic_size_t nextIndex{0};
  std::vector<std::thread> threads;
  threads.reserve(workerThreads);

  for (unsigned int i = 0; i < workerThreads; ++i) {
    threads.emplace_back([&]() {
      for (;;) {
        const size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
        if (index >= n) {
          return;
        }
        work(index);
      }
    });
  }

  for (auto &thread : threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

std::string ws2s_utf8(const std::wstring &wstr);

class Utils {
public:
  inline static std::string GameName = "AsoBMaShow";
  inline static std::string TeamName = "SNURhythm";
  static std::filesystem::path
  GetDocumentsPath(const std::filesystem::path &SubPath = "");
  static std::filesystem::path GetStoragePathRelativeToDocuments(
      const std::filesystem::path &Path, const std::filesystem::path &SubPath);
  static std::string GetStoragePathUtf8RelativeToDocuments(
      const std::filesystem::path &Path, const std::filesystem::path &SubPath);
  static bool EnsureDirectoryExists(const std::filesystem::path &Path,
                                    std::error_code &Error);
};

class threadRAII {
  std::thread th;

public:
  explicit threadRAII(std::thread &&_th);

  ~threadRAII();
};
