#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace storage_management {
class StoragePathIndex {
public:
  explicit StoragePathIndex(std::vector<std::filesystem::path> roots);
  std::vector<std::size_t> Ancestors(const std::filesystem::path &path,
                                    std::uint64_t *steps = nullptr) const;
  std::vector<std::filesystem::path> MinimalRoots() const;
private:
  struct Node {
    std::unordered_map<std::filesystem::path, std::size_t> children;
    std::vector<std::size_t> roots;
  };
  std::vector<std::filesystem::path> roots_;
  std::vector<Node> nodes_;
};
}
