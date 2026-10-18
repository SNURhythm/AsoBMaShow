#pragma once

#include <algorithm>
#include <filesystem>
#ifdef _WIN32
#include <limits>
#include <system_error>
#include <windows.h>
#endif

namespace storage_management {
enum class StoragePathComparison { Native, CaseSensitive };
// Compare individual components, never string prefixes. Windows uses its
// ordinal Unicode case table rather than the process locale or ASCII folding.
inline int CompareStoragePathComponent(const std::filesystem::path &left,
                                       const std::filesystem::path &right) {
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
struct StoragePathComponentLess {
  StoragePathComparison comparison = StoragePathComparison::Native;
  bool operator()(const std::filesystem::path &left, const std::filesystem::path &right) const {
    if (comparison == StoragePathComparison::CaseSensitive) return left.compare(right) < 0;
    return CompareStoragePathComponent(left, right) < 0;
  }
};
struct StoragePathLess {
  bool operator()(const std::filesystem::path &left, const std::filesystem::path &right) const {
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                                        StoragePathComponentLess{});
  }
};
}
