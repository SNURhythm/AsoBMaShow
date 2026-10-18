#include "StoragePathIndex.h"

#include <algorithm>

namespace storage_management {
namespace fs = std::filesystem;
namespace {
bool hasParentTraversal(const fs::path &path) {
  for (const auto &part : path) if (part == "..") return true;
  return false;
}
fs::path normalize(fs::path path) {
  path = path.lexically_normal();
  while (path != path.root_path() && path.filename().empty()) path = path.parent_path();
  return path == "." ? fs::path{} : path;
}
}
StoragePathIndex::StoragePathIndex(std::vector<fs::path> roots, StoragePathComparison comparison)
    : roots_(std::move(roots)) {
  nodes_.emplace_back(comparison);
  for (std::size_t index = 0; index < roots_.size(); ++index) {
    if (hasParentTraversal(roots_[index])) continue;
    auto &root = roots_[index];
    root = normalize(std::move(root));
    std::size_t node = 0;
    for (const auto &part : root) {
      const auto found = nodes_[node].children.find(part);
      if (found != nodes_[node].children.end()) node = found->second;
      else {
        const auto next = nodes_.size();
        nodes_[node].children.emplace(part, next);
        nodes_.emplace_back(comparison);
        node = next;
      }
    }
    nodes_[node].roots.push_back(index);
  }
}
std::vector<std::size_t> StoragePathIndex::Ancestors(const fs::path &path, std::uint64_t *steps) const {
  if (hasParentTraversal(path)) return {};
  std::vector<std::size_t> result = nodes_.front().roots;
  std::size_t node = 0;
  for (const auto &part : path) {
    if (part.empty() || part == ".") continue;
    if (steps) ++*steps;
    const auto found = nodes_[node].children.find(part);
    if (found == nodes_[node].children.end()) break;
    node = found->second;
    result.insert(result.end(), nodes_[node].roots.begin(), nodes_[node].roots.end());
  }
  return result;
}
std::vector<fs::path> StoragePathIndex::MinimalRoots() const {
  std::vector<fs::path> result;
  std::vector<std::size_t> pending{0};
  while (!pending.empty()) {
    const auto node = pending.back();
    pending.pop_back();
    if (!nodes_[node].roots.empty()) result.push_back(roots_[nodes_[node].roots.front()]);
    else for (const auto &[part, child] : nodes_[node].children) pending.push_back(child);
  }
  std::ranges::sort(result);
  return result;
}
}
