#include "path.h"
#include "RAII.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

using CachePathNormalizer = std::function<void(std::filesystem::path &)>;
std::mutex gCachePathNormalizerMutex;
CachePathNormalizer gCachePathNormalizer;

#include "unzip_marker_methods.inc"

int main(int argc, char **argv) {
  if (argc != 6) return 2;
  const std::filesystem::path folder = argv[1];
  const std::filesystem::path source = argv[2];
  const bool complete = std::string(argv[3]) == "complete";
  const bool expectedMatch = std::string(argv[4]) == "1";
  const bool expectedError = std::string(argv[5]) == "1";
  std::error_code error = std::make_error_code(std::errc::permission_denied);
  bool matches;
  if (complete) {
    matches = unzipFolderHasMatchingCompleteMarker(folder, source, "key");
    error.clear();
  } else {
    matches = unzipFolderHasMatchingIncompleteMarker(folder, source, "key", &error);
  }
  std::cout << "validator=" << argv[3] << " matches=" << matches
            << " error=" << error.value() << " (" << error.message() << ")"
            << " expected_match=" << expectedMatch
            << " expected_io_error=" << expectedError << '\n';
  const bool correctError = expectedError ? error == std::errc::io_error : !error;
  return matches == expectedMatch && correctError ? 0 : 1;
}
