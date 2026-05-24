#include "UniformCache.h"

namespace rendering {
UniformCache &UniformCache::getInstance() {
  static UniformCache instance;
  return instance;
}

bgfx::UniformHandle UniformCache::getSampler(const std::string &name) {
  auto it = samplers_.find(name);
  if (it != samplers_.end()) {
    return it->second;
  }
  auto handle = bgfx::createUniform(name.c_str(), bgfx::UniformType::Sampler);
  samplers_.emplace(name, handle);
  return handle;
}

bgfx::UniformHandle UniformCache::getVec4(const std::string &name) {
  auto it = vec4s_.find(name);
  if (it != vec4s_.end()) {
    return it->second;
  }
  auto handle = bgfx::createUniform(name.c_str(), bgfx::UniformType::Vec4);
  vec4s_.emplace(name, handle);
  return handle;
}

void UniformCache::destroyAll() {
  for (auto &pair : samplers_) {
    if (bgfx::isValid(pair.second)) {
      bgfx::destroy(pair.second);
    }
  }
  samplers_.clear();
  for (auto &pair : vec4s_) {
    if (bgfx::isValid(pair.second)) {
      bgfx::destroy(pair.second);
    }
  }
  vec4s_.clear();
}
} // namespace rendering
