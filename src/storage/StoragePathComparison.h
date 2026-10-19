#pragma once

#include <algorithm>
#include <filesystem>
#ifdef _WIN32
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <vector>
#include <windows.h>
#endif

namespace storage_management {
enum class StoragePathComparison { Native, CaseSensitive };
// The folded order is always primary; directory sensitivity only breaks ties.
// Thus sampling sensitivity on the first differently spelled alias cannot
// change the ordering of keys already compared without that information.
inline int CompareStoragePathComponentOrdinal(const std::filesystem::path &left,
                                              const std::filesystem::path &right) {
  if (left == right) return 0;
#ifdef _WIN32
  const auto a = std::filesystem::path(left).make_preferred().native();
  const auto b = std::filesystem::path(right).make_preferred().native();
  if (a.size() > (std::numeric_limits<int>::max)() || b.size() > (std::numeric_limits<int>::max)())
    throw std::system_error(std::make_error_code(std::errc::filename_too_long));
  const auto compared = CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                             b.data(), static_cast<int>(b.size()), TRUE);
  if (!compared) throw std::system_error(GetLastError(), std::system_category());
  return compared - CSTR_EQUAL;
#else
  return left.compare(right);
#endif
}
#ifdef _WIN32
inline bool StorageDirectoryIgnoresCase(const std::filesystem::path &parent) {
  const auto directory = parent.empty() ? std::filesystem::path(L".") : parent;
  const auto handle = CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  // FileCaseSensitiveInfo (23) is hidden by SDK headers when targeting older
  // Windows. Older/unsupported filesystems fail this query and stay exact.
  FILE_CASE_SENSITIVE_INFO info{};
  const bool read = GetFileInformationByHandleEx(handle,
      static_cast<FILE_INFO_BY_HANDLE_CLASS>(23), &info, sizeof(info));
  CloseHandle(handle);
  return read && !(info.Flags & 1); // FILE_CS_FLAG_CASE_SENSITIVE_DIR
}
inline bool StorageNamespaceComponent(const std::filesystem::path &parent,
                                      const std::filesystem::path &component) {
  // Drive/server names and UNC share names are Windows namespace components,
  // not entries in a directory whose case sensitivity can be queried.
  return (parent.empty() && component.has_root_name()) ||
      (parent == parent.root_path() && parent.root_name().native().starts_with(L"\\\\"));
}
#endif
struct StoragePathComponentLess {
  StoragePathComparison comparison = StoragePathComparison::Native;
  explicit StoragePathComponentLess(StoragePathComparison mode = StoragePathComparison::Native,
                                    std::filesystem::path parent = {}) : comparison(mode) {
#ifdef _WIN32
    if (mode == StoragePathComparison::Native) state = std::make_shared<State>(std::move(parent));
#endif
  }
  int Compare(const std::filesystem::path &left, const std::filesystem::path &right) const {
    if (comparison == StoragePathComparison::CaseSensitive) return left.compare(right);
    const auto folded = CompareStoragePathComponentOrdinal(left, right);
#ifdef _WIN32
    if (folded || left == right || StorageNamespaceComponent(state->parent, left)) return folded;
    const auto exact = std::filesystem::path(left).make_preferred().compare(
        std::filesystem::path(right).make_preferred());
    if (!exact) return 0;
    std::lock_guard lock(state->mutex);
    if (!state->ignoreCase) state->ignoreCase = StorageDirectoryIgnoresCase(state->parent);
    return *state->ignoreCase ? 0 : exact;
#else
    return folded;
#endif
  }
  bool operator()(const std::filesystem::path &left, const std::filesystem::path &right) const {
    return Compare(left, right) < 0;
  }
#ifdef _WIN32
private:
  struct State {
    explicit State(std::filesystem::path path) : parent(std::move(path)) {}
    std::filesystem::path parent;
    std::mutex mutex;
    std::optional<bool> ignoreCase;
  };
  std::shared_ptr<State> state;
#endif
};
inline int CompareStoragePathComponent(const std::filesystem::path &left,
                                       const std::filesystem::path &right,
                                       const std::filesystem::path &parent = {}) {
  const auto folded = CompareStoragePathComponentOrdinal(left, right);
#ifdef _WIN32
  if (folded || left == right || StorageNamespaceComponent(parent, left)) return folded;
  const auto exact = std::filesystem::path(left).make_preferred().compare(
      std::filesystem::path(right).make_preferred());
  return !exact || StorageDirectoryIgnoresCase(parent) ? 0 : exact;
#else
  return folded;
#endif
}
struct StoragePathLess {
  bool operator()(const std::filesystem::path &left, const std::filesystem::path &right) const {
#ifdef _WIN32
    std::lock_guard lock(context->mutex);
    auto a = left.begin(), b = right.begin();
    std::size_t node = 0;
    while (a != left.end() && b != right.end()) {
      const auto compared = context->nodes[node].less.Compare(*a, *b);
      if (compared) return compared < 0;
      const auto part = *a;
      ++a; ++b;
      if (a == left.end() || b == right.end()) break;
      const auto found = context->nodes[node].children.find(part);
      if (found != context->nodes[node].children.end()) node = found->second;
      else {
        const auto next = context->nodes.size();
        auto prefix = context->nodes[node].path / part;
        context->nodes[node].children.emplace(part, next);
        context->nodes.emplace_back(std::move(prefix));
        node = next;
      }
    }
    return a == left.end() && b != right.end();
#else
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                                        StoragePathComponentLess{});
#endif
  }
#ifdef _WIN32
private:
  // Each container owns its snapshot; copies share it so comparator copies keep
  // the same ordering. This trie unifies parent aliases before caching their
  // sensitivity, without retaining assets or using a process-wide stale cache.
  struct Node {
    explicit Node(std::filesystem::path prefix) : path(std::move(prefix)),
        less(StoragePathComparison::Native, path), children(less) {}
    std::filesystem::path path;
    StoragePathComponentLess less;
    std::map<std::filesystem::path, std::size_t, StoragePathComponentLess> children;
  };
  struct Context {
    Context() { nodes.emplace_back(std::filesystem::path{}); }
    std::mutex mutex;
    std::vector<Node> nodes;
  };
  std::shared_ptr<Context> context = std::make_shared<Context>();
#endif
};
}
