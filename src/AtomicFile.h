#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace atomic_file {
struct Operations {
  std::function<bool(const std::filesystem::path &, std::span<const std::byte>,
                     std::string &)>
      writeAndSync;
  std::function<bool(const std::filesystem::path &,
                     const std::filesystem::path &, std::string &)>
      replace;
  std::function<void(const std::filesystem::path &)> remove;
};

Operations defaultOperations();

bool writeWithBackup(const std::filesystem::path &path,
                     std::span<const std::byte> contents,
                     std::string &errorMessage,
                     const Operations *operations = nullptr);
} // namespace atomic_file
