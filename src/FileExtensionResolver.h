#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace file_extension_resolver {

template <typename Lookup>
auto find(const std::filesystem::path &basePath,
          std::span<const std::string_view> extensions,
          const Lookup &lookup) {
  if (auto found = lookup(basePath)) {
    return found;
  }
  for (const auto extension : extensions) {
    auto candidate = basePath;
    candidate.replace_extension(std::string(extension));
    if (candidate == basePath) continue;
    if (auto found = lookup(candidate)) {
      return found;
    }
  }
  return decltype(lookup(basePath)){};
}

}
