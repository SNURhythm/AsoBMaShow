#pragma once
#include <filesystem>
#include <string>
#ifdef _WIN32
#include <windows.h>
typedef std::wstring path_t;
#define PATH(x) L##x
#else
typedef std::string path_t;
#define PATH(x) x
#endif

#ifdef _WIN32
#define fspath_to_path_t(fspath) (fspath.wstring())
#else
#define fspath_to_path_t(fspath) (fspath.string())
#endif

std::string path_t_to_utf8(const path_t &input);
path_t utf8_to_path_t(const std::string &input);
