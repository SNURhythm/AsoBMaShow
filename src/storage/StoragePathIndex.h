#pragma once

#include "StoragePathComparison.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <vector>

namespace storage_management {
class StoragePathIndex {
public:
  explicit StoragePathIndex(std::vector<std::filesystem::path> roots,
                            StoragePathComparison comparison = StoragePathComparison::Native);
  std::vector<std::size_t> Ancestors(const std::filesystem::path &path,
                                    std::uint64_t *steps = nullptr) const;
  std::vector<std::filesystem::path> MinimalRoots() const;
private:
  struct Node {
#ifdef _WIN32
    explicit Node(StoragePathComparison comparison, std::filesystem::path parent = {})
        : path(std::move(parent)), children(StoragePathComponentLess{comparison, path}) {}
    std::filesystem::path path;
    std::map<std::filesystem::path, std::size_t, StoragePathComponentLess> children;
#else
    explicit Node(StoragePathComparison, const std::filesystem::path & = {}) {}
    std::unordered_map<std::filesystem::path, std::size_t> children;
#endif
    std::vector<std::size_t> roots;
  };
  std::vector<std::filesystem::path> roots_;
  std::vector<Node> nodes_;
};
}
