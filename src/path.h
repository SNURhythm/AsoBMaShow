#pragma once
#include <filesystem>
#include <string>
#ifdef _WIN32
#include <windows.h>
using path_t = std::wstring;
#define PATH(x) L##x
#else
using path_t = std::string;
#define PATH(x) x
#endif

inline path_t fspath_to_path_t(const std::filesystem::path &path) {
#ifdef _WIN32
  return path.wstring();
#else
  return path.string();
#endif
}

std::string path_t_to_utf8(const path_t &input);
path_t utf8_to_path_t(const std::string &input);
