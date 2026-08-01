#pragma once

#include <bgfx/bgfx.h>
#include <string>
#include <unordered_map>

namespace rendering {
class UniformCache {
public:
  static UniformCache &getInstance();

  UniformCache(const UniformCache &) = delete;
  UniformCache &operator=(const UniformCache &) = delete;

  bgfx::UniformHandle getSampler(const std::string &name);
  bgfx::UniformHandle getVec4(const std::string &name);
  void destroyAll();

private:
  UniformCache() = default;
  std::unordered_map<std::string, bgfx::UniformHandle> samplers_;
  std::unordered_map<std::string, bgfx::UniformHandle> vec4s_;
};
} // namespace rendering
