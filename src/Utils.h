#pragma once
#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#endif

#include <string>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

unsigned int parallel_worker_count(size_t n);
void parallel_for(size_t n, std::function<void(int start, int end)> f);

std::string ws2s_utf8(const std::wstring &wstr);

class Utils {
public:
  inline static std::string GameName = "AsoBMaShow";
  inline static std::string TeamName = "SNURhythm";
  static std::filesystem::path
  GetDocumentsPath(const std::filesystem::path &SubPath = "");
  static std::filesystem::path GetStoragePathRelativeToDocuments(
      const std::filesystem::path &Path, const std::filesystem::path &SubPath);
};

class threadRAII {
  std::thread th;

public:
  explicit threadRAII(std::thread &&_th);

  ~threadRAII();
};
