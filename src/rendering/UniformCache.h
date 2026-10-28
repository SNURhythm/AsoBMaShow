#pragma once

#include <bgfx/bgfx.h>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rendering {
class UniformCache {
public:
  static UniformCache &getInstance();

  UniformCache(const UniformCache &) = delete;
  UniformCache &operator=(const UniformCache &) = delete;

  bgfx::UniformHandle getSampler(std::string_view name);
  bgfx::UniformHandle getVec4(std::string_view name);
  void destroyAll();

private:
  UniformCache() = default;
  struct NameHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view name) const noexcept {
      return std::hash<std::string_view>{}(name);
    }
  };
  std::unordered_map<std::string, bgfx::UniformHandle, NameHash,
                     std::equal_to<>> samplers_;
  std::unordered_map<std::string, bgfx::UniformHandle, NameHash,
                     std::equal_to<>> vec4s_;
};
} // namespace rendering
